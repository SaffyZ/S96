#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <fstream>

namespace ps96 {

struct DiscFile {
    std::string name;   // uppercase, no version
    u32 lba = 0;
    u32 size = 0;
};

class Disc {
public:
    bool load_cue(const std::string& path);
    bool load_bin(const std::string& path);

    bool is_loaded() const { return m_loaded; }
    u32 sector_count() const { return m_sector_count; }

    // Read raw 2352-byte sector
    bool read_sector(u32 lba, u8* out2352);
    // Read 2048 user-data bytes from a data sector
    bool read_user_data(u32 lba, u8* out2048);

    // ISO9660: list files in root (and one level of subdirs when path has \)
    bool find_file(const std::string& path, DiscFile& out);
    bool read_file(const DiscFile& f, std::vector<u8>& out);

    // Parse SYSTEM.CNF BOOT= line → relative path like "PATH\FILE.EXE"
    bool read_system_cnf_boot(std::string& boot_path);

    std::string path() const { return m_path; }

private:
    bool m_loaded = false;
    std::string m_path;
    std::string m_bin_path;
    u32 m_sector_count = 0;
    std::ifstream m_file;
    u32 m_pregap = 150;
    u32 m_sector_size = 2352;

    bool parse_directory(u32 lba, u32 size, std::vector<DiscFile>& files);
};

} // namespace ps96
