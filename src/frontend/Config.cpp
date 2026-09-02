#include "frontend/Config.hpp"
#include "common/Log.hpp"
#include <fstream>
#include <sstream>

namespace ps96 {

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "bios") bios_path = val;
        else if (key == "disc") disc_path = val;
        else if (key == "memcard0") memcard0_path = val;
        else if (key == "memcard1") memcard1_path = val;
        else if (key == "fullscreen") fullscreen = (val == "1" || val == "true");
        else if (key == "vsync") vsync = (val == "1" || val == "true");
        else if (key == "scale") scale = std::stoi(val);
        else if (key == "limit_fps") limit_fps = (val == "1" || val == "true");
        else if (key == "skip_ps_boot") skip_ps_boot = (val == "1" || val == "true");
        else if (key == "video_mode") video_mode = (val == "1" || val == "overclocked") ? VideoMode::Overclocked60 : VideoMode::Native240p;
        else if (key == "theme") {
            if (val == "plasma" || val == "1") theme = Theme::Plasma;
            else if (val == "crimson" || val == "2") theme = Theme::Crimson;
            else if (val == "carbon" || val == "3") theme = Theme::Carbon;
            else if (val == "sunset" || val == "4") theme = Theme::Sunset;
            else theme = Theme::Neon;
        }
    }
    return true;
}

bool Config::save(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "bios=" << bios_path << "\n";
    f << "disc=" << disc_path << "\n";
    f << "memcard0=" << memcard0_path << "\n";
    f << "memcard1=" << memcard1_path << "\n";
    f << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    f << "vsync=" << (vsync ? 1 : 0) << "\n";
    f << "scale=" << scale << "\n";
    f << "limit_fps=" << (limit_fps ? 1 : 0) << "\n";
    f << "skip_ps_boot=" << (skip_ps_boot ? 1 : 0) << "\n";
    f << "video_mode=" << (video_mode == VideoMode::Overclocked60 ? "overclocked" : "native") << "\n";
    const char* tn = theme == Theme::Plasma ? "plasma" : theme == Theme::Crimson ? "crimson" : theme == Theme::Carbon ? "carbon" : theme == Theme::Sunset ? "sunset" : "neon";
    f << "theme=" << tn << "\n";
    return true;
}

} // namespace ps96
