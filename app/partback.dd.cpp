#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "partimg.hpp"

// Запись count байт из буфера в файл или пайп
void write_exact(int fd, const void* buf, size_t count) {
    const char* ptr = static_cast<const char*>(buf);
    io_exact(count, [&](size_t off, size_t rem) {
        return ::write(fd, ptr + off, rem);
    });
}

// Полное посекторное чтение источника (раздела или файла) и упаковка в формат EXTIMAGE
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

    bool is_stdout = (args.dst == "-");
    UniqueFd out_fd(
        is_stdout ? STDOUT_FILENO : ::open(args.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644),
        !is_stdout
    );
    if (!out_fd) {
        throw std::runtime_error("Cannot open output '" + args.dst + "': " + std::string(strerror(errno)));
    }

    ImageHeader img_hdr{};
    img_hdr.magic = IMAGE_MAGIC;
    img_hdr.version = IMAGE_VERSION;
    img_hdr.block_size = block_size;
    img_hdr.total_blocks = total_blocks;
    img_hdr.used_blocks = total_blocks; // В raw-режиме копируются все блоки
    img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
    write_exact(out_fd.get(), &img_hdr, sizeof(img_hdr));

    ProgressReporter progress(args.progress_fd, total_blocks, block_size);

    constexpr size_t MAX_CHUNK_BYTES = 4 * 1024 * 1024;
    const uint32_t max_blocks_per_chunk = MAX_CHUNK_BYTES / block_size;
    std::vector<char> buffer(MAX_CHUNK_BYTES);

    uint64_t current_block = 0;

    while (current_block < total_blocks) {
        uint32_t blocks_to_read = static_cast<uint32_t>(
            std::min<uint64_t>(max_blocks_per_chunk, total_blocks - current_block)
        );
        size_t bytes_to_read = static_cast<size_t>(blocks_to_read) * block_size;

        // Чтение ровно bytes_to_read байт с диска
        io_exact(bytes_to_read, [&](size_t off, size_t rem) {
            return ::read(in_fd.get(), buffer.data() + off, rem);
        });

        ChunkHeader chunk_hdr{};
        chunk_hdr.start_block = current_block;
        chunk_hdr.block_count = blocks_to_read;
        chunk_hdr.header_crc32 = compute_crc32(&chunk_hdr, offsetof(ChunkHeader, header_crc32));
        chunk_hdr.data_crc32 = compute_crc32(buffer.data(), bytes_to_read);

        write_exact(out_fd.get(), &chunk_hdr, sizeof(chunk_hdr));
        write_exact(out_fd.get(), buffer.data(), bytes_to_read);

        current_block += blocks_to_read;
        progress.update(current_block);
    }

    ChunkHeader eof_hdr{};
    eof_hdr.start_block = 0;
    eof_hdr.block_count = 0;
    eof_hdr.header_crc32 = compute_crc32(&eof_hdr, offsetof(ChunkHeader, header_crc32));
    eof_hdr.data_crc32 = 0;
    write_exact(out_fd.get(), &eof_hdr, sizeof(eof_hdr));

    out_fd.reset();
    progress.finish();

    std::cerr << "Success! Copied " << current_block << " blocks ("
              << (current_block * block_size) / (1024 * 1024) << " MB)\n";
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
