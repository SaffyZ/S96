#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>

namespace ps96 {

class BIOS {
public:
    bool load(const std::string& path);
    const u8* data() const { return m_data.data(); }
    u32 size() const { return static_cast<u32>(m_data.size()); }
    bool is_loaded() const { return !m_data.empty(); }
    const std::string& path() const { return m_path; }

private:
    std::vector<u8> m_data;
    std::string m_path;
};

} // namespace ps96
