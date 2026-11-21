#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <ext2fs/ext2fs.h>
#include "format.hpp"

void write_exact(int fd, const void* buf, size_t count) {
    size_t total_written = 0;
    const char* ptr = static_cast<const char*>(buf);
    while (total_written < count) {
        ssize_t res = ::write(fd, ptr + total_written, count - total_written);
        if (res <= 0) {
            if (res < 0 && errno == EINTR) continue;
            throw std::runtime_error("Write error: " + std::string(strerror(errno)));
        }
        total_written += res;
    }
}

class Ext2Volume {
public:
    explicit Ext2Volume(const std::string& device_path) {
        initialize_ext2_error_table();

        // 1. Проверяем, не смонтирован ли раздел прямо сейчас!
        int mount_flags = 0;
        errcode_t err = ext2fs_check_if_mounted(device_path.c_str(), &mount_flags);
        if (err) {
            throw std::runtime_error("ext2fs_check_if_mounted error: " + std::string(error_message(err)));
        }
        if ((mount_flags & EXT2_MF_MOUNTED) && !(mount_flags & EXT2_MF_READONLY)) {
            throw std::runtime_error(
                "Device '" + device_path + "' is mounted read-write! Unmount it or remount read-only first."
            );
        }

        // 2. Открываем ФС
        err = ext2fs_open(
            device_path.c_str(), EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs_
        );
        if (err) {
            throw std::runtime_error("ext2fs_open error: " + std::string(error_message(err)));
        }

        err = ext2fs_read_block_bitmap(fs_);
        if (err) {
            ext2fs_close(fs_);
            throw std::runtime_error("read_block_bitmap error: " + std::string(error_message(err)));
        }
    }

    ~Ext2Volume() {
        if (fs_) ext2fs_close(fs_);
    }

    Ext2Volume(const Ext2Volume&) = delete;
    Ext2Volume& operator=(const Ext2Volume&) = delete;
    ext2_filsys handle() const { return fs_; }

private:
    ext2_filsys fs_ = nullptr;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv << " <ext4_device_or_image> <output_file_or_->\n";
        return 1;
    }

    std::string src_path = argv[1];
    std::string dst_path = argv[2];

    int out_fd = (dst_path == "-")
        ? STDOUT_FILENO
        : ::open(dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (out_fd < 0) {
        std::cerr << "Cannot open output: " << strerror(errno) << "\n";
        return 1;
    }

    try {
        Ext2Volume vol(src_path);
        ext2_filsys fs = vol.handle();

        // Проверяем чистоту ФС и журнала
        if (fs->super->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
            throw std::runtime_error("Filesystem has dirty journal (needs recovery). Run e2fsck or mount/umount cleanly first.");
        }
        if (fs->super->s_state & EXT2_ERROR_FS) {
            std::cerr << "Warning: Filesystem has errors marked in superblock!\n";
        }

        blk64_t total_blocks = ext2fs_blocks_count(fs->super);
        blk64_t first_block  = fs->super->s_first_data_block;
        uint32_t block_size  = fs->blocksize;

        // Заполняем и подписываем заголовок образа
        ImageHeader img_hdr{};
        std::memcpy(img_hdr.magic, "MYEXT4\0\0", 8);
        img_hdr.version = 2;
        img_hdr.block_size = block_size;
        img_hdr.total_blocks = total_blocks;
        img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
        write_exact(out_fd, &img_hdr, sizeof(img_hdr));

        const size_t MAX_CHUNK_BYTES = 4 * 1024 * 1024;
        const uint32_t max_blocks_per_chunk = std::max<uint32_t>(1, MAX_CHUNK_BYTES / block_size);
        std::vector<char> buffer(max_blocks_per_chunk * block_size);

        blk64_t total_copied_blocks = 0;

        auto flush_chunk = [&](blk64_t start, uint32_t count) {
            if (count == 0) return;

            size_t bytes_to_read = static_cast<size_t>(count) * block_size;
            errcode_t err = io_channel_read_blk64(fs->io, start, count, buffer.data());
            if (err) {
                throw std::runtime_error("Read error at block " + std::to_string(start));
            }

            ChunkHeader chunk_hdr{};
            chunk_hdr.start_block = start;
            chunk_hdr.block_count = count;
            chunk_hdr.header_crc32 = compute_crc32(&chunk_hdr, offsetof(ChunkHeader, header_crc32));
            chunk_hdr.data_crc32 = compute_crc32(buffer.data(), bytes_to_read);

            write_exact(out_fd, &chunk_hdr, sizeof(chunk_hdr));
            write_exact(out_fd, buffer.data(), bytes_to_read);

            total_copied_blocks += count;
        };

        blk64_t chunk_start = 0;
        uint32_t chunk_count = 0;

        for (blk64_t blk = first_block; blk < total_blocks; ++blk) {
            if (ext2fs_test_block_bitmap2(fs->block_map, blk)) {
                if (chunk_count == 0) chunk_start = blk;
                chunk_count++;

                if (chunk_count == max_blocks_per_chunk) {
                    flush_chunk(chunk_start, chunk_count);
                    chunk_count = 0;
                }
            } else if (chunk_count > 0) {
                flush_chunk(chunk_start, chunk_count);
                chunk_count = 0;
            }
        }

        if (chunk_count > 0) {
            flush_chunk(chunk_start, chunk_count);
        }

        // Маркер конца потока (также защищенный CRC32)
        ChunkHeader eof_hdr{};
        eof_hdr.start_block = 0;
        eof_hdr.block_count = 0;
        eof_hdr.header_crc32 = compute_crc32(&eof_hdr, offsetof(ChunkHeader, header_crc32));
        eof_hdr.data_crc32 = 0;
        write_exact(out_fd, &eof_hdr, sizeof(eof_hdr));

        if (out_fd != STDOUT_FILENO) ::close(out_fd);

        std::cerr << "Success! Copied " << total_copied_blocks << " of " << total_blocks
                  << " blocks (" << (total_copied_blocks * block_size) / (1024 * 1024) << " MB)\n";

    } catch (const std::exception& e) {
        if (out_fd != STDOUT_FILENO) ::close(out_fd);
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
