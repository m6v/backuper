#pragma once
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include "sysutil.hpp"
#include "progress.hpp"
#include "partimg.hpp"

// Формирует поток EXTIMAGE: склеивает занятые блоки в чанки по 4 МБ, считает CRC32 и обновляет прогресс
class ImageWriter {
public:
    ImageWriter(const CliArgs& args, uint32_t block_size, uint64_t total_blocks, uint64_t used_blocks)
        : out_fd_(open_output(args.dst), args.dst != "-"),
          block_size_(validate_block_size(block_size)),
          total_blocks_(total_blocks),
          max_blocks_per_chunk_(std::max<uint32_t>(1, (4 * 1024 * 1024) / block_size_)),
          buffer_(static_cast<size_t>(max_blocks_per_chunk_) * block_size_),
          progress_(args.progress_fd, used_blocks, block_size_) {
        ImageHeader img_hdr{};
        img_hdr.magic = IMAGE_MAGIC;
        img_hdr.version = IMAGE_VERSION;
        img_hdr.block_size = block_size_;
        img_hdr.total_blocks = total_blocks_;
        img_hdr.used_blocks = used_blocks;
        img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
        write_all(&img_hdr, sizeof(img_hdr));
    }

    // Добавляет один занятый блок (автоматически склеивает соседние блоки в чанк до 4 МБ)
    template <typename ReadFunc>
    void push_block(uint64_t blk, ReadFunc&& read_blocks) {
        if (chunk_count_ > 0 && blk != chunk_start_ + chunk_count_) {
            flush(read_blocks);
        }
        if (chunk_count_ == 0) {
            chunk_start_ = blk;
        }
        chunk_count_++;
        if (chunk_count_ == max_blocks_per_chunk_) {
            flush(read_blocks);
        }
    }

    // Записывает готовый непрерывный диапазон блоков произвольной длины (безопасно режет по 4 МБ)
    template <typename ReadFunc>
    void write_range(uint64_t start, uint64_t count, ReadFunc&& read_blocks) {
        flush(read_blocks);
        while (count > 0) {
            uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(max_blocks_per_chunk_, count));
            chunk_start_ = start;
            chunk_count_ = step;
            flush(read_blocks);
            start += step;
            count -= step;
        }
    }

    // Сбрасывает остаток буфера, пишет маркер EOF, закрывает поток и выводит итог
    template <typename ReadFunc>
    void finish(ReadFunc&& read_blocks, const char* unit_name = "blocks") {
        flush(read_blocks);

        ChunkHeader eof_hdr{};
        eof_hdr.start_block = 0;
        eof_hdr.block_count = 0;
        eof_hdr.header_crc32 = compute_crc32(&eof_hdr, offsetof(ChunkHeader, header_crc32));
        eof_hdr.data_crc32 = 0;
        write_all(&eof_hdr, sizeof(eof_hdr));

        out_fd_.reset();
        progress_.finish();

        std::cerr << "Success! Copied " << copied_blocks_ << " of " << total_blocks_
                  << " " << unit_name << " (" << (copied_blocks_ * block_size_) / (1024 * 1024) << " MB)\n";
    }

private:
    static int open_output(const std::string& dst) {
        if (dst == "-") return STDOUT_FILENO;
        int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            throw std::runtime_error("Cannot open output '" + dst + "': " + std::string(strerror(errno)));
        }
        return fd;
    }

    static uint32_t validate_block_size(uint32_t block_size) {
        if (block_size == 0) {
            throw std::runtime_error("Invalid filesystem block size (0)");
        }
        return block_size;
    }

    void write_all(const void* buf, size_t count) {
        const char* ptr = static_cast<const char*>(buf);
        io_exact(count, [&](size_t off, size_t rem) {
            return ::write(out_fd_.get(), ptr + off, rem);
        });
    }

    template <typename ReadFunc>
    void flush(ReadFunc&& read_blocks) {
        if (chunk_count_ == 0) return;

        size_t bytes = static_cast<size_t>(chunk_count_) * block_size_;
        read_blocks(chunk_start_, chunk_count_, buffer_.data());

        ChunkHeader chunk_hdr{};
        chunk_hdr.start_block = chunk_start_;
        chunk_hdr.block_count = chunk_count_;
        chunk_hdr.header_crc32 = compute_crc32(&chunk_hdr, offsetof(ChunkHeader, header_crc32));
        chunk_hdr.data_crc32 = compute_crc32(buffer_.data(), bytes);

        write_all(&chunk_hdr, sizeof(chunk_hdr));
        write_all(buffer_.data(), bytes);

        copied_blocks_ += chunk_count_;
        progress_.update(copied_blocks_);
        chunk_count_ = 0;
    }

    UniqueFd out_fd_;
    uint32_t block_size_;
    uint64_t total_blocks_;
    uint32_t max_blocks_per_chunk_;
    std::vector<char> buffer_;
    ProgressReporter progress_;

    uint64_t chunk_start_ = 0;
    uint32_t chunk_count_ = 0;
    uint64_t copied_blocks_ = 0;
};
