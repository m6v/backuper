#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "extfs.hpp"

// Чтение count байт из файла (пайпа) или возврат false при EOF
bool read_exact(int fd, void* buf, size_t count) {
    char* ptr = static_cast<char*>(buf);
    return io_exact(count, [&](size_t off, size_t rem) {
        return ::read(fd, ptr + off, rem);
    }, true);
}

// Запись count байт на диск по заданному смещению (pwrite)
void pwrite_exact(int fd, const void* buf, size_t count, off_t offset) {
    const char* ptr = static_cast<const char*>(buf);
    io_exact(count, [&](size_t off, size_t rem) {
        return ::pwrite(fd, ptr + off, rem, offset + off);
    });
}

// Проверка архива (и запись блоков на диск с атомарной фиксацией суперблока, если не --verify)
void restore_volume(const CliArgs& args) {
    bool is_dev = false;
    UniqueFd out_fd;

    if (!args.verify_only) {
        struct stat st{};
        bool exists = (::stat(args.dst.c_str(), &st) == 0);
        is_dev = (args.dst.rfind("/dev/", 0) == 0);

        if (is_dev && !exists) {
            throw std::runtime_error("Target device '" + args.dst + "' does not exist!");
        }
        if (exists && !S_ISBLK(st.st_mode) && (is_dev || !S_ISREG(st.st_mode))) {
            throw std::runtime_error("Target '" + args.dst + "' must be a block device" + (is_dev ? "" : " or regular file") + "!");
        }

        ensure_not_mounted(args.dst);

        int open_flags = O_WRONLY | (is_dev ? 0 : O_CREAT);
        out_fd.reset(::open(args.dst.c_str(), open_flags, 0644), true);
        if (!out_fd) {
            throw std::runtime_error("Cannot open target device '" + args.dst + "': " + std::string(strerror(errno)));
        }

        // Эксклюзивная блокировка устройства (защита от гонок с udev и параллельных процессов)
        if (::flock(out_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Target '" + args.dst + "' is busy (locked by another process or udev)!");
        }
    }

    bool is_stdin = (args.src == "-");
    UniqueFd in_fd(
        is_stdin ? STDIN_FILENO : ::open(args.src.c_str(), O_RDONLY),
        !is_stdin
    );
    if (!in_fd) {
        throw std::runtime_error("Cannot open source image '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (!is_stdin) {
        ::posix_fadvise(in_fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    }

    ImageHeader hdr{};
    if (!read_exact(in_fd.get(), &hdr, sizeof(hdr))) {
        throw std::runtime_error("Empty input stream");
    }

    if (hdr.magic != IMAGE_MAGIC || hdr.version != IMAGE_VERSION) {
        throw std::runtime_error("Invalid image format or unsupported version");
    }

    uint32_t expected_hdr_crc = compute_crc32(&hdr, offsetof(ImageHeader, header_crc32));
    if (hdr.header_crc32 != expected_hdr_crc) {
        throw std::runtime_error("ImageHeader CRC32 mismatch! Header is corrupted.");
    }

    uint64_t target_bytes = 0;
    if (!args.verify_only) {
        if (ioctl(out_fd.get(), BLKGETSIZE64, &target_bytes) == 0) {
            uint64_t required_bytes = hdr.total_blocks * hdr.block_size;
            if (target_bytes < required_bytes) {
                throw std::runtime_error("Target device is smaller than original filesystem!");
            }
        }

        // Фаза 1: затираем начало раздела (основной суперблок), чтобы недописанная ФС не монтировалась
        std::vector<char> zero_sb(hdr.block_size, 0);
        pwrite_exact(out_fd.get(), zero_sb.data(), zero_sb.size(), 0);
        ::fsync(out_fd.get());
    }

    std::cerr << (args.verify_only ? "Verifying" : "Restoring")
              << " ext4 (block size: " << hdr.block_size
              << ", total blocks: " << hdr.total_blocks << ")...\n";

    ProgressReporter progress(args.progress_fd, hdr.used_blocks, hdr.block_size);

    std::vector<char> buffer;
    std::vector<char> first_chunk; // Буфер для отложенной записи первого чанка (с суперблоком)
    uint64_t restored_blocks = 0;
    bool clean_eof = false;

    while (true) {
        ChunkHeader chunk{};
        if (!read_exact(in_fd.get(), &chunk, sizeof(chunk))) {
            break;
        }

        uint32_t expected_chunk_hdr_crc = compute_crc32(&chunk, offsetof(ChunkHeader, header_crc32));
        if (chunk.header_crc32 != expected_chunk_hdr_crc) {
            throw std::runtime_error("ChunkHeader CRC32 mismatch! Stream is corrupted.");
        }

        if (chunk.block_count == 0) {
            clean_eof = true;
            break;
        }

        if (chunk.start_block + chunk.block_count > hdr.total_blocks) {
            throw std::runtime_error("Corrupted chunk: block range exceeds filesystem size");
        }

        size_t data_bytes = static_cast<size_t>(chunk.block_count) * hdr.block_size;
        if (buffer.size() < data_bytes) {
            buffer.resize(data_bytes);
        }

        if (!read_exact(in_fd.get(), buffer.data(), data_bytes)) {
            throw std::runtime_error("Truncated chunk data");
        }

        uint32_t actual_data_crc = compute_crc32(buffer.data(), data_bytes);
        if (chunk.data_crc32 != actual_data_crc) {
            throw std::runtime_error(
                "Data CRC32 mismatch at block " + std::to_string(chunk.start_block) + "!"
            );
        }

        if (!args.verify_only) {
            if (chunk.start_block == 0) {
                // Пропуск первого чанка с суперблоком до полного завершения потока
                first_chunk.assign(buffer.begin(), buffer.begin() + data_bytes);
            } else {
                off_t disk_offset = static_cast<off_t>(chunk.start_block) * hdr.block_size;
                pwrite_exact(out_fd.get(), buffer.data(), data_bytes, disk_offset);
            }
        }

        restored_blocks += chunk.block_count;
        progress.update(restored_blocks);
    }

    if (!clean_eof) {
        throw std::runtime_error("Stream ended without EOF marker (incomplete backup)!");
    }

    if (!args.verify_only) {
        if (target_bytes == 0) {
            off_t full_fs_size = static_cast<off_t>(hdr.total_blocks) * hdr.block_size;
            if (::ftruncate(out_fd.get(), full_fs_size) != 0) {
                std::cerr << "Warning: ftruncate failed: " << strerror(errno) << "\n";
            }
        }

        // Сброс основных данных и атомарный возврат суперблока на свое место
        ::fsync(out_fd.get());
        if (!first_chunk.empty()) {
            pwrite_exact(out_fd.get(), first_chunk.data(), first_chunk.size(), 0);
            ::fsync(out_fd.get());
        }
        out_fd.reset();
    }

    progress.finish();

    std::cerr << "Done! " << (args.verify_only ? "Verified " : "Restored ")
              << restored_blocks << " blocks (CRC32 verified).\n";
}


int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Block-level image restore & verify tool for ext2/ext3/ext4 filesystems",
            "<input_file|-> [target_device]",
            true
        );
        restore_volume(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
