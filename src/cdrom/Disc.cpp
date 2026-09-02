#include "cdrom/Disc.hpp"
#include "common/Log.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;
namespace ps96 {

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
static bool ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    return to_lower(s.substr(s.size() - suf.size())) == to_lower(suf);
}

bool Disc::load_bin(const std::string& path) {
    m_file.close();
    m_file.open(path, std::ios::binary);
    if (!m_file) {
        Log::error("Failed to open BIN: %s", path.c_str());
        return false;
    }
    m_file.seekg(0, std::ios::end);
    auto size = m_file.tellg();
    if (size <= 0) {
        Log::error("BIN empty: %s", path.c_str());
        return false;
    }
    u64 sz = static_cast<u64>(size);

    // Detect raw MODE2/MODE1 by sync header even if the file was truncated
    // mid-sector (size % 2352 != 0). A real 2352 image starts with
    // 00 FF FF FF FF FF FF FF FF FF FF 00.
    bool has_sync = false;
    if (sz >= 12) {
        m_file.clear();
        m_file.seekg(0);
        u8 hdr[12]{};
        m_file.read(reinterpret_cast<char*>(hdr), 12);
        has_sync = (hdr[0] == 0x00 && hdr[11] == 0x00);
        for (int i = 1; i <= 10 && has_sync; i++)
            if (hdr[i] != 0xFF) has_sync = false;
    }

    if (has_sync || (sz % 2352 == 0 && sz % 2048 != 0)) {
        m_sector_size = 2352;
        m_sector_count = static_cast<u32>(sz / 2352);
        if (sz % 2352 != 0)
            Log::warn("BIN truncated mid-sector (%llu bytes leftover) — using %u full 2352 sectors",
                      (unsigned long long)(sz % 2352), m_sector_count);
    } else if (sz % 2048 == 0) {
        m_sector_count = static_cast<u32>(sz / 2048);
        m_sector_size = 2048;
        Log::info("BIN is 2048-byte sectors (Mode1/Mode2 form1 data)");
    } else {
        m_sector_count = static_cast<u32>(sz / 2352);
        m_sector_size = 2352;
        if (m_sector_count == 0) {
            Log::error("BIN too small: %s", path.c_str());
            return false;
        }
        Log::warn("BIN size not multiple of 2352/2048 — treating as 2352 (%u sectors)", m_sector_count);
    }
    if (m_sector_count == 0) {
        Log::error("BIN too small: %s", path.c_str());
        return false;
    }
    m_bin_path = path;
    m_path = path;
    m_loaded = true;
    Log::info("BIN loaded: %s (%u sectors, %u bytes/sector)", path.c_str(), m_sector_count, m_sector_size);
    return true;
}

