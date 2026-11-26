#include "backuper_extfs.hpp"
#include "imgwriter.hpp"
#include <stdexcept>
#include <iostream>
#include <ext2fs/ext2fs.h>

namespace {

// RAII-обертка над дескриптором файловой системы libext2fs
class Ext2Volume {
public:
    explicit Ext2Volume(const std::string& device_path) {
        initialize_ext2_error_table();

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

    ~Ext2Volume() {
        if (fs_) ext2fs_close(fs_);
    }

    Ext2Volume(const Ext2Volume&) = delete;
    Ext2Volume& operator=(const Ext2Volume&) = delete;

    ext2_filsys handle() const { return fs_; }

private:
    ext2_filsys fs_ = nullptr;
};

} // namespace

void ExtfsBackuper::do_backup(const CliArgs& args, UniqueFd& /*src_fd*/) {
    Ext2Volume vol(args.src);
    ext2_filsys fs = vol.handle();

    if (fs->super->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        throw std::runtime_error(
            "Filesystem has dirty journal (needs recovery). Run e2fsck or mount/umount cleanly first."
        );
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
