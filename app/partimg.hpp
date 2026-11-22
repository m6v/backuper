#pragma once
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <string>
#include <stdexcept>
#include <chrono>
#include <iostream>


#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <mntent.h>
#include <zlib.h>


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

// Проверка через /proc/mounts, не смонтировано ли устройство (учитывает симлинки /dev/disk/by-*)
inline void ensure_not_mounted(const std::string& path, bool allow_readonly = false) {
    char target_real[PATH_MAX];
    const char* resolved_target = ::realpath(path.c_str(), target_real);
    std::string target_dev = resolved_target ? resolved_target : path;

    FILE* mtab = ::setmntent("/proc/mounts", "r");
    if (!mtab) {
        throw std::runtime_error("Cannot open /proc/mounts to check mount status");
    }

    struct mntent* entry = nullptr;
    bool is_mounted = false;
    bool is_ro = false;

    while ((entry = ::getmntent(mtab)) != nullptr) {
        if (entry->mnt_fsname[0] != '/') continue;
        char mnt_real[PATH_MAX];
        const char* resolved_mnt = ::realpath(entry->mnt_fsname, mnt_real);
        std::string mnt_dev = resolved_mnt ? resolved_mnt : entry->mnt_fsname;

        if (mnt_dev == target_dev) {
            is_mounted = true;
            is_ro = (::hasmntopt(entry, MNTOPT_RO) != nullptr);
            break;
        }
    }
    ::endmntent(mtab);

    if (is_mounted && !(allow_readonly && is_ro)) {
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
    bool verify_only = false;
};

// Разбор опций (--status-fd / -s <FD>, --verify / -v, --help / -h) и позиционных аргументов
inline CliArgs parse_cli_args(int argc, char* argv[],
                              const std::string& description,
                              const std::string& usage_hint,
                              bool support_verify = false) {
    std::string prog = (argc > 0) ? *argv : "tool";
    std::string opts_hint = support_verify ? "[--status-fd N] [--verify] " : "[--status-fd N] ";
    std::string short_usage = "Usage: " + prog + " " + opts_hint + usage_hint;

    std::string full_help =
        prog + " - " + description + "\n\n" +
        short_usage + "\n\n"
        "Options:\n"
        "  -s, --status-fd N   Write machine-readable progress (whiptail/dialog) to FD N\n";
    if (support_verify) {
        full_help += "  -v, --verify        Verify image integrity (CRC32 & EOF) without writing to disk\n";
    }
    full_help += "  -h, --help          Show this help message and exit\n";

    static constexpr struct option long_opts[] = {
        {"status-fd", required_argument, nullptr, 's'},
        {"verify",    no_argument,       nullptr, 'v'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    CliArgs res;
    int opt;
    const char* short_opts = support_verify ? "s:vh" : "s:h";
    while ((opt = ::getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        if (opt == 's') {
            try {
                res.progress_fd = std::stoi(optarg);
            } catch (...) {
                throw std::runtime_error("Invalid file descriptor number for --status-fd");
            }
        } else if (opt == 'v' && support_verify) {
            res.verify_only = true;
        } else if (opt == 'h') {
            std::cout << full_help;
            std::exit(0);
        } else {
            throw std::runtime_error(short_usage);
        }
    }

    int expected_pos = res.verify_only ? 1 : 2;
    if (argc - optind != expected_pos) {
        throw std::runtime_error(short_usage);
    }

    res.src = *(argv + optind);
    if (!res.verify_only) {
        res.dst = *(argv + optind + 1);
    }
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

// Формирует поток EXTIMAGE: склеивает занятые блоки в чанки по 4 МБ, считает CRC32 и обновляет прогресс
class ImageWriter {
public:
    ImageWriter(const CliArgs& args, uint32_t block_size, uint64_t total_blocks, uint64_t used_blocks)
        : out_fd_(args.dst == "-" ? STDOUT_FILENO
                                  : ::open(args.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644),
                  args.dst != "-"),
          block_size_(block_size),
          total_blocks_(total_blocks),
          max_blocks_per_chunk_(std::max<uint32_t>(1, (4 * 1024 * 1024) / block_size)),
          buffer_(static_cast<size_t>(max_blocks_per_chunk_) * block_size),
          progress_(args.progress_fd, used_blocks, block_size) {
        if (!out_fd_) {
            throw std::runtime_error("Cannot open output '" + args.dst + "': " + std::string(strerror(errno)));
        }

        ImageHeader img_hdr{};
        img_hdr.magic = IMAGE_MAGIC;
        img_hdr.version = IMAGE_VERSION;
        img_hdr.block_size = block_size_;
        img_hdr.total_blocks = total_blocks_;
        img_hdr.used_blocks = used_blocks;
        img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
        write_all(&img_hdr, sizeof(img_hdr));
    }

    // Добавляет один занятый блок (автоматически склеивает соседние блоки в чанк до 4 МБ)
    template <typename ReadFunc>
    void push_block(uint64_t blk, ReadFunc&& read_blocks) {
        if (chunk_count_ > 0 && blk != chunk_start_ + chunk_count_) {
            flush(read_blocks);
        }
        if (chunk_count_ == 0) {
            chunk_start_ = blk;
        }
        chunk_count_++;
        if (chunk_count_ == max_blocks_per_chunk_) {
            flush(read_blocks);
        }
    }

    // Записывает готовый непрерывный диапазон блоков (удобно для partback.dd)
    template <typename ReadFunc>
    void write_range(uint64_t start, uint32_t count, ReadFunc&& read_blocks) {
        flush(read_blocks);
        if (count == 0) return;
        chunk_start_ = start;
        chunk_count_ = count;
        flush(read_blocks);
    }

    // Сбрасывает остаток буфера, пишет маркер EOF, закрывает поток и выводит итог
    template <typename ReadFunc>
    void finish(ReadFunc&& read_blocks, const char* unit_name = "blocks") {
        flush(read_blocks);

        ChunkHeader eof_hdr{};
        eof_hdr.start_block = 0;
        eof_hdr.block_count = 0;
        eof_hdr.header_crc32 = compute_crc32(&eof_hdr, offsetof(ChunkHeader, header_crc32));
        eof_hdr.data_crc32 = 0;
        write_all(&eof_hdr, sizeof(eof_hdr));

        out_fd_.reset();
        progress_.finish();

        std::cerr << "Success! Copied " << copied_blocks_ << " of " << total_blocks_
                  << " " << unit_name << " (" << (copied_blocks_ * block_size_) / (1024 * 1024) << " MB)\n";
    }

private:
    void write_all(const void* buf, size_t count) {
        const char* ptr = static_cast<const char*>(buf);
        io_exact(count, [&](size_t off, size_t rem) {
            return ::write(out_fd_.get(), ptr + off, rem);
        });
    }

    template <typename ReadFunc>
    void flush(ReadFunc&& read_blocks) {
        if (chunk_count_ == 0) return;

        size_t bytes = static_cast<size_t>(chunk_count_) * block_size_;
        read_blocks(chunk_start_, chunk_count_, buffer_.data());

        ChunkHeader chunk_hdr{};
        chunk_hdr.start_block = chunk_start_;
        chunk_hdr.block_count = chunk_count_;
        chunk_hdr.header_crc32 = compute_crc32(&chunk_hdr, offsetof(ChunkHeader, header_crc32));
        chunk_hdr.data_crc32 = compute_crc32(buffer_.data(), bytes);

        write_all(&chunk_hdr, sizeof(chunk_hdr));
        write_all(buffer_.data(), bytes);

        copied_blocks_ += chunk_count_;
        progress_.update(copied_blocks_);
        chunk_count_ = 0;
    }

    UniqueFd out_fd_;
    uint32_t block_size_;
    uint64_t total_blocks_;
    uint32_t max_blocks_per_chunk_;
    std::vector<char> buffer_;
    ProgressReporter progress_;

    uint64_t chunk_start_ = 0;
    uint32_t chunk_count_ = 0;
    uint64_t copied_blocks_ = 0;
};
