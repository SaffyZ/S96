#pragma once

#include "common/Types.hpp"
#include <string>
#include <array>
#include <fstream>

namespace ps96 {

class MemoryCard {
public:
    static constexpr u32 CARD_SIZE = 128 * 1024; // 128KB

    MemoryCard();
    bool create(const std::string& path);
    bool load(const std::string& path);
    bool save();
    bool is_loaded() const { return m_loaded; }
    const std::string& path() const { return m_path; }

    u8* data() { return m_data.data(); }
    const u8* data() const { return m_data.data(); }

private:
    std::array<u8, CARD_SIZE> m_data{};
    std::string m_path;
    bool m_loaded = false;
};

} // namespace ps96
