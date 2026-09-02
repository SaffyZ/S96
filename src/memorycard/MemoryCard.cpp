#include "memorycard/MemoryCard.hpp"
#include "common/Log.hpp"
#include <cstring>
#include <fstream>

namespace ps96 {

MemoryCard::MemoryCard() {
    m_data.fill(0xFF);
}

bool MemoryCard::create(const std::string& path) {
    m_data.fill(0xFF);
    // Header block
    m_data[0] = 'M';
    m_data[1] = 'C';
    // Checksum of first 127 bytes for block 0
    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum ^= m_data[i];
    m_data[127] = sum;
    m_path = path;
    m_loaded = true;
    return save();
}

bool MemoryCard::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Log::warn("Memory card not found, creating: %s", path.c_str());
        return create(path);
    }
    f.read(reinterpret_cast<char*>(m_data.data()), CARD_SIZE);
    m_path = path;
    m_loaded = true;
    Log::info("Memory card loaded: %s", path.c_str());
    return true;
}

bool MemoryCard::save() {
    if (!m_loaded || m_path.empty()) return false;
    std::ofstream f(m_path, std::ios::binary);
    if (!f) {
        Log::error("Failed to save memory card: %s", m_path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(m_data.data()), CARD_SIZE);
    return true;
}

} // namespace ps96
