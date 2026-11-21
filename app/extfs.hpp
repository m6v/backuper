#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <getopt.h>
#include <unistd.h>

#pragma pack(push, 1)

inline constexpr std::array<char, 8> IMAGE_MAGIC = {'E', 'X', 'T', '4', 'P', 'A', 'R', 'T'};
inline constexpr uint32_t IMAGE_VERSION = 3;

struct ImageHeader {
    std::array<char, 8> magic;  // Магическая последовательность
    uint32_t version;           // Версия программы
    uint32_t block_size;        // Размер блока ФС (обычно 4096)
    uint64_t total_blocks;      // Общее число блоков ФС
    uint64_t used_blocks;       // Число занятых блоков (для прогресс-бара)
    uint32_t header_crc32;      // CRC32 первых 32 байт этого заголовка
};

struct ChunkHeader {
    uint64_t start_block;       // Начальный блок непрерывного диапазона
    uint32_t block_count;       // Количество блоков в чанке (0 = конец потока)
    uint32_t header_crc32;      // CRC32 полей start_block и block_count
    uint32_t data_crc32;        // CRC32 полезных данных чанка
};
#pragma pack(pop)

// RAII-обертка над системным файловым дескриптором
class UniqueFd {
public:
    // Сохраняет дескриптор и флаг необходимости его автоматического закрытия
    explicit UniqueFd(int fd = -1, bool auto_close = true)
        : fd_(fd), auto_close_(auto_close) {}

    // Гарантированно закрывает дескриптор при выходе объекта из области видимости
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // Закрывает текущий дескриптор (если разрешено) и заменяет его новым
    void reset(int new_fd = -1, bool auto_close = true) {
        if (auto_close_ && fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
        auto_close_ = auto_close;
    }

    // Возвращает сырой файловый дескриптор для системных вызовов
    int get() const { return fd_; }

    // Проверяет, валиден ли дескриптор (>= 0)
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
    bool auto_close_;
};

struct CliArgs {
    std::string src;
    std::string dst;
    int progress_fd = -1;
};

// Разбирает опции (--status-fd / -s <FD>, --help / -h) и два позиционных аргумента
inline CliArgs parse_cli_args(int argc, char* argv[],
                              const std::string& description,
                              const std::string& usage_hint) {
    std::string prog = (argc > 0) ? *argv : "tool";
    std::string short_usage = "Usage: " + prog + " [--status-fd N] " + usage_hint;
    std::string full_help =
        prog + " - " + description + "\n\n" +
        short_usage + "\n\n"
        "Options:\n"
        "  -s, --status-fd N   Write machine-readable progress (whiptail/dialog) to FD N\n"
        "  -h, --help          Show this help message and exit\n";

    static constexpr struct option long_opts[] = {
        {"status-fd", required_argument, nullptr, 's'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    CliArgs res;
    int opt;
    while ((opt = ::getopt_long(argc, argv, "s:h", long_opts, nullptr)) != -1) {
        if (opt == 's') {
            try {
                res.progress_fd = std::stoi(optarg);
            } catch (...) {
                throw std::runtime_error("Invalid file descriptor number for --status-fd");
            }
        } else if (opt == 'h') {
            std::cout << full_help;
            std::exit(0);
        } else {
            throw std::runtime_error(short_usage);
        }
    }

    if (argc - optind != 2) {
        throw std::runtime_error(short_usage);
    }

    res.src = *(argv + optind);
    res.dst = *(argv + optind + 1);
    return res;
}

// Вычисляет контрольную сумму CRC32 (IEEE 802.3) с использованием таблицы, сгенерированной при компиляции
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

// Управляет выводом прогресса и скорости (МБ/с) в заданный FD для whiptail/dialog
class ProgressReporter {
public:
    // Инициализирует таймер, запоминает общий объем и отправляет начальный 0%
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

    // Периодически (не чаще раза в 250 мс) рассчитывает процент и среднюю скорость
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

    // Отправляет финальные 100% и закрывает FD, чтобы внешний диалог получил EOF и закрылся
    void finish() {
        if (fd_) {
            auto now = std::chrono::steady_clock::now();
            double total_sec = std::chrono::duration<double>(now - start_time_).count();
            double copied_mb = (static_cast<double>(total_) * block_size_) / (1024.0 * 1024.0);
            double speed_mbps = (total_sec > 0.001) ? (copied_mb / total_sec) : 0.0;

            write_status(100, total_, speed_mbps);
            last_pct_ = 100;
            fd_.reset();
        }
    }

private:
    // Форматирует и записывает статус в формате XXX для whiptail/dialog
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
