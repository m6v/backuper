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
#include "sysutil.hpp"
#include "progress.hpp"
#include "partimg.hpp"

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

// Запись блоков на диск с атомарной фиксацией начала раздела или проверка архива, если задана --verify
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
        out_fd = UniqueFd::open_locked(args.dst, open_flags, LOCK_EX, 0644);
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
    if (hdr.block_size == 0) {
        throw std::runtime_error("Invalid block size (0) in ImageHeader");
    }

    const uint64_t required_bytes = hdr.total_blocks * hdr.block_size;
    bool is_blk_dev = false;

    // Размер защитной зоны в начале раздела (минимум 4096 байт, чтобы гарантированно накрыть суперблок ext4/VBR)
    const size_t head_guard_bytes = static_cast<size_t>(
        std::min<uint64_t>(required_bytes, std::max<uint64_t>(4096, hdr.block_size))
    );

    if (!args.verify_only) {
        uint64_t target_bytes = 0;
        if (::ioctl(out_fd.get(), BLKGETSIZE64, &target_bytes) == 0) {
            is_blk_dev = true;
            if (target_bytes < required_bytes) {
                throw std::runtime_error("Target device is smaller than original filesystem!");
            }
        }

        // Затирание начала раздела (суперблока/VBR) для предотвращения монтирования при сбое
        if (head_guard_bytes > 0) {
            std::vector<char> zero_head(head_guard_bytes, 0);
            pwrite_exact(out_fd.get(), zero_head.data(), zero_head.size(), 0);
            ::fsync(out_fd.get());
        }
    }

    std::cerr << (args.verify_only ? "Verifying" : "Restoring")
              << " image (block size: " << hdr.block_size
              << ", total blocks: " << hdr.total_blocks << ")...\n";

    ProgressReporter progress(args.progress_fd, hdr.used_blocks, hdr.block_size);

    constexpr size_t MAX_ALLOWED_CHUNK_BYTES = 64 * 1024 * 1024;
    std::vector<char> buffer;
    std::vector<std::pair<off_t, std::vector<char>>> deferred_head_chunks; // Отложенные чанки из начала раздела
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

        if (chunk.start_block > hdr.total_blocks ||
            chunk.block_count > hdr.total_blocks - chunk.start_block) {
            throw std::runtime_error("Corrupted chunk: block range exceeds filesystem size");
        }

        size_t data_bytes = static_cast<size_t>(chunk.block_count) * hdr.block_size;
        if (data_bytes > MAX_ALLOWED_CHUNK_BYTES) {
            throw std::runtime_error("Corrupted chunk: chunk size exceeds safety limit");
        }

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
            off_t disk_offset = static_cast<off_t>(chunk.start_block) * hdr.block_size;
            if (static_cast<uint64_t>(disk_offset) < head_guard_bytes) {
                // Придерживаем чанки, перекрывающие первые 4096 байт, до полного завершения потока
                deferred_head_chunks.emplace_back(
                    disk_offset,
                    std::vector<char>(buffer.begin(), buffer.begin() + data_bytes)
                );
            } else {
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
        if (!is_blk_dev) {
            if (::ftruncate(out_fd.get(), static_cast<off_t>(required_bytes)) != 0) {
                std::cerr << "Warning: ftruncate failed: " << strerror(errno) << "\n";
            }
        }

        // Сброс основных данных на диск и атомарный возврат начала раздела (суперблока/VBR)
        ::fsync(out_fd.get());
        for (const auto& [offset, chunk_data] : deferred_head_chunks) {
            pwrite_exact(out_fd.get(), chunk_data.data(), chunk_data.size(), offset);
        }
        if (!deferred_head_chunks.empty()) {
            ::fsync(out_fd.get());
        }
        out_fd.reset();
    }

    progress.finish();

    std::cerr << "Done! " << (args.verify_only ? "Verified " : "Restored ")
              << restored_blocks << " blocks (CRC32 verified).\n";
}

// Точка входа: разбирает аргументы, запускает восстановление/проверку и перехватывает ошибки
int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Universal block-level image restore & verify tool (ext2/3/4, NTFS, exFAT, raw)",
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
