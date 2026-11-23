#include <iostream>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include "imgwriter.hpp"
#include <ext2fs/ext2fs.h>

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

// Чтение занятых блоков ext2/ext3/ext4 и передача их в общий ImageWriter
void backup_volume(const CliArgs& args) {
    Ext2Volume vol(args.src);
    ext2_filsys fs = vol.handle();

    UniqueFd lock_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (lock_fd && ::flock(lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source device '" + args.src + "' is locked by an exclusive operation!");
    }

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

    auto read_blocks = [&](uint64_t start, uint32_t count, char* dst) {
        if (io_channel_read_blk64(fs->io, start, count, dst)) {
            throw std::runtime_error("Read error at block " + std::to_string(start));
        }
    };

    ImageWriter writer(args, block_size, total_blocks, total_used_blocks);
    for (blk64_t blk = first_block; blk < total_blocks; ++blk) {
        if (ext2fs_test_block_bitmap2(fs->block_map, blk)) {
            writer.push_block(blk, read_blocks);
        }
    }
    writer.finish(read_blocks);
}

// Точка входа: разбирает аргументы, запускает бэкап extfs и перехватывает ошибки
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
