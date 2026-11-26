#pragma once
#include <string>
#include <memory>
#include <fcntl.h>
#include <sys/file.h>
#include "sysutil.hpp"

// Базовый абстрактный класс для всех стратегий резервного копирования разделов
class VolumeBackuper {
public:
    virtual ~VolumeBackuper() = default;

    // Шаблонный метод: проверяет монтирование, блокирует устройство и запускает бэкап
    void backup(const CliArgs& args) {
        ensure_not_mounted(args.src, true);
        UniqueFd src_fd = UniqueFd::open_locked(args.src, O_RDONLY, LOCK_SH);
        do_backup(args, src_fd);
    }

protected:
    // Реализуется конкретным наследником для своей файловой системы
    virtual void do_backup(const CliArgs& args, UniqueFd& src_fd) = 0;
};

// Определяет тип файловой системы на устройстве или в файле-образе с помощью libblkid
std::string detect_filesystem(const std::string& path);

// Фабрика: создает полиморфный объект бэкапера под определенный тип файловой системы
std::unique_ptr<VolumeBackuper> create_backuper(const std::string& fs_type);
