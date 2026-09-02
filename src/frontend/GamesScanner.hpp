#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>

namespace ps96 {

struct GameEntry {
    std::string folder_name;   // e.g. "Fifa Soccer 2005"
    std::string folder_path;   // absolute or relative path to the folder
    std::string cue_path;      // preferred
    std::string bin_path;
    std::string cover_path;    // .png / .bmp / .jpg if present
    bool has_cover = false;
    bool has_disc  = false;
};

class GamesScanner {
public:
    // Scan <base>/Games/* for folders containing .cue/.bin (+ optional cover)
    static std::vector<GameEntry> scan(const std::string& base_dir);

    // Resolve executable directory (folder containing the .exe / binary)
    static std::string executable_dir();

    // Try common BIOS names next to exe and in ./bios/
    static std::string find_bios(const std::string& base_dir);
};

} // namespace ps96
