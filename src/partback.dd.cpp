#include <iostream>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "imgwriter.hpp"

// Полное посекторное чтение источника (раздела или файла) и упаковка через ImageWriter
void backup_raw_volume(const CliArgs& args) {
    struct stat st{};
    if (::stat(args.src.c_str(), &st) != 0) {
        throw std::runtime_error("Source '" + args.src + "' does not exist: " + std::string(strerror(errno)));
    }
    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) {
        throw std::runtime_error("Source '" + args.src + "' must be a block device or a regular file!");
    }

    ensure_not_mounted(args.src, true);

    UniqueFd in_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (!in_fd) {
        throw std::runtime_error("Cannot open source '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (::flock(in_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source '" + args.src + "' is locked by an exclusive operation!");
    }
    ::posix_fadvise(in_fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);

    // Определение точного размера источника в байтах
    uint64_t total_bytes = 0;
    if (S_ISBLK(st.st_mode)) {
        if (::ioctl(in_fd.get(), BLKGETSIZE64, &total_bytes) != 0) {
            throw std::runtime_error("Cannot get block device size: " + std::string(strerror(errno)));
        }
    } else {
        total_bytes = static_cast<uint64_t>(st.st_size);
    }

    if (total_bytes == 0) {
        throw std::runtime_error("Source '" + args.src + "' is empty (0 bytes)!");
    }

    // Автовыбор оптимального размера блока: 4096 (оптимум), 512 (классический сектор) или 1 (любой файл)
    uint32_t block_size = 4096;
    if (total_bytes % 4096 != 0) {
        block_size = (total_bytes % 512 == 0) ? 512 : 1;
    }

    uint64_t total_blocks = total_bytes / block_size;

    auto read_blocks = [&](uint64_t start, uint32_t count, char* dst) {
        size_t bytes = static_cast<size_t>(count) * block_size;
        io_exact(bytes, [&](size_t off, size_t rem) {
            return ::pread(in_fd.get(), dst + off, rem, static_cast<off_t>(start) * block_size + off);
        });
    };

    ImageWriter writer(args, block_size, total_blocks, total_blocks);
    writer.write_range(0, total_blocks, read_blocks);
    writer.finish(read_blocks);
}

// Точка входа: разбирает аргументы, запускает raw-бэкап и перехватывает ошибки
int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Raw block-level backup tool for any partition (ESP/FAT32, NTFS, XFS, etc.)",
            "<source_device> <output_file|->"
        );
        backup_raw_volume(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
