#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <chrono>
#include <cstdio>
#include <unistd.h>

#pragma pack(push, 1)
struct ImageHeader {
    char magic[8];          // "MYEXT4\0\0"
    uint32_t version;       // 3
    uint32_t block_size;    // Например: 4096
    uint64_t total_blocks;  // Общее число блоков ФС
    uint64_t used_blocks;   // Точное число занятых блоков (для прогресс-бара)
    uint32_t header_crc32;  // CRC32 первых 32 байт этого заголовка
};

struct ChunkHeader {
    uint64_t start_block;   // Начальный блок чанка
    uint32_t block_count;   // Количество блоков подряд (0 = конец потока)
    uint32_t header_crc32;  // CRC32 полей start_block и block_count
    uint32_t data_crc32;    // CRC32 полезных данных чанка
};
#pragma pack(pop)

// Быстрый табличный CRC32 (полином 0xEDB88320, стандартный IEEE 802.3)
inline uint32_t compute_crc32(const void* data, size_t length, uint32_t initial = 0) {
    static constexpr auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = ~initial;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Вывод процентов в заданный файловый дескриптор для whiptail / dialog / zenity
class ProgressReporter {
public:
    ProgressReporter(int fd, uint64_t total_items)
        : fd_(fd), total_(total_items), last_update_(std::chrono::steady_clock::now()) {
        if (fd_ >= 0) {
            write_percent(0);
            last_pct_ = 0;
        }
    }

    ~ProgressReporter() {
        if (fd_ >= 0 && fd_ != STDOUT_FILENO && fd_ != STDERR_FILENO) {
            ::close(fd_);
        }
    }

    ProgressReporter(const ProgressReporter&) = delete;
    ProgressReporter& operator=(const ProgressReporter&) = delete;

    void update(uint64_t current_items) {
        if (fd_ < 0 || total_ == 0) return;

        int pct = static_cast<int>((current_items * 100) / total_);
        if (pct > 100) pct = 100;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count();

        if (pct > last_pct_ && elapsed_ms >= 150) {
            write_percent(pct);
            last_pct_ = pct;
            last_update_ = now;
        }
    }

    void finish() {
        if (fd_ >= 0 && last_pct_ < 100) {
            write_percent(100);
            last_pct_ = 100;
        }
    }

private:
    void write_percent(int pct) {
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%d\n", pct);
        if (len > 0) {
            ssize_t ignored = ::write(fd_, buf, len);
            (void)ignored;
        }
    }

    int fd_ = -1;
    uint64_t total_ = 0;
    int last_pct_ = -1;
    std::chrono::steady_clock::time_point last_update_;
};
