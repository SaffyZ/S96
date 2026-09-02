#pragma once

#include "common/Types.hpp"
#include <vector>
#include <cstring>

namespace ps96 {

class RAM {
public:
    explicit RAM(u32 size) : m_data(size, 0) {}

    u8  read8(u32 offset) const { return m_data[offset % m_data.size()]; }
    u16 read16(u32 offset) const {
        offset %= m_data.size();
        u16 v;
        std::memcpy(&v, &m_data[offset], 2);
        return v;
    }
    u32 read32(u32 offset) const {
        offset %= m_data.size();
        u32 v;
        std::memcpy(&v, &m_data[offset], 4);
        return v;
    }
    void write8(u32 offset, u8 v)  { m_data[offset % m_data.size()] = v; }
    void write16(u32 offset, u16 v) {
        offset %= m_data.size();
        std::memcpy(&m_data[offset], &v, 2);
    }
    void write32(u32 offset, u32 v) {
        offset %= m_data.size();
        std::memcpy(&m_data[offset], &v, 4);
    }

    u8* data() { return m_data.data(); }
    const u8* data() const { return m_data.data(); }
    u32 size() const { return static_cast<u32>(m_data.size()); }

private:
    std::vector<u8> m_data;
};

} // namespace ps96
