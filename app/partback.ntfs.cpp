#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include "partimg.hpp"

// Подключаем C-заголовки ntfs-3g ПОСЛЕ заголовков C++ в режиме совместимости с C
extern "C" {
#include <ntfs-3g/volume.h>
#include <ntfs-3g/attrib.h>
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

// Чтение занятых кластеров NTFS через $Bitmap и передача их в общий ImageWriter
void backup_ntfs_volume(const CliArgs& args) {
    ensure_not_mounted(args.src, true);

    UniqueFd lock_fd(::open(args.src.c_str(), O_RDONLY), true);
    if (!lock_fd) {
        throw std::runtime_error("Cannot open source '" + args.src + "': " + std::string(strerror(errno)));
    }
    if (::flock(lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
        throw std::runtime_error("Source '" + args.src + "' is locked by an exclusive operation!");
    }

    NtfsVolume ntfs(args.src);
    ntfs_volume* vol = ntfs.handle();

    uint64_t total_clusters = static_cast<uint64_t>(vol->nr_clusters);
    uint32_t cluster_size   = vol->cluster_size;

    // Загрузка битовой карты занятости кластеров ($Bitmap) в ОЗУ
    size_t bitmap_bytes = static_cast<size_t>((total_clusters + 7) / 8);
    std::vector<uint8_t> bitmap(bitmap_bytes, 0);

    s64 br = ntfs_attr_pread(vol->lcnbmp_na, 0, static_cast<s64>(bitmap_bytes), bitmap.data());
    if (br != static_cast<s64>(bitmap_bytes)) {
        throw std::runtime_error("Failed to read full NTFS $Bitmap attribute");
    }

    auto is_cluster_used = [&](uint64_t lcn) -> bool {
        return (bitmap[lcn >> 3] & (1U << (lcn & 7))) != 0;
    };

    uint64_t total_used_clusters = 0;
    for (uint64_t lcn = 0; lcn < total_clusters; ++lcn) {
        if (is_cluster_used(lcn)) total_used_clusters++;
    }

    auto read_clusters = [&](uint64_t start, uint32_t count, char* dst) {
        s64 read_cnt = ntfs_cluster_read(
            vol, static_cast<LCN>(start), static_cast<s64>(count), dst
        );
        if (read_cnt != static_cast<s64>(count)) {
            throw std::runtime_error("NTFS cluster read error at cluster " + std::to_string(start));
        }
    };

    ImageWriter writer(args, cluster_size, total_clusters, total_used_clusters);
    for (uint64_t lcn = 0; lcn < total_clusters; ++lcn) {
        if (is_cluster_used(lcn)) {
            writer.push_block(lcn, read_clusters);
        }
    }
    writer.finish(read_clusters, "clusters");
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
