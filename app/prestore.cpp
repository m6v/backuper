#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <ext2fs/ext2fs.h>
#include "format.hpp"

bool read_exact(int fd, void* buf, size_t count) {
    size_t total_read = 0;
    char* ptr = static_cast<char*>(buf);

    while (total_read < count) {
        ssize_t res = ::read(fd, ptr + total_read, count - total_read);
        if (res == 0) {
            if (total_read == 0) return false;
            throw std::runtime_error("Unexpected EOF inside stream (corrupted archive)");
        }
        if (res < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Read error: " + std::string(strerror(errno)));
        }
        total_read += res;
    }
    return true;
}

void pwrite_exact(int fd, const void* buf, size_t count, off_t offset) {
    size_t total_written = 0;
    const char* ptr = static_cast<const char*>(buf);

    while (total_written < count) {
        ssize_t res = ::pwrite(fd, ptr + total_written, count - total_written, offset + total_written);
        if (res <= 0) {
            if (res < 0 && errno == EINTR) continue;
            throw std::runtime_error("Write error at offset " + std::to_string(offset));
        }
        total_written += res;
    }
}

void restore_image(const std::string& src_path, const std::string& dst_path, int progress_fd) {
    initialize_ext2_error_table();

    // 0. Проверяем целевой путь (защита от опечаток в /dev/...)
    struct stat st{};
    bool target_exists = (::stat(dst_path.c_str(), &st) == 0);
    bool is_dev_path = (dst_path.rfind("/dev/", 0) == 0); // начинается ли путь с "/dev/"

    if (is_dev_path) {
        if (!target_exists) {
            throw std::runtime_error("Target device '" + dst_path + "' does not exist!");
        }
        if (!S_ISBLK(st.st_mode)) {
            throw std::runtime_error("Target '" + dst_path + "' is in /dev/ but is NOT a block device!");
        }
    } else if (target_exists && !S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) {
        throw std::runtime_error("Target '" + dst_path + "' must be a block device or a regular file!");
    }

    // 1. Запрещаем восстановление на любой смонтированный раздел
    int mount_flags = 0;
    errcode_t err = ext2fs_check_if_mounted(dst_path.c_str(), &mount_flags);
    if (err) {
        throw std::runtime_error("ext2fs_check_if_mounted error: " + std::string(error_message(err)));
    }
    if (mount_flags & EXT2_MF_MOUNTED) {
        throw std::runtime_error(
            "Target '" + dst_path + "' is currently mounted! Unmount it before restoring."
        );
    }

    int in_fd = (src_path == "-") ? STDIN_FILENO : ::open(src_path.c_str(), O_RDONLY);
    if (in_fd < 0) {
        throw std::runtime_error("Cannot open source image: " + std::string(strerror(errno)));
    }

    // 2. Открываем целевое устройство (флаг O_CREAT разрешаем только вне /dev/)
    int open_flags = O_WRONLY | (is_dev_path ? 0 : O_CREAT);
    int out_fd = ::open(dst_path.c_str(), open_flags, 0644);
    if (out_fd < 0) {
        if (in_fd != STDIN_FILENO) ::close(in_fd);
        throw std::runtime_error("Cannot open target device: " + std::string(strerror(errno)));
    }

    try {
        ImageHeader hdr{};
        if (!read_exact(in_fd, &hdr, sizeof(hdr))) {
            throw std::runtime_error("Empty input stream");
        }
        if (std::memcmp(hdr.magic, "MYEXT4\0\0", 8) != 0 || hdr.version != 3) {
            throw std::runtime_error("Invalid image format or unsupported version");
        }

        uint32_t expected_hdr_crc = compute_crc32(&hdr, offsetof(ImageHeader, header_crc32));
        if (hdr.header_crc32 != expected_hdr_crc) {
            throw std::runtime_error("ImageHeader CRC32 mismatch! Header is corrupted.");
        }

        uint64_t target_bytes = 0;
        if (ioctl(out_fd, BLKGETSIZE64, &target_bytes) == 0) {
            uint64_t required_bytes = hdr.total_blocks * hdr.block_size;
            if (target_bytes < required_bytes) {
                throw std::runtime_error("Target device is smaller than original filesystem!");
            }
        }

        std::cerr << "Restoring ext4 (block size: " << hdr.block_size
                  << ", total blocks: " << hdr.total_blocks << ")...\n";

        ProgressReporter progress(progress_fd, hdr.used_blocks);

        std::vector<char> buffer;
        uint64_t restored_blocks = 0;
        bool clean_eof = false;

        while (true) {
            ChunkHeader chunk{};
            if (!read_exact(in_fd, &chunk, sizeof(chunk))) {
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

            if (!read_exact(in_fd, buffer.data(), data_bytes)) {
                throw std::runtime_error("Truncated chunk data");
            }

            uint32_t actual_data_crc = compute_crc32(buffer.data(), data_bytes);
            if (chunk.data_crc32 != actual_data_crc) {
                throw std::runtime_error(
                    "Data CRC32 mismatch at block " + std::to_string(chunk.start_block) + "!"
                );
            }

            off_t disk_offset = static_cast<off_t>(chunk.start_block) * hdr.block_size;
            pwrite_exact(out_fd, buffer.data(), data_bytes, disk_offset);

            restored_blocks += chunk.block_count;
            progress.update(restored_blocks);
        }

        if (!clean_eof) {
            throw std::runtime_error("Stream ended without EOF marker (incomplete backup)!");
        }

        if (target_bytes == 0) {
            off_t full_fs_size = static_cast<off_t>(hdr.total_blocks) * hdr.block_size;
            if (::ftruncate(out_fd, full_fs_size) != 0) {
                std::cerr << "Warning: ftruncate failed: " << strerror(errno) << "\n";
            }
        }

        ::fsync(out_fd);
        progress.finish();
        std::cerr << "Done! Restored " << restored_blocks << " blocks (CRC32 verified).\n";

    } catch (...) {
        if (in_fd != STDIN_FILENO) ::close(in_fd);
        ::close(out_fd);
        throw;
    }

    if (in_fd != STDIN_FILENO) ::close(in_fd);
    ::close(out_fd);
}

int main(int argc, char* argv[]) {
    std::vector<std::string> pos_args;
    int progress_fd = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--progress-fd" && i + 1 < argc) {
            progress_fd = std::stoi(argv[++i]);
        } else {
            pos_args.push_back(arg);
        }
    }

    if (pos_args.size() < 2) {
        std::cerr << "Usage: " << argv[0] << " <input_file|-> <target_device> [--progress-fd N]\n";
        return 1;
    }

    try {
        restore_image(pos_args[0], pos_args[1], progress_fd);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
