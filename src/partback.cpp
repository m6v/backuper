#include <iostream>
#include <string>
#include <memory>
#include <stdexcept>
#include <blkid/blkid.h>

#include "sysutil.hpp"
#include "backuper.hpp"
#include "backuper_extfs.hpp"
#include "backuper_ntfs.hpp"
#include "backuper_exfat.hpp"
#include "backuper_raw.hpp"

// Определяет тип файловой системы через libblkid (возвращает "unknown", если сигнатура не найдена)
std::string detect_filesystem(const std::string& path) {
    blkid_probe pr = blkid_new_probe_from_filename(path.c_str());
    if (!pr) {
        throw std::runtime_error("Cannot open '" + path + "' for filesystem detection");
    }

    // Гарантированное освобождение blkid_probe при выходе из функции (RAII)
    std::unique_ptr<std::remove_pointer<blkid_probe>::type, decltype(&blkid_free_probe)>
        probe_guard(pr, &blkid_free_probe);

    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE);

    int rc = blkid_do_safeprobe(pr);
    if (rc < 0) {
        throw std::runtime_error("libblkid probe error on '" + path + "'");
    }
    if (rc == 1) {
        return "unknown";
    }

    const char* fs_type = nullptr;
    if (blkid_probe_lookup_value(pr, "TYPE", &fs_type, nullptr) == 0 && fs_type) {
        return std::string(fs_type);
    }
    return "unknown";
}

// Фабрика: подбирает оптимальную стратегию резервного копирования под тип ФС
std::unique_ptr<VolumeBackuper> create_backuper(const std::string& fs_type) {
    if (fs_type == "ext2" || fs_type == "ext3" || fs_type == "ext4") {
        return std::make_unique<ExtfsBackuper>();
    }
    if (fs_type == "ntfs") {
        return std::make_unique<NtfsBackuper>();
    }
    if (fs_type == "exfat") {
        return std::make_unique<ExfatBackuper>();
    }
    return std::make_unique<RawBackuper>();
}

int main(int argc, char* argv[]) {
    try {
        CliArgs args = parse_cli_args(
            argc, argv,
            "Smart block-level partition backup tool (ext2/3/4, NTFS, exFAT, raw)",
            "<source_device> <output_file|->"
        );

        std::string fs_type = detect_filesystem(args.src);
        std::cerr << "Detected filesystem: " << fs_type << "\n";

        std::unique_ptr<VolumeBackuper> backuper = create_backuper(fs_type);
        backuper->backup(args);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
