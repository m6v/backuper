#include "backuper_ntfs.hpp"
#include "imgwriter.hpp"
#include <vector>
#include <stdexcept>
#include <cstring>
#include <unistd.h>

extern "C" {
#include <ntfs-3g/volume.h>
#include <ntfs-3g/attrib.h>
}

namespace {

// RAII-обертка над дескриптором тома libntfs-3g
class NtfsVolume {
public:
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

    ~NtfsVolume() {
        if (vol_) ntfs_umount(vol_, FALSE);
    }

    NtfsVolume(const NtfsVolume&) = delete;
    NtfsVolume& operator=(const NtfsVolume&) = delete;

    ntfs_volume* handle() const { return vol_; }

private:
    ntfs_volume* vol_ = nullptr;
};

} // namespace

void NtfsBackuper::do_backup(const CliArgs& args, UniqueFd& src_fd) {
    NtfsVolume ntfs(args.src);
    ntfs_volume* vol = ntfs.handle();

    uint64_t data_clusters = static_cast<uint64_t>(vol->nr_clusters);
    uint32_t cluster_size  = vol->cluster_size;

    // Загружаем битовую карту занятых кластеров ($Bitmap) в ОЗУ
    size_t bitmap_bytes = static_cast<size_t>((data_clusters + 7) / 8);
    std::vector<uint8_t> bitmap(bitmap_bytes, 0);

    s64 br = ntfs_attr_pread(vol->lcnbmp_na, 0, static_cast<s64>(bitmap_bytes), bitmap.data());
    if (br != static_cast<s64>(bitmap_bytes)) {
        throw std::runtime_error("Failed to read full NTFS $Bitmap attribute");
    }

    auto is_cluster_used = [&](uint64_t lcn) -> bool {
        return (bitmap[lcn >> 3] & (1U << (lcn & 7))) != 0;
    };

    // +1 дополнительный блок в самом конце раздела для резервного загрузочного сектора NTFS (Alternate VBR)
    uint64_t total_blocks = data_clusters + 1;
    uint64_t total_used_blocks = 1; // Учитываем финальный блок с резервным VBR
    for (uint64_t lcn = 0; lcn < data_clusters; ++lcn) {
        if (is_cluster_used(lcn)) total_used_blocks++;
    }

    auto read_blocks = [&](uint64_t start, uint32_t count, char* dst) {
        // Если диапазон затрагивает последний граничный блок за пределами nr_clusters, читаем через pread
        if (start + count > data_clusters) {
            size_t bytes = static_cast<size_t>(count) * cluster_size;
            io_exact(bytes, [&](size_t off, size_t rem) {
                return ::pread(src_fd.get(), dst + off, rem, static_cast<off_t>(start) * cluster_size + off);
            });
            return;
        }

        s64 read_cnt = ntfs_cluster_read(
            vol, static_cast<LCN>(start), static_cast<s64>(count), dst
        );
        if (read_cnt != static_cast<s64>(count)) {
            throw std::runtime_error("NTFS cluster read error at cluster " + std::to_string(start));
        }
    };

    ImageWriter writer(args, cluster_size, total_blocks, total_used_blocks);
    for (uint64_t lcn = 0; lcn < data_clusters; ++lcn) {
        if (is_cluster_used(lcn)) {
            writer.push_block(lcn, read_blocks);
        }
    }
    // Копируем последний блок раздела с резервной копией загрузочного сектора NTFS
    writer.push_block(data_clusters, read_blocks);
    writer.finish(read_blocks, "clusters");
}