bool Disc::load_cue(const std::string& path) {
    std::ifstream cue(path);
    if (!cue) {
        Log::error("Failed to open CUE: %s", path.c_str());
        return false;
    }
    fs::path cue_dir = fs::path(path).parent_path();

    // Parse all FILE / TRACK entries. Prefer the first data track
    // (MODE1/MODE2), not the last FILE (often an audio track).
    struct Entry {
        std::string file;
        int track = 0;
        bool data = false;
    };
    std::vector<Entry> entries;
    std::string current_file;
    Entry pending;

    std::string line;
    while (std::getline(cue, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        auto p0 = line.find_first_not_of(" \t");
        if (p0 == std::string::npos) continue;
        line = line.substr(p0);
        std::string upper = to_upper(line);

        if (upper.rfind("FILE ", 0) == 0) {
            auto q1 = line.find('"');
            auto q2 = line.find('"', q1 == std::string::npos ? 0 : q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
                current_file = line.substr(q1 + 1, q2 - q1 - 1);
            else {
                std::istringstream iss(line);
                std::string tok, name;
                iss >> tok >> name;
                current_file = name;
            }
        } else if (upper.rfind("TRACK ", 0) == 0) {
            std::istringstream iss(line);
            std::string tok, type;
            int num = 0;
            iss >> tok >> num >> type;
            type = to_upper(type);
            Entry e;
            e.file = current_file;
            e.track = num;
            e.data = (type.find("MODE1") != std::string::npos ||
                      type.find("MODE2") != std::string::npos);
            entries.push_back(e);
        }
    }

    std::string bin_file;
    // 1) First data track FILE
    for (auto& e : entries) {
        if (e.data && !e.file.empty()) {
            bin_file = e.file;
            Log::info("CUE: using data track %02d file '%s'", e.track, bin_file.c_str());
            break;
        }
    }
    // 2) Track 01 any type
    if (bin_file.empty()) {
        for (auto& e : entries) {
            if (e.track == 1 && !e.file.empty()) {
                bin_file = e.file;
                Log::info("CUE: fallback track 01 file '%s'", bin_file.c_str());
                break;
            }
        }
    }
    // 3) First FILE seen
    if (bin_file.empty()) {
        for (auto& e : entries) {
            if (!e.file.empty()) { bin_file = e.file; break; }
        }
    }
    if (bin_file.empty()) {
        bin_file = fs::path(path).stem().string() + ".bin";
    }

    fs::path bin_path = cue_dir / bin_file;
    if (!fs::exists(bin_path)) {
        bool found = false;
        if (fs::exists(cue_dir)) {
            for (auto& e : fs::directory_iterator(cue_dir)) {
                if (to_lower(e.path().filename().string()) == to_lower(bin_file)) {
                    bin_path = e.path();
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            Log::error("CUE references missing BIN: %s", bin_path.string().c_str());
            return false;
        }
    }
    if (!load_bin(bin_path.string())) return false;
    m_path = path;
    Log::info("CUE loaded: %s → %s (data track)", path.c_str(), bin_path.string().c_str());
    return true;
}

bool Disc::read_sector(u32 lba, u8* out2352) {
    if (!m_loaded || !out2352) return false;
    std::memset(out2352, 0, 2352);
    if (lba >= m_sector_count) return false;

    if (m_sector_size == 2352) {
        u64 offset = static_cast<u64>(lba) * 2352;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset));
        m_file.read(reinterpret_cast<char*>(out2352), 2352);
        return m_file.gcount() > 0;
    }

    // 2048-byte sectors → synthesize MODE2 raw sector
    u64 offset = static_cast<u64>(lba) * 2048;
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(offset));
    char user[2048]{};
    m_file.read(user, 2048);
    if (m_file.gcount() <= 0) return false;
    out2352[0] = 0x00;
    for (int i = 1; i < 11; i++) out2352[i] = 0xFF;
    out2352[11] = 0x00;
    u32 t = lba + 150;
    auto bcd = [](u32 v) -> u8 { return u8(((v / 10) << 4) | (v % 10)); };
    u8 ff = bcd(t % 75);
    u32 ss = t / 75;
    u8 s = bcd(ss % 60);
    u8 m = bcd(ss / 60);
    out2352[12] = m; out2352[13] = s; out2352[14] = ff; out2352[15] = 0x02;
    std::memcpy(out2352 + 24, user, 2048);
    return true;
}

bool Disc::read_user_data(u32 lba, u8* out2048) {
    if (!out2048) return false;
    if (m_sector_size == 2048) {
        u64 offset = static_cast<u64>(lba) * 2048;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset));
        m_file.read(reinterpret_cast<char*>(out2048), 2048);
        return m_file.gcount() > 0;
    }
    u8 raw[2352];
    if (!read_sector(lba, raw)) return false;
    // Detect mode: byte 15 = mode. Mode1 data at 16, Mode2 Form1 at 24
    if (raw[15] == 0x01)
        std::memcpy(out2048, raw + 16, 2048);
    else
        std::memcpy(out2048, raw + 24, 2048);
    return true;
}

bool Disc::parse_directory(u32 lba, u32 size, std::vector<DiscFile>& files) {
    const u32 sector_count = (size + 2047) / 2048;
    for (u32 sec_index = 0; sec_index < sector_count; ++sec_index) {
        u8 data[2048];
        if (!read_user_data(lba + sec_index, data)) return false;
        u32 off = 0;
        while (off < 2048) {
            const u8 len = data[off];
            // A zero length record terminates the current directory sector;
            // the directory itself can continue in the following sector.
            if (len == 0) break;
            if (len < 34 || off + len > 2048) break;

            const u32 file_lba = u32(data[off+2]) | (u32(data[off+3]) << 8) |
                                 (u32(data[off+4]) << 16) | (u32(data[off+5]) << 24);
            const u32 file_size = u32(data[off+10]) | (u32(data[off+11]) << 8) |
                                  (u32(data[off+12]) << 16) | (u32(data[off+13]) << 24);
            const u8 flags = data[off+25];
            const u8 name_len = data[off+32];
            if (name_len > 0 && off + 33 + name_len <= off + len) {
                std::string name(reinterpret_cast<char*>(&data[off+33]), name_len);
                const auto semi = name.find(';');
                if (semi != std::string::npos) name.resize(semi);
                // ISO9660 special entries are single-byte 0/1 names and are
                // not useful as normal files for path lookup.
                if (!name.empty() && name != "\x00" && name != "\x01") {
                    DiscFile f;
                    f.name = to_upper(name);
                    f.lba = file_lba;
                    f.size = file_size;
                    if (flags & 0x02) f.name += '/';
                    files.push_back(f);
                }
            }
            off += len;
        }
    }
    return true;
}

