#pragma once
#include <string>

struct SrGuiOptions {
    std::string recipes_dir, out_dir, instruments_dir;
    std::string pick;
    bool browse = false, build = false;
};

int sr_gui_run(const SrGuiOptions& opt);
