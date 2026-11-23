#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <stdexcept>
#include <iostream>

#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <mntent.h>

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

    std::unique_ptr<FILE, decltype(&::endmntent)> mtab(::setmntent("/proc/mounts", "r"), &::endmntent);
    if (!mtab) {
        throw std::runtime_error("Cannot open /proc/mounts to check mount status");
    }

    struct mntent* entry = nullptr;
    while ((entry = ::getmntent(mtab.get())) != nullptr) {
        if (!entry->mnt_fsname || entry->mnt_fsname[0] != '/') continue;
        char mnt_real[PATH_MAX];
        const char* resolved_mnt = ::realpath(entry->mnt_fsname, mnt_real);
        std::string mnt_dev = resolved_mnt ? resolved_mnt : entry->mnt_fsname;

        if (mnt_dev == target_dev) {
            bool is_ro = (::hasmntopt(entry, MNTOPT_RO) != nullptr);
            if (!(allow_readonly && is_ro)) {
                throw std::runtime_error("Device '" + path + "' is mounted! Unmount it first.");
            }
            break;
        }
    }
}

// RAII-обертка над системным файловым дескриптором
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1, bool auto_close = true)
        : fd_(fd), auto_close_(auto_close) {}

    ~UniqueFd() { reset(); }

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
