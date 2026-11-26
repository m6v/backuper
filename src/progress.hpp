#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include "sysutil.hpp"

// Управление выводом прогресса и скорости (МБ/с) в заданный FD (для whiptail/dialog)
class ProgressReporter {
public:
    ProgressReporter(int fd, uint64_t total_blocks, uint32_t block_size)
        : fd_(fd, fd >= 0 && fd != STDOUT_FILENO && fd != STDERR_FILENO),
          total_(total_blocks),
          block_size_(block_size),
          start_time_(std::chrono::steady_clock::now()),
          last_update_(start_time_) {
        if (fd_) {
            write_status(0, start_time_);
        }
    }

    // Периодический (не чаще раза в 250 мс) расчет процента и средней скорости
    void update(uint64_t current_blocks) {
        if (!fd_ || total_ == 0) return;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count() >= 250) {
            write_status(current_blocks, now);
            last_update_ = now;
        }
    }

    // Отправка финальных 100% и закрытие FD, чтобы внешний диалог получил EOF
    void finish() {
        if (fd_) {
            write_status(total_, std::chrono::steady_clock::now());
            fd_.reset();
        }
    }

private:
    void write_status(uint64_t current_blocks, std::chrono::steady_clock::time_point now) {
        int pct = (total_ > 0) ? static_cast<int>((current_blocks * 100) / total_) : 100;
        if (pct > 100) pct = 100;

        double total_sec = std::chrono::duration<double>(now - start_time_).count();
        double copied_mb_exact = (static_cast<double>(current_blocks) * block_size_) / (1024.0 * 1024.0);
        double speed_mbps = (total_sec > 0.001) ? (copied_mb_exact / total_sec) : 0.0;

        uint64_t copied_mb = (current_blocks * block_size_) / (1024 * 1024);
        uint64_t total_mb  = (total_ * block_size_) / (1024 * 1024);

        std::array<char, 256> buf{};
        int len = std::snprintf(
            buf.data(), buf.size(),
            "XXX\n%d\nОбработано: %llu / %llu МБ | Скорость: %.1f МБ/с\nXXX\n",
            pct,
            static_cast<unsigned long long>(copied_mb),
            static_cast<unsigned long long>(total_mb),
            speed_mbps
        );
        if (len > 0) {
            ssize_t ignored = ::write(fd_.get(), buf.data(), len);
            (void)ignored;
        }
    }

    UniqueFd fd_;
    uint64_t total_ = 0;
    uint32_t block_size_ = 4096;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_;
};
