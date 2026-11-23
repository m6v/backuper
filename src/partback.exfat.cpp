#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include "imgwriter.hpp"

#pragma pack(push, 1)
struct ExfatBootSector {
    uint8_t  jump_boot[3];
    char     fs_name[8];               // "EXFAT   "
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;            // Общее число секторов
    uint32_t fat_offset;               // Смещение FAT в секторах
    uint32_t fat_length;               // Длина FAT в секторах
    uint32_t cluster_heap_offset;      // Начало кучи кластеров в секторах
    uint32_t cluster_count;            // Общее число кластеров данных
    uint32_t first_cluster_of_root;    // Первый кластер корневого каталога (>= 2)
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;             // Бит 1 (0x0002) = VolumeDirty
    uint8_t  bytes_per_sector_shift;   // 2^N байт на сектор (обычно 9 -> 512)
    uint8_t  sectors_per_cluster_shift;// 2^N секторов на кластер
    uint8_t  number_of_fats;           // 1 или 2
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
};

struct ExfatBitmapEntry {
    uint8_t  entry_type;               // 0x81 = Allocation Bitmap
    uint8_t  bitmap_flags;             // Бит 0: 0 = первая FAT, 1 = вторая FAT
    uint8_t  reserved[18];
    uint32_t first_cluster;            // Начальный кластер битмапа
    uint64_t data_length;              // Размер битмапа в байтах
};
#pragma pack(pop)

// Чтение count байт с диска по заданному смещению (pread)
void pread_exact(int fd, void* buf, size_t count, off_t offset) {
    char* ptr = static_cast<char*>(buf);
    io_exact(count, [&](size_t off, size_t rem) {
        return ::pread(fd, ptr + off, rem, offset + off);
    });
}

