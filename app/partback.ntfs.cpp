#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include "partimg.hpp"

// Подключаем C-заголовки ntfs-3g ПОСЛЕ всех заголовков C++ и убираем макросы min/max
extern "C" {
#include <ntfs-3g/volume.h>
#include <ntfs-3g/attrib.h>
}

// Запись count байт из буфера в файл или пайп
void write_exact(int fd, const void* buf, size_t count) {
    const char* ptr = static_cast<const char*>(buf);
    io_exact(count, [&](size_t off, size_t rem) {
        return ::write(fd, ptr + off, rem);
    });
}

// RAII-обертка над дескриптором тома libntfs-3g
class NtfsVolume {
public:
    // Монтирование тома NTFS в режиме только чтение и проверка его состояния
    explicit NtfsVolume(const std::string& device_path) {
        vol_ = ntfs_mount(device_path.c_str(), NTFS_MNT_RDONLY);
        if (!vol_) {
            throw std::runtime_error(
                "ntfs_mount error on '" + device_path + "': " + std::string(strerror(errno))
            );
        }

        if (vol_->flags & VOLUME_IS_DIRTY) {
            ntfs_umount(vol_, FALSE);
            throw std::runtime_error(
                "NTFS volume is marked dirty or Windows Fast Startup / hibernation is active! "
                "Boot Windows and run 'shutdown /s /t 0' or run 'ntfsfix' first."
            );
        }

        if (!vol_->lcnbmp_na) {
            ntfs_umount(vol_, FALSE);
            throw std::runtime_error("NTFS cluster bitmap ($Bitmap) attribute is missing!");
        }
    }

    // Размонтирование тома и освобождение ресурсов libntfs-3g
    ~NtfsVolume() {
        if (vol_) {
            ntfs_umount(vol_, FALSE);
        }
    }

    NtfsVolume(const NtfsVolume&) = delete;
    NtfsVolume& operator=(const NtfsVolume&) = delete;

    // Возврат указателя на открытую структуру ntfs_volume
    ntfs_volume* handle() const { return vol_; }

private:
    ntfs_volume* vol_ = nullptr;
};

// Чтение занятых кластеров NTFS через $Bitmap, объединение в чанки до 4 МБ и запись с CRC32
void backup_ntfs_volume(const CliArgs& args) {
    ensure_not_mounted(args.src, true);

    UniqueFd lock_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (!lock_fd) {
        throw std::runtime_error("Cannot open source '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (::flock(lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source '" + args.src + "' is locked by an exclusive operation!");
    }

    bool is_stdout = (args.dst == "-");
    UniqueFd out_fd(
        is_stdout ? STDOUT_FILENO : ::open(args.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644),
        !is_stdout
    );
    if (!out_fd) {
        throw std::runtime_error("Cannot open output '" + args.dst + "': " + std::string(strerror(errno)));
    }

    NtfsVolume ntfs(args.src);
    ntfs_volume* vol = ntfs.handle();

    uint64_t total_clusters = static_cast<uint64_t>(vol->nr_clusters);
    uint32_t cluster_size   = vol->cluster_size;

    // Загружаем битовую карту занятости кластеров ($Bitmap) в оперативную память
    size_t bitmap_bytes = static_cast<size_t>((total_clusters + 7) / 8);
    std::vector<uint8_t> bitmap(bitmap_bytes, 0);

    s64 br = ntfs_attr_pread(vol->lcnbmp_na, 0, static_cast<s64>(bitmap_bytes), bitmap.data());
    if (br != static_cast<s64>(bitmap_bytes)) {
        throw std::runtime_error("Failed to read full NTFS $Bitmap attribute");
    }

    // Проверка занятости кластера по индексу lcn в битовой карте
    auto is_cluster_used = [&](uint64_t lcn) -> bool {
        return (bitmap[lcn >> 3] & (1U << (lcn & 7))) != 0;
    };

    // Подсчет общего числа занятых кластеров для прогресс-бара
    uint64_t total_used_clusters = 0;
    for (uint64_t lcn = 0; lcn < total_clusters; ++lcn) {
        if (is_cluster_used(lcn)) {
            total_used_clusters++;
        }
    }

    ImageHeader img_hdr{};
    img_hdr.magic = IMAGE_MAGIC;
    img_hdr.version = IMAGE_VERSION;
    img_hdr.block_size = cluster_size;
    img_hdr.total_blocks = total_clusters;
    img_hdr.used_blocks = total_used_clusters;
    img_hdr.header_crc32 = compute_crc32(&img_hdr, offsetof(ImageHeader, header_crc32));
    write_exact(out_fd.get(), &img_hdr, sizeof(img_hdr));

    ProgressReporter progress(args.progress_fd, total_used_clusters, cluster_size);

    constexpr size_t MAX_CHUNK_BYTES = 4 * 1024 * 1024;
    const uint32_t max_clusters_per_chunk = std::max<uint32_t>(1, MAX_CHUNK_BYTES / cluster_size);
    std::vector<char> buffer(max_clusters_per_chunk * cluster_size);

    uint64_t total_copied_clusters = 0;

    // Чтение непрерывного диапазона занятых кластеров с диска, расчет CRC32 и запись в поток
    auto flush_chunk = [&](uint64_t start, uint32_t count) {
        if (count == 0) return;

        size_t bytes_to_read = static_cast<size_t>(count) * cluster_size;
        s64 read_clusters = ntfs_cluster_read(
            vol, static_cast<LCN>(start), static_cast<s64>(count), buffer.data()
        );
        if (read_clusters != static_cast<s64>(count)) {
            throw std::runtime_error("NTFS cluster read error at cluster " + std::to_string(start));
        }

        ChunkHeader chunk_hdr{};
        chunk_hdr.start_block = start;
        chunk_hdr.block_count = count;
        chunk_hdr.header_crc32 = compute_crc32(&chunk_hdr, offsetof(ChunkHeader, header_crc32));
        chunk_hdr.data_crc32 = compute_crc32(buffer.data(), bytes_to_read);

        write_exact(out_fd.get(), &chunk_hdr, sizeof(chunk_hdr));
        write_exact(out_fd.get(), buffer.data(), bytes_to_read);

        total_copied_clusters += count;
        progress.update(total_copied_clusters);
    };

    uint64_t chunk_start = 0;
    uint32_t chunk_count = 0;

    for (uint64_t lcn = 0; lcn < total_clusters; ++lcn) {
        if (is_cluster_used(lcn)) {
            if (chunk_count == 0) chunk_start = lcn;
            chunk_count++;

            if (chunk_count == max_clusters_per_chunk) {
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

    std::cerr << "Success! Copied " << total_copied_clusters << " of " << total_clusters
              << " clusters (" << (total_copied_clusters * cluster_size) / (1024 * 1024) << " MB)\n";
}

// Точка входа: разбирает аргументы, запускает бэкап NTFS и перехватывает ошибки
int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Fast cluster-level backup tool for NTFS filesystems",
            "<source_device> <output_file|->"
        );
        backup_ntfs_volume(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
