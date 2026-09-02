#pragma once

#include "common/Types.hpp"
#include "memory/RAM.hpp"
#include <functional>
#include <array>

namespace ps96 {

class GPU;
class DMA;
class CDROM;
class SPU;
class MDEC;
class Timers;
class Controllers;
class InterruptController;

class Bus {
public:
    Bus();
    ~Bus() = default;

    void reset();
    void load_bios(const u8* data, u32 size);

    // Connect peripherals
    void set_gpu(GPU* gpu) { m_gpu = gpu; }
    void set_dma(DMA* dma) { m_dma = dma; }
    void set_cdrom(CDROM* cd) { m_cdrom = cd; }
    void set_spu(SPU* spu) { m_spu = spu; }
    void set_mdec(MDEC* mdec) { m_mdec = mdec; }
    void set_timers(Timers* t) { m_timers = t; }
    void set_controllers(Controllers* c) { m_controllers = c; }
    void set_irq(InterruptController* irq) { m_irq = irq; }

    u8  read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Direct RAM access for DMA
    RAM& main_ram() { return m_ram; }
    const RAM& main_ram() const { return m_ram; }
    RAM& scratchpad() { return m_scratch; }

    u8* bios_data() { return m_bios.data(); }
    u32 bios_size() const { return static_cast<u32>(m_bios.size()); }

private:
    RAM m_ram{MAIN_RAM_SIZE};
    RAM m_scratch{SCRATCHPAD_SIZE};
    std::vector<u8> m_bios;

    GPU* m_gpu = nullptr;
    DMA* m_dma = nullptr;
    CDROM* m_cdrom = nullptr;
    SPU* m_spu = nullptr;
    MDEC* m_mdec = nullptr;
    Timers* m_timers = nullptr;
    Controllers* m_controllers = nullptr;
    InterruptController* m_irq = nullptr;

    // Memory control registers
    u32 m_mem_ctrl[9]{};
    u32 m_ram_size = 0x00000B88; // default
    u32 m_cache_ctrl = 0;

    u32 read_io32(u32 addr);
    void write_io32(u32 addr, u32 value);
    u8  read_io8(u32 addr);
    void write_io8(u32 addr, u8 value);
};

} // namespace ps96
