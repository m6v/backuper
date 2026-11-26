#pragma once
#include "backuper.hpp"

class NtfsBackuper : public VolumeBackuper {
protected:
    void do_backup(const CliArgs& args, UniqueFd& src_fd) override;
};