// Чтение занятых кластеров exFAT через запись 0x81 (Allocation Bitmap) и передача в ImageWriter
void backup_exfat_volume(const CliArgs& args) {
    ensure_not_mounted(args.src, true);

    UniqueFd in_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (!in_fd) {
        throw std::runtime_error("Cannot open source '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (::flock(in_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source '" + args.src + "' is locked by an exclusive operation!");
    }
    ::posix_fadvise(in_fd.get(), 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);

    ExfatBootSector vbr{};
    pread_exact(in_fd.get(), &vbr, sizeof(vbr), 0);

    if (std::memcmp(vbr.fs_name, "EXFAT   ", 8) != 0) {
        throw std::runtime_error("Invalid exFAT signature (not an exFAT volume)");
    }
    if (vbr.volume_flags & 0x0002) {
        throw std::runtime_error("exFAT volume is marked dirty (VolumeDirty)! Run fsck.exfat first.");
    }
    if (vbr.bytes_per_sector_shift < 9 || vbr.bytes_per_sector_shift > 12 ||
        vbr.sectors_per_cluster_shift > 25) {
        throw std::runtime_error("Corrupted exFAT geometry in boot sector");
    }

    const uint64_t sector_size  = 1ULL << vbr.bytes_per_sector_shift;
    const uint32_t cluster_size = static_cast<uint32_t>(sector_size << vbr.sectors_per_cluster_shift);
    const uint64_t heap_offset_bytes = static_cast<uint64_t>(vbr.cluster_heap_offset) * sector_size;

    auto cluster_to_offset = [&](uint32_t cluster) -> off_t {
        if (cluster < 2 || cluster >= vbr.cluster_count + 2) {
            throw std::runtime_error("exFAT cluster index out of range: " + std::to_string(cluster));
        }
        return static_cast<off_t>(heap_offset_bytes + static_cast<uint64_t>(cluster - 2) * cluster_size);
    };

    // Поиск записи 0x81 (Allocation Bitmap) в корневом каталоге
    std::vector<uint32_t> fat(vbr.cluster_count + 2, 0);
    pread_exact(in_fd.get(), fat.data(), fat.size() * sizeof(uint32_t),
                static_cast<off_t>(vbr.fat_offset) * sector_size);

    ExfatBitmapEntry bmp_entry{};
    bool found_bitmap = false;
    std::vector<uint8_t> dir_cluster(cluster_size);

    uint32_t cur_dir_cluster = vbr.first_cluster_of_root;
    uint32_t guard = 0;
    while (cur_dir_cluster >= 2 && cur_dir_cluster < 0xFFFFFFF8U && !found_bitmap) {
        if (++guard > vbr.cluster_count) {
            throw std::runtime_error("Infinite cluster loop in exFAT root directory");
        }
        pread_exact(in_fd.get(), dir_cluster.data(), cluster_size, cluster_to_offset(cur_dir_cluster));

        for (size_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t type = dir_cluster[off];
            if (type == 0x00) break;
            if (type == 0x81 && (dir_cluster[off + 1] & 0x01) == 0) {
                std::memcpy(&bmp_entry, &dir_cluster[off], sizeof(ExfatBitmapEntry));
                found_bitmap = true;
                break;
            }
        }
        cur_dir_cluster = fat[cur_dir_cluster];
    }

    if (!found_bitmap) {
        throw std::runtime_error("exFAT Allocation Bitmap entry (0x81) not found in root directory");
    }

    // Чтение битовой карты по цепочке кластеров из FAT
    size_t expected_bmp_bytes = (vbr.cluster_count + 7) / 8;
    if (bmp_entry.data_length < expected_bmp_bytes) {
        throw std::runtime_error("exFAT Allocation Bitmap is smaller than cluster count");
    }

    std::vector<uint8_t> bitmap(expected_bmp_bytes, 0);
    size_t bmp_read = 0;
    uint32_t bmp_cluster = bmp_entry.first_cluster;
    guard = 0;

    while (bmp_read < expected_bmp_bytes && bmp_cluster >= 2 && bmp_cluster < 0xFFFFFFF8U) {
        if (++guard > vbr.cluster_count) {
            throw std::runtime_error("Infinite cluster loop in exFAT Allocation Bitmap");
        }
        size_t to_copy = std::min<size_t>(cluster_size, expected_bmp_bytes - bmp_read);
        pread_exact(in_fd.get(), bitmap.data() + bmp_read, to_copy, cluster_to_offset(bmp_cluster));
        bmp_read += to_copy;
        bmp_cluster = fat[bmp_cluster];
    }

    if (bmp_read < expected_bmp_bytes) {
        throw std::runtime_error("Truncated exFAT Allocation Bitmap chain");
    }

    // Приведение тома к сетке блоков 4096 байт (или размеру кластера, если он < 4096)
    const uint32_t block_size = std::min<uint32_t>(4096, cluster_size);
    const uint64_t total_volume_bytes = vbr.volume_length * sector_size;
    const uint64_t total_blocks = total_volume_bytes / block_size;
    const uint64_t heap_start_block = heap_offset_bytes / block_size;
    const uint64_t blocks_per_cluster = cluster_size / block_size;

    auto is_block_used = [&](uint64_t blk) -> bool {
        if (blk < heap_start_block) return true;
        uint64_t cluster_idx = (blk - heap_start_block) / blocks_per_cluster;
        if (cluster_idx >= vbr.cluster_count) return false;
        return (bitmap[cluster_idx >> 3] & (1U << (cluster_idx & 7))) != 0;
    };

    uint64_t total_used_blocks = 0;
    for (uint64_t blk = 0; blk < total_blocks; ++blk) {
        if (is_block_used(blk)) total_used_blocks++;
    }

    auto read_blocks = [&](uint64_t start, uint32_t count, char* dst) {
        pread_exact(in_fd.get(), dst, static_cast<size_t>(count) * block_size,
                    static_cast<off_t>(start) * block_size);
    };

    ImageWriter writer(args, block_size, total_blocks, total_used_blocks);
    for (uint64_t blk = 0; blk < total_blocks; ++blk) {
        if (is_block_used(blk)) {
            writer.push_block(blk, read_blocks);
        }
    }
    writer.finish(read_blocks);
}

// Точка входа: разбирает аргументы, запускает бэкап exFAT и перехватывает ошибки
int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Fast cluster-level backup tool for exFAT filesystems",
            "<source_device> <output_file|->"
        );
        backup_exfat_volume(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
