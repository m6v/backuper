#pragma once
#include "backuper.hpp"

class ExtfsBackuper : public VolumeBackuper {
protected:
    void do_backup(const CliArgs& args, UniqueFd& src_fd) override;
};
