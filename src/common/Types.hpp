#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <functional>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <bit>

namespace ps96 {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;

constexpr u32 KILOBYTE = 1024;
constexpr u32 MEGABYTE = 1024 * 1024;

// PS1 memory sizes
constexpr u32 MAIN_RAM_SIZE     = 2 * MEGABYTE;
constexpr u32 SCRATCHPAD_SIZE   = 1 * KILOBYTE;
constexpr u32 BIOS_SIZE         = 512 * KILOBYTE;
constexpr u32 VRAM_SIZE         = 1024 * KILOBYTE; // 1MB VRAM (1024x512 16-bit)
constexpr u32 SPU_RAM_SIZE      = 512 * KILOBYTE;
constexpr u32 EXPANSION1_SIZE   = 8 * MEGABYTE;
constexpr u32 EXPANSION2_SIZE   = 8 * KILOBYTE;
constexpr u32 EXPANSION3_SIZE   = 2 * MEGABYTE;

// Physical addresses (KSEG0/KSEG1 mapped)
constexpr u32 ADDR_MAIN_RAM     = 0x00000000;
constexpr u32 ADDR_EXPANSION1   = 0x1F000000;
constexpr u32 ADDR_SCRATCHPAD   = 0x1F800000;
constexpr u32 ADDR_IO           = 0x1F801000;
constexpr u32 ADDR_EXPANSION2   = 0x1F802000;
constexpr u32 ADDR_EXPANSION3   = 0x1FA00000;
constexpr u32 ADDR_BIOS         = 0x1FC00000;
constexpr u32 ADDR_CACHE_CTRL   = 0xFFFE0130;

// IO register bases
constexpr u32 IO_MEM_CTRL       = 0x1F801000;
constexpr u32 IO_PAD_MEMCARD    = 0x1F801040;
constexpr u32 IO_MEM_CTRL2      = 0x1F801060;
constexpr u32 IO_INTERRUPT      = 0x1F801070;
constexpr u32 IO_DMA            = 0x1F801080;
constexpr u32 IO_TIMERS         = 0x1F801100;
constexpr u32 IO_CDROM          = 0x1F801800;
constexpr u32 IO_GPU            = 0x1F801810;
constexpr u32 IO_MDEC           = 0x1F801820;
constexpr u32 IO_SPU            = 0x1F801C00;
constexpr u32 IO_EXP2           = 0x1F802000;

inline constexpr u32 phys_addr(u32 addr) {
    // Strip KSEG bits for physical mapping (KUSEG/KSEG0/KSEG1 share low 29 bits for most)
    return addr & 0x1FFFFFFF;
}

inline constexpr bool is_kseg0(u32 addr) { return (addr & 0xE0000000) == 0x80000000; }
inline constexpr bool is_kseg1(u32 addr) { return (addr & 0xE0000000) == 0xA0000000; }
inline constexpr bool is_kuseg(u32 addr) { return (addr & 0xE0000000) == 0x00000000; }

template<typename T>
inline T sign_extend(T value, int bits) {
    using ST = std::make_signed_t<T>;
    ST s = static_cast<ST>(value);
    ST shift = static_cast<ST>(sizeof(T) * 8 - bits);
    return static_cast<T>((s << shift) >> shift);
}

inline s32 sex8(u32 v)  { return static_cast<s32>(static_cast<s8>(v)); }
inline s32 sex16(u32 v) { return static_cast<s32>(static_cast<s16>(v)); }

} // namespace ps96
