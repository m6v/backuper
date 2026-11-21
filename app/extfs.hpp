#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <string>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <getopt.h>
#include <zlib.h>
#include <ext2fs/ext2fs.h>

inline constexpr std::array<char, 8> IMAGE_MAGIC = {'E', 'X', 'T', 'I', 'M', 'A', 'G', 'E'};
inline constexpr uint32_t IMAGE_VERSION = 3;

#pragma pack(push, 1)
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

// Вычисление контрольной суммы CRC32 (IEEE 802.3) с использованием zlib
inline uint32_t compute_crc32(const void* data, size_t length, uint32_t initial = 0) {
    return static_cast<uint32_t>(
        ::crc32(initial, static_cast<const Bytef*>(data), static_cast<uInt>(length))
    );
}

// Гарантированное выполнение системного вызова ввода-вывода (read/write/pwrite) на всю длину count
template <typename Func>
inline bool io_exact(size_t count, Func&& op, bool allow_clean_eof = false) {
    size_t done = 0;
    while (done < count) {
        ssize_t res = op(done, count - done);
        if (res == 0) {
            if (allow_clean_eof && done == 0) return false;
            throw std::runtime_error("Unexpected EOF in stream");
        }
        if (res < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("I/O error: " + std::string(strerror(errno)));
        }
        done += res;
    }
    return true;
}

// Проверка, что устройство не смонтировано (или смонтировано только в ro, если разрешено)
inline void ensure_not_mounted(const std::string& path, bool allow_readonly = false) {
    initialize_ext2_error_table();
    int flags = 0;
    if (errcode_t err = ext2fs_check_if_mounted(path.c_str(), &flags)) {
        throw std::runtime_error("Mount check error: " + std::string(error_message(err)));
    }
    if ((flags & EXT2_MF_MOUNTED) && !(allow_readonly && (flags & EXT2_MF_READONLY))) {
        throw std::runtime_error("Device '" + path + "' is mounted! Unmount it first.");
    }
}

// RAII-обертка над системным файловым дескриптором
class UniqueFd {
public:
    // Сохранение дескриптора и флага необходимости его автоматического закрытия
    explicit UniqueFd(int fd = -1, bool auto_close = true)
        : fd_(fd), auto_close_(auto_close) {}

    // Гарантированное закрытие дескриптора при выходе объекта из области видимости
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // Закрытие текущего дескриптора (если разрешено) и замена его новым
    void reset(int new_fd = -1, bool auto_close = true) {
        if (auto_close_ && fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
        auto_close_ = auto_close;
    }

    // Возврат сырого файлового дескриптора для системных вызовов
    int get() const { return fd_; }

    // Проверка валидности дескриптора (>= 0)
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

// Разбор опций (--status-fd / -s <FD>, --help / -h) и двух позиционных аргументов src и dst
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

// Управление выводом прогресса и скорости (МБ/с) в заданный FD (для whiptail/dialog)
class ProgressReporter {
public:
    // Инициализация таймера, запоминание общего объема и отправка начального 0%
    ProgressReporter(int fd, uint64_t total_blocks, uint32_t block_size)
        : fd_(fd, fd >= 0 && fd != STDOUT_FILENO && fd != STDERR_FILENO),
          total_(total_blocks),
          block_size_(block_size),
          start_time_(std::chrono::steady_clock::now()),
          last_update_(start_time_) {
        if (fd_) {
            write_status(0, start_time_);
        }
    }

    // Периодический (не чаще раза в 250 мс) расчет процента и средней скорости
    void update(uint64_t current_blocks) {
        if (!fd_ || total_ == 0) return;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count() >= 250) {
            write_status(current_blocks, now);
            last_update_ = now;
        }
    }

    // Отправка финальных 100% и закрытие FD, чтобы внешний диалог получил EOF и закрылся
    void finish() {
        if (fd_) {
            write_status(total_, std::chrono::steady_clock::now());
            fd_.reset();
        }
    }

private:
    // Расчет статистики и запись статуса в формате XXX для whiptail/dialog
    void write_status(uint64_t current_blocks, std::chrono::steady_clock::time_point now) {
        int pct = (total_ > 0) ? static_cast<int>((current_blocks * 100) / total_) : 100;
        if (pct > 100) pct = 100;

        double total_sec = std::chrono::duration<double>(now - start_time_).count();
        double copied_mb_exact = (static_cast<double>(current_blocks) * block_size_) / (1024.0 * 1024.0);
        double speed_mbps = (total_sec > 0.001) ? (copied_mb_exact / total_sec) : 0.0;

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
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_;
};
