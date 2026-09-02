#include "bios/BIOS.hpp"
#include "common/Log.hpp"
#include <fstream>

namespace ps96 {

bool BIOS::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        Log::error("Cannot open BIOS: %s", path.c_str());
        return false;
    }
    auto sz = f.tellg();
    if (sz <= 0 || sz > static_cast<std::streamoff>(BIOS_SIZE)) {
        Log::error("Invalid BIOS size");
        return false;
    }
    m_data.resize(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(m_data.data()), sz);
    m_path = path;
    Log::info("BIOS loaded from %s (%zu bytes)", path.c_str(), m_data.size());
    return true;
}

} // namespace ps96
