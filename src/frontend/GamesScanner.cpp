#include "frontend/GamesScanner.hpp"
#include "common/Log.hpp"
#include <filesystem>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

namespace ps96 {

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

std::string GamesScanner::executable_dir() {
#ifdef _WIN32
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    fs::path p(buf);
    return p.parent_path().string();
#else
    char buf[PATH_MAX]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        return fs::path(buf).parent_path().string();
    }
    return ".";
#endif
}

std::string GamesScanner::find_bios(const std::string& base_dir) {
    // Preference order: US → EU → JP, then any SCPH*.BIN
    static const char* preferred[] = {
        "SCPH5501.BIN", "SCPH1001.BIN", "SCPH7001.BIN", "SCPH7501.BIN",
        "SCPH5502.BIN", "SCPH1002.BIN", "SCPH7002.BIN",
        "SCPH5500.BIN", "SCPH1000.BIN", "SCPH7000.BIN",
        "scph5501.bin", "scph1001.bin", "openbios.bin", "bios.bin", "SCPH.BIN"
    };

    auto try_path = [](const fs::path& p) -> std::string {
        if (fs::exists(p) && fs::is_regular_file(p) && fs::file_size(p) == 512 * 1024)
            return p.string();
        return {};
    };

    for (const char* name : preferred) {
        if (auto r = try_path(fs::path(base_dir) / name); !r.empty()) return r;
        if (auto r = try_path(fs::path(base_dir) / "bios" / name); !r.empty()) return r;
    }

    // Fallback: any 512 KiB file matching SCPH* in base or bios/
    for (const char* dir_rel : {"", "bios"}) {
        fs::path dir = dir_rel[0] ? fs::path(base_dir) / dir_rel : fs::path(base_dir);
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
        for (auto& ent : fs::directory_iterator(dir)) {
            if (!ent.is_regular_file()) continue;
            auto name = ent.path().filename().string();
            auto lower = to_lower(name);
            if ((lower.find("scph") != std::string::npos || lower.find("openbios") != std::string::npos || lower == "bios.bin") && ends_with_ci(name, ".bin")) {
                if (ent.file_size() == 512 * 1024)
                    return ent.path().string();
            }
        }
    }
    return {};
}

std::vector<GameEntry> GamesScanner::scan(const std::string& base_dir) {
    std::vector<GameEntry> out;
    // Prefer Games next to the exe; also accept parent/Games so running from
    // build/ still finds ../Games without copying dumps every rebuild.
    fs::path games_root = fs::path(base_dir) / "Games";
    if (!fs::exists(games_root) || !fs::is_directory(games_root)) {
        fs::path alt = fs::path(base_dir).parent_path() / "Games";
        if (fs::exists(alt) && fs::is_directory(alt))
            games_root = alt;
    }
    if (!fs::exists(games_root) || !fs::is_directory(games_root)) {
        Log::info("Games folder not found at: %s (create it and add game folders)", games_root.string().c_str());
        return out;
    }
    Log::info("Scanning games in: %s", games_root.string().c_str());

    for (auto& ent : fs::directory_iterator(games_root)) {
        if (!ent.is_directory()) continue;
        GameEntry g;
        g.folder_name = ent.path().filename().string();
        g.folder_path = ent.path().string();

        std::string cue, bin, cover;
        for (auto& f : fs::directory_iterator(ent.path())) {
            if (!f.is_regular_file()) continue;
            auto name = f.path().filename().string();
            if (ends_with_ci(name, ".cue")) {
                if (cue.empty()) cue = f.path().string();
            } else if (ends_with_ci(name, ".bin") || ends_with_ci(name, ".img") || ends_with_ci(name, ".iso")) {
                if (bin.empty()) bin = f.path().string();
            } else if (ends_with_ci(name, ".png") || ends_with_ci(name, ".bmp") ||
                       ends_with_ci(name, ".jpg") || ends_with_ci(name, ".jpeg")) {
                // Prefer cover / folder name match
                auto lower = to_lower(name);
                if (cover.empty() || lower.find("cover") != std::string::npos ||
                    lower.find(to_lower(g.folder_name).substr(0, std::min<size_t>(8, g.folder_name.size()))) != std::string::npos) {
                    cover = f.path().string();
                }
            }
        }

        g.cue_path = cue;
        g.bin_path = bin;
        g.cover_path = cover;
        g.has_cover = !cover.empty();
        g.has_disc  = !cue.empty() || !bin.empty();
        if (g.has_disc)
            out.push_back(std::move(g));
    }

    std::sort(out.begin(), out.end(), [](const GameEntry& a, const GameEntry& b) {
        return to_lower(a.folder_name) < to_lower(b.folder_name);
    });
    Log::info("Games scanner: found %zu title(s) in %s", out.size(), games_root.string().c_str());
    return out;
}

} // namespace ps96
