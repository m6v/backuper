#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <unistd.h>

#pragma pack(push, 1)
struct ImageHeader {
    std::array<char, 8> magic;  // "MYEXT4\0\0"
    uint32_t version;           // 3
    uint32_t block_size;        // Например: 4096
    uint64_t total_blocks;      // Общее число блоков ФС
    uint64_t used_blocks;       // Точное число занятых блоков (для прогресс-бара)
    uint32_t header_crc32;      // CRC32 первых 32 байт этого заголовка
};

struct ChunkHeader {
    uint64_t start_block;       // Начальный блок чанка
    uint32_t block_count;       // Количество блоков подряд (0 = конец потока)
    uint32_t header_crc32;      // CRC32 полей start_block и block_count
    uint32_t data_crc32;        // CRC32 полезных данных чанка
};
#pragma pack(pop)

// RAII-обертка над системным файловым дескриптором
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1, bool auto_close = true)
        : fd_(fd), auto_close_(auto_close) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    void reset(int new_fd = -1, bool auto_close = true) {
        if (auto_close_ && fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
        auto_close_ = auto_close;
    }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
    bool auto_close_;
};

// Единый парсер аргументов командной строки
struct CliArgs {
    std::string prog_name;
    std::string src;
    std::string dst;
    int progress_fd = -1;
};

inline CliArgs parse_cli_args(int argc, char* argv[], const std::string& usage_hint) {
    CliArgs res;
    res.prog_name = (argc > 0) ? *argv : "tool";
    std::vector<std::string> pos;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            throw std::runtime_error("Usage: " + res.prog_name + " " + usage_hint);
        }
        if (arg == "--progress-fd") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Option --progress-fd requires a file descriptor number");
            }
            try {
                res.progress_fd = std::stoi(argv[++i]);
            } catch (...) {
                throw std::runtime_error("Invalid file descriptor number for --progress-fd");
            }
        } else {
            pos.push_back(arg);
        }
    }

    if (pos.size() != 2) {
        throw std::runtime_error("Usage: " + res.prog_name + " " + usage_hint);
    }

    res.src = pos.at(0);
    res.dst = pos.at(1);
    return res;
}

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

// Вывод прогресса в заданный дескриптор (fd) для whiptail и аналогов (dialog, zenity)
class ProgressReporter {
public:
    ProgressReporter(int fd, uint64_t total_blocks, uint32_t block_size)
        : fd_(fd, fd >= 0 && fd != STDOUT_FILENO && fd != STDERR_FILENO),
          total_(total_blocks),
          block_size_(block_size),
          start_time_(std::chrono::steady_clock::now()),
          last_update_(start_time_) {
        if (fd_) {
            write_status(0, 0, 0.0);
            last_pct_ = 0;
        }
    }

    void update(uint64_t current_blocks) {
        if (!fd_ || total_ == 0) return;

        int pct = static_cast<int>((current_blocks * 100) / total_);
        if (pct > 100) pct = 100;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count();

        if (elapsed_ms >= 250) {
            double total_sec = std::chrono::duration<double>(now - start_time_).count();
            double copied_mb = (static_cast<double>(current_blocks) * block_size_) / (1024.0 * 1024.0);
            double speed_mbps = (total_sec > 0.001) ? (copied_mb / total_sec) : 0.0;

            write_status(pct, current_blocks, speed_mbps);
            last_pct_ = pct;
            last_update_ = now;
        }
    }

    void finish() {
        if (fd_) {
            auto now = std::chrono::steady_clock::now();
            double total_sec = std::chrono::duration<double>(now - start_time_).count();
            double copied_mb = (static_cast<double>(total_) * block_size_) / (1024.0 * 1024.0);
            double speed_mbps = (total_sec > 0.001) ? (copied_mb / total_sec) : 0.0;

            write_status(100, total_, speed_mbps);
            last_pct_ = 100;
            fd_.reset(); // Закрытие fd, чтобы whiptail получил EOF до возврата из функции
        }
    }

private:
    void write_status(int pct, uint64_t current_blocks, double speed_mbps) {
        uint64_t copied_mb = (current_blocks * block_size_) / (1024 * 1024);
        uint64_t total_mb  = (total_ * block_size_) / (1024 * 1024);

        std::array<char, 256> buf{};
        int len = std::snprintf(
            buf.data(), buf.size(),
            "XXX\n%d\nОбработано: %llu / %llu МБ | Скорость: %.1f МБ/с\nXXX\n",
            pct,
            static_cast<unsigned long long>(copied_mb),
            static_cast<unsigned long long>(total_mb),
            speed_mbps
        );
        if (len > 0) {
            ssize_t ignored = ::write(fd_.get(), buf.data(), len);
            (void)ignored;
        }
    }

    UniqueFd fd_;
    uint64_t total_ = 0;
    uint32_t block_size_ = 4096;
    int last_pct_ = -1;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_;
};
