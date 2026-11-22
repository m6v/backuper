#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <sys/file.h>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <ext2fs/ext2fs.h>
#include "partimg.hpp"

// Запись count байт из буфера в файл или пайп
void write_exact(int fd, const void* buf, size_t count) {
    const char* ptr = static_cast<const char*>(buf);
    io_exact(count, [&](size_t off, size_t rem) {
        return ::write(fd, ptr + off, rem);
    });
}

// RAII-обертка над дескриптором файловой системы libext2fs
class Ext2Volume {
public:
    // Проверка, что ФС не смонтирована в rw, открытие её и загрузка карты занятых блоков
    explicit Ext2Volume(const std::string& device_path) {
        ensure_not_mounted(device_path, true);

        errcode_t err = ext2fs_open(
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

    // Освобождение структуры файловой системы и закрытие устройства
    ~Ext2Volume() {
        if (fs_) ext2fs_close(fs_);
    }

    Ext2Volume(const Ext2Volume&) = delete;
    Ext2Volume& operator=(const Ext2Volume&) = delete;

    // Возврат указателя на открытую структуру ext2_filsys
    ext2_filsys handle() const { return fs_; }

private:
    ext2_filsys fs_ = nullptr;
};

// Чтение занятых блоков ФС, объединение их в чанки до 4 МБ и запись в поток с CRC32
void backup_volume(const CliArgs& args) {
    bool is_stdout = (args.dst == "-");
    UniqueFd out_fd(
        is_stdout ? STDOUT_FILENO : ::open(args.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644),
        !is_stdout
    );
    if (!out_fd) {
        throw std::runtime_error("Cannot open output '" + args.dst + "': " + std::string(strerror(errno)));
    }

    Ext2Volume vol(args.src);
    
    UniqueFd lock_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (lock_fd && ::flock(lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source device '" + args.src + "' is locked by an exclusive operation!");
    }
    
    ext2_filsys fs = vol.handle();

    if (fs->super->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        throw std::runtime_error("Filesystem has dirty journal (needs recovery). Run e2fsck or mount/umount cleanly first.");
    }
    if (fs->super->s_state & EXT2_ERROR_FS) {
        std::cerr << "Warning: Filesystem has errors marked in superblock!\n";
    }

    blk64_t total_blocks      = ext2fs_blocks_count(fs->super);
    blk64_t total_used_blocks = total_blocks - ext2fs_free_blocks_count(fs->super);
    blk64_t first_block       = fs->super->s_first_data_block;
    uint32_t block_size       = fs->blocksize;

    ImageHeader img_hdr{};
    img_hdr.magic = IMAGE_MAGIC;
    img_hdr.version = IMAGE_VERSION;
    img_hdr.block_size = block_size;
    img_hdr.total_blocks = total_blocks;
    img_hdr.used_blocks = total_used_blocks;
    img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
    write_exact(out_fd.get(), &img_hdr, sizeof(img_hdr));

    ProgressReporter progress(args.progress_fd, total_used_blocks, block_size);

    const size_t MAX_CHUNK_BYTES = 4 * 1024 * 1024;
    const uint32_t max_blocks_per_chunk = std::max<uint32_t>(1, MAX_CHUNK_BYTES / block_size);
    std::vector<char> buffer(max_blocks_per_chunk * block_size);

    blk64_t total_copied_blocks = 0;

    // Чтение непрерывного диапазона используемых блоков с диска, расчет CRC32 и отправка в выходной поток
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

        write_exact(out_fd.get(), &chunk_hdr, sizeof(chunk_hdr));
        write_exact(out_fd.get(), buffer.data(), bytes_to_read);

        total_copied_blocks += count;
        progress.update(total_copied_blocks);
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

    ChunkHeader eof_hdr{};
    eof_hdr.start_block = 0;
    eof_hdr.block_count = 0;
    eof_hdr.header_crc32 = compute_crc32(&eof_hdr, offsetof(ChunkHeader, header_crc32));
    eof_hdr.data_crc32 = 0;
    write_exact(out_fd.get(), &eof_hdr, sizeof(eof_hdr));

    out_fd.reset();
    progress.finish();

    std::cerr << "Success! Copied " << total_copied_blocks << " of " << total_blocks
              << " blocks (" << (total_copied_blocks * block_size) / (1024 * 1024) << " MB)\n";
}


int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Fast block-level backup tool for ext2/ext3/ext4 filesystems",
            "<source_device> <output_file|->"
        );
        backup_volume(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
