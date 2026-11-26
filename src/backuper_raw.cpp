#include "backuper_raw.hpp"
#include "imgwriter.hpp"
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

void RawBackuper::do_backup(const CliArgs& args, UniqueFd& src_fd) {
    struct stat st{};
    if (::fstat(src_fd.get(), &st) != 0) {
        throw std::runtime_error("Cannot stat '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) {
        throw std::runtime_error("Source '" + args.src + "' must be a block device or a regular file!");
    }

    ::posix_fadvise(src_fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);

    uint64_t total_bytes = 0;
    if (S_ISBLK(st.st_mode)) {
        if (::ioctl(src_fd.get(), BLKGETSIZE64, &total_bytes) != 0) {
            throw std::runtime_error("Cannot get block device size: " + std::string(strerror(errno)));
        }
    } else {
        total_bytes = static_cast<uint64_t>(st.st_size);
    }

    if (total_bytes == 0) {
        throw std::runtime_error("Source '" + args.src + "' is empty (0 bytes)!");
    }

    uint32_t block_size = 4096;
    if (total_bytes % 4096 != 0) {
        block_size = (total_bytes % 512 == 0) ? 512 : 1;
    }

    uint64_t total_blocks = total_bytes / block_size;

    auto read_blocks = [&](uint64_t start, uint32_t count, char* dst) {
        size_t bytes = static_cast<size_t>(count) * block_size;
        io_exact(bytes, [&](size_t off, size_t rem) {
            return ::pread(src_fd.get(), dst + off, rem, static_cast<off_t>(start) * block_size + off);
        });
    };

    ImageWriter writer(args, block_size, total_blocks, total_blocks);
    writer.write_range(0, total_blocks, read_blocks);
    writer.finish(read_blocks);
}
