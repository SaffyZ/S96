#pragma once

#include "common/Types.hpp"
#include <string>

namespace ps96 {

enum class VideoMode { Native240p = 0, Overclocked60 = 1 };
enum class Theme { Neon = 0, Plasma = 1, Crimson = 2, Carbon = 3, Sunset = 4 };

struct Config {
    std::string bios_path;
    std::string disc_path;
    std::string memcard0_path = "memcard0.mcd";
    std::string memcard1_path = "memcard1.mcd";
    bool fullscreen = false;
    bool vsync = true;
    int scale = 2;
    // Legacy setting retained for config compatibility. Emulation speed is
    // now governed by the PS1 CPU clock, not a host FPS limiter.
    bool limit_fps = false;
    // Skip BIOS intro when launching a game (load EXE from SYSTEM.CNF and jump)
    bool skip_ps_boot = false; // default: execute the real BIOS boot path
    VideoMode video_mode = VideoMode::Native240p;
    Theme theme = Theme::Neon;

    bool load(const std::string& path);
    bool save(const std::string& path);
};

} // namespace ps96