bool Disc::find_file(const std::string& path, DiscFile& out) {
    if (!m_loaded) return false;
    // Primary Volume Descriptor at LBA 16
    u8 pvd[2048];
    if (!read_user_data(16, pvd)) return false;
    if (pvd[0] != 0x01 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        Log::warn("Disc: no ISO9660 PVD at LBA 16");
        return false;
    }
    u32 root_lba = u32(pvd[158]) | (u32(pvd[159]) << 8) | (u32(pvd[160]) << 16) | (u32(pvd[161]) << 24);
    u32 root_size = u32(pvd[166]) | (u32(pvd[167]) << 8) | (u32(pvd[168]) << 16) | (u32(pvd[169]) << 24);

    // Normalize path: strip cdrom:\, leading \, ;1
    std::string norm = path;
    auto strip_pref = [&](const char* p) {
        std::string pl = to_lower(p);
        if (to_lower(norm).rfind(pl, 0) == 0) norm = norm.substr(pl.size());
    };
    strip_pref("cdrom:");
    strip_pref("\\");
    strip_pref("/");
    auto semi = norm.find(';');
    if (semi != std::string::npos) norm = norm.substr(0, semi);
    // Split into components
    std::vector<std::string> parts;
    std::string cur;
    for (char c : norm) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) { parts.push_back(to_upper(cur)); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(to_upper(cur));
    if (parts.empty()) return false;

    u32 dir_lba = root_lba;
    u32 dir_size = root_size;
    for (size_t i = 0; i < parts.size(); i++) {
        std::vector<DiscFile> files;
        if (!parse_directory(dir_lba, dir_size, files)) return false;
        bool found = false;
        bool is_last = (i + 1 == parts.size());
        for (auto& f : files) {
            std::string n = f.name;
            bool is_dir = !n.empty() && n.back() == '/';
            if (is_dir) n.pop_back();
            if (n == parts[i]) {
                if (is_last && !is_dir) {
                    out = f;
                    if (!out.name.empty() && out.name.back() == '/') out.name.pop_back();
                    return true;
                }
                if (!is_last && is_dir) {
                    dir_lba = f.lba;
                    dir_size = f.size;
                    found = true;
                    break;
                }
            }
        }
        if (is_last) {
            // Also accept without exact dir flag
            for (auto& f : files) {
                std::string n = f.name;
                if (!n.empty() && n.back() == '/') n.pop_back();
                if (n == parts[i]) { out = f; return true; }
            }
        }
        if (!found && !is_last) return false;
    }
    return false;
}

bool Disc::read_file(const DiscFile& f, std::vector<u8>& out) {
    out.clear();
    if (f.size == 0) return false;
    out.resize(f.size);
    u32 left = f.size;
    u32 lba = f.lba;
    u32 pos = 0;
    while (left > 0) {
        u8 sec[2048];
        if (!read_user_data(lba, sec)) return false;
        u32 n = left < 2048 ? left : 2048;
        std::memcpy(out.data() + pos, sec, n);
        pos += n;
        left -= n;
        lba++;
    }
    return true;
}

bool Disc::read_system_cnf_boot(std::string& boot_path) {
    DiscFile f;
    if (!find_file("SYSTEM.CNF", f)) {
        // Try common alternate
        if (!find_file("system.cnf", f)) {
            Log::warn("SYSTEM.CNF not found on disc");
            return false;
        }
    }
    std::vector<u8> data;
    if (!read_file(f, data)) return false;
    std::string text(data.begin(), data.end());
    // Parse BOOT = cdrom:\FOO\BAR.EXE;1
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = to_upper(line.substr(0, eq));
        // trim key
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (key != "BOOT") continue;
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        boot_path = val;
        Log::info("SYSTEM.CNF BOOT=%s", boot_path.c_str());
        return true;
    }
    Log::warn("SYSTEM.CNF has no BOOT= line");
    return false;
}

} // namespace ps96
