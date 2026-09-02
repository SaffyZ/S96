#include "memory/Bus.hpp"
#include "gpu/GPU.hpp"
#include "dma/DMA.hpp"
#include "cdrom/CDROM.hpp"
#include "spu/SPU.hpp"
#include "mdec/MDEC.hpp"
#include "timers/Timers.hpp"
#include "controllers/Controllers.hpp"
#include "emulator/InterruptController.hpp"
#include "common/Log.hpp"

namespace ps96 {

Bus::Bus() {
    m_bios.resize(BIOS_SIZE, 0);
    reset();
}

void Bus::reset() {
    for (auto& r : m_mem_ctrl) r = 0;
    // Defaults similar to post-BIOS init values so early probes are stable
    m_mem_ctrl[0] = 0x1F000000; // EXP1 base
    m_mem_ctrl[1] = 0x1F802000; // EXP2 base
    m_mem_ctrl[2] = 0x0013243F; // EXP1 delay/size
    m_mem_ctrl[3] = 0x00003022; // EXP3 delay/size
    m_mem_ctrl[4] = 0x0013243F; // BIOS delay/size
    m_mem_ctrl[5] = 0x200931E1; // SPU delay/size
    m_mem_ctrl[6] = 0x00020843; // CDROM delay/size
    m_mem_ctrl[7] = 0x00070777; // EXP2 delay/size
    m_mem_ctrl[8] = 0x00031125; // common delay
    m_ram_size = 0x00000B88;
    m_cache_ctrl = 0;
}

void Bus::load_bios(const u8* data, u32 size) {
    if (size > BIOS_SIZE) size = BIOS_SIZE;
    std::memcpy(m_bios.data(), data, size);
    if (size < BIOS_SIZE)
        std::memset(m_bios.data() + size, 0, BIOS_SIZE - size);
    Log::info("BIOS loaded: %u bytes", size);
}

u8 Bus::read8(u32 addr) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000)
        return m_ram.read8(p & (MAIN_RAM_SIZE - 1));
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)
        return m_scratch.read8(p - ADDR_SCRATCHPAD);
    if (p >= ADDR_BIOS && p < ADDR_BIOS + BIOS_SIZE)
        return m_bios[p - ADDR_BIOS];
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000)
        return read_io8(p);
    // EXP2 / POST area
    if (p >= 0x1F802000 && p < 0x1F804000)
        return 0xFF;
    return 0xFF;
}

u16 Bus::read16(u32 addr) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000)
        return m_ram.read16(p & (MAIN_RAM_SIZE - 1));
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)
        return m_scratch.read16(p - ADDR_SCRATCHPAD);
    if (p >= ADDR_BIOS && p < ADDR_BIOS + BIOS_SIZE) {
        u32 off = p - ADDR_BIOS;
        return static_cast<u16>(m_bios[off] | (m_bios[off + 1] << 8));
    }
    // SPU is a 16-bit peripheral — do NOT RMW through neighbouring regs
    if (p >= 0x1F801C00 && p < 0x1F801E80) {
        u32 v = m_spu ? m_spu->read32(p & ~1u) : 0;
        return static_cast<u16>(v & 0xFFFF);
    }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        return m_cdrom ? m_cdrom->read8(p) : 0xFF;
    }
    if (p >= 0x1F801040 && p < 0x1F801060) {
        if (m_controllers) return m_controllers->read16(p & ~1u);
        return 0xFFFF;
    }
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000) {
        u32 v = read_io32(p & ~3u);
        return static_cast<u16>((v >> ((p & 2) * 8)) & 0xFFFF);
    }
    return 0xFFFF;
}

u32 Bus::read32(u32 addr) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000)
        return m_ram.read32(p & (MAIN_RAM_SIZE - 1));
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)
        return m_scratch.read32(p - ADDR_SCRATCHPAD);
    if (p >= ADDR_BIOS && p < ADDR_BIOS + BIOS_SIZE) {
        u32 off = p - ADDR_BIOS;
        return m_bios[off] | (m_bios[off+1] << 8) | (m_bios[off+2] << 16) | (m_bios[off+3] << 24);
    }
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000)
        return read_io32(p);
    if (p == ADDR_CACHE_CTRL || (p & 0xFFFF0000) == 0xFFFE0000)
        return m_cache_ctrl;
    return 0xFFFFFFFF;
}

void Bus::write8(u32 addr, u8 value) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000) {
m_ram.write8(p & (MAIN_RAM_SIZE - 1), value);
        return;
    }
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE) {
        m_scratch.write8(p - ADDR_SCRATCHPAD, value);
        return;
    }
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000) {
        write_io8(p, value);
        return;
    }
    // EXP2 POST - accept and ignore
}

void Bus::write16(u32 addr, u16 value) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000) {
        m_ram.write16(p & (MAIN_RAM_SIZE - 1), value);
        return;
    }
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE) {
        m_scratch.write16(p - ADDR_SCRATCHPAD, value);
        return;
    }
    // SPU registers are 16-bit wide. Aligning to 32-bit and RMW would smash
    // neighbouring regs (e.g. SPUCNT at 1F801DAA through data port at DA8).
    if (p >= 0x1F801C00 && p < 0x1F801E80) {
        if (m_spu) m_spu->write32(p & ~1u, value);
        return;
    }
    // CD-ROM is strictly 8-bit — only the addressed byte is written
    if (p >= 0x1F801800 && p < 0x1F801804) {
        if (m_cdrom) m_cdrom->write8(p, static_cast<u8>(value & 0xFF));
        return;
    }
    // Controller registers at 1F801040..4E are mixed-width. The PS1 BIOS and
    // games commonly use SH/LH for JOY_MODE/JOY_CTRL/JOY_BAUD. Do not route
    // these through the generic 32-bit read-modify-write path: 104A would be
    // accidentally written as JOY_MODE (1048), so ACK/IRQ/port-select bits
    // never reach the controller. That makes a game look frozen because input
    // transactions never complete.
    if (p >= 0x1F801040 && p < 0x1F801060) {
        if (m_controllers) m_controllers->write16(p & ~1u, value);
        return;
    }
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000) {
        u32 aligned = p & ~3u;
        // I_STAT is write-0-to-clear. Pass the written value through directly;
        // do not RMW-merge with current status.
        if (aligned == 0x1F801070) {
            if (m_irq) m_irq->acknowledge(value);
            return;
        }
        if (aligned == 0x1F801074) {
            if (m_irq) {
                u32 cur = m_irq->mask();
                if (p & 2)
                    cur = (cur & 0x0000FFFF) | (static_cast<u32>(value) << 16);
                else
                    cur = (cur & 0xFFFF0000) | value;
                m_irq->set_mask(cur);
            }
            return;
        }
        u32 cur = read_io32(aligned);
        if (p & 2)
            cur = (cur & 0x0000FFFF) | (static_cast<u32>(value) << 16);
        else
            cur = (cur & 0xFFFF0000) | value;
        write_io32(aligned, cur);
    }
}

void Bus::write32(u32 addr, u32 value) {
    u32 p = phys_addr(addr);
    if (p < 0x00800000) {
        if ((p & ~0x3F) == 0x18600) {
            static int s_claim = 0;
            if (s_claim < 20) {
                // Need PC; Bus doesn't have CPU ptr always, use Log only
                Log::info("CLAIM_WR p=%08X v=%08X", p, value);
                s_claim++;
            }
        }
        m_ram.write32(p & (MAIN_RAM_SIZE - 1), value);
        if ((p & 0x1FFFFFFF) == 0x0004445C) {
            static int s_lk = 0;
            if (s_lk < 20) {
                Log::info("WR_LOCK v=%08X", value);
                s_lk++;
            }
        }
        return;
    }
    if (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE) {
        m_scratch.write32(p - ADDR_SCRATCHPAD, value);
        return;
    }
    if (p >= ADDR_IO && p < ADDR_IO + 0x2000) {
        write_io32(p, value);
        return;
    }
    if (p == ADDR_CACHE_CTRL || (p & 0xFFFF0000) == 0xFFFE0000) {
        m_cache_ctrl = value;
        return;
    }
}

u32 Bus::read_io32(u32 addr) {
    if (addr == 0x1F801810) return m_gpu ? m_gpu->read_gpuread() : 0;
    if (addr == 0x1F801814) return m_gpu ? m_gpu->read_gpustat() : 0x1C000000;

    if (addr == 0x1F801070) return m_irq ? m_irq->status() : 0;
    if (addr == 0x1F801074) return m_irq ? m_irq->mask() : 0;

    if (addr >= 0x1F801080 && addr < 0x1F801100)
        return m_dma ? m_dma->read32(addr) : 0;

    if (addr >= 0x1F801100 && addr < 0x1F801130)
        return m_timers ? m_timers->read32(addr) : 0;

    if (addr >= 0x1F801800 && addr < 0x1F801804)
        return m_cdrom ? m_cdrom->read32(addr) : 0;

    if (addr == 0x1F801820) return m_mdec ? m_mdec->read_data() : 0;
    if (addr == 0x1F801824) return m_mdec ? m_mdec->read_status() : 0x80000000;

    if (addr >= 0x1F801C00 && addr < 0x1F801E80)
        return m_spu ? m_spu->read32(addr) : 0;

    if (addr >= 0x1F801040 && addr < 0x1F801060)
        return m_controllers ? m_controllers->read32(addr & ~3u) : 0xFFFFFFFF;

    if (addr >= 0x1F801000 && addr < 0x1F801024) {
        u32 idx = (addr - 0x1F801000) / 4;
        if (idx < 9) return m_mem_ctrl[idx];
    }
    if (addr == 0x1F801060) return m_ram_size;

    // Unknown IO — open bus
    return 0;
}

void Bus::write_io32(u32 addr, u32 value) {
    if (addr == 0x1F801810) {
        if (m_gpu) m_gpu->write_gp0(value);
        return;
    }
    if (addr == 0x1F801814) {
        if (m_gpu) m_gpu->write_gp1(value);
        return;
    }
    if (addr == 0x1F801070) {
        if (m_irq) m_irq->acknowledge(value);
        return;
    }
    if (addr == 0x1F801074) {
        if (m_irq) m_irq->set_mask(value);
        return;
    }
    if (addr >= 0x1F801080 && addr < 0x1F801100) {
        if (m_dma) m_dma->write32(addr, value);
        return;
    }
    if (addr >= 0x1F801100 && addr < 0x1F801130) {
        if (m_timers) m_timers->write32(addr, value);
        return;
    }
    if (addr >= 0x1F801800 && addr < 0x1F801804) {
        if (m_cdrom) m_cdrom->write32(addr, value);
        return;
    }
    if (addr == 0x1F801820) {
        if (m_mdec) m_mdec->write_data(value);
        return;
    }
    if (addr == 0x1F801824) {
        if (m_mdec) m_mdec->write_control(value);
        return;
    }
    if (addr >= 0x1F801C00 && addr < 0x1F801E80) {
        if (m_spu) m_spu->write32(addr, value);
        return;
    }
    if (addr >= 0x1F801040 && addr < 0x1F801060) {
        if (m_controllers) m_controllers->write32(addr & ~3u, value);
        return;
    }
    if (addr >= 0x1F801000 && addr < 0x1F801024) {
        u32 idx = (addr - 0x1F801000) / 4;
        if (idx < 9) m_mem_ctrl[idx] = value;
        return;
    }
    if (addr == 0x1F801060) {
        m_ram_size = value;
        return;
    }
}

u8 Bus::read_io8(u32 addr) {
    if (addr >= 0x1F801800 && addr <= 0x1F801803)
        return m_cdrom ? m_cdrom->read8(addr) : 0xFF;
    if (addr >= 0x1F801040 && addr < 0x1F801060)
        return m_controllers ? m_controllers->read8(addr) : 0xFF;
    u32 v = read_io32(addr & ~3u);
    return static_cast<u8>((v >> ((addr & 3) * 8)) & 0xFF);
}

void Bus::write_io8(u32 addr, u8 value) {
    if (addr >= 0x1F801800 && addr <= 0x1F801803) {
        if (m_cdrom) m_cdrom->write8(addr, value);
        return;
    }
    if (addr >= 0x1F801040 && addr < 0x1F801060) {
        if (m_controllers) m_controllers->write8(addr, value);
        return;
    }
    // I_STAT is write-0-to-clear. Pass the written byte through; bits not
    // covered by this byte write are treated as 1 (no change) by acknowledge
    // only for the low 11 IRQ bits when the full value is constructed by caller.
    if (addr == 0x1F801070) {
        if (m_irq) m_irq->acknowledge(0xFFFFFF00u | value);
        return;
    }
    if (addr == 0x1F801074) {
        if (m_irq) {
            u32 cur = m_irq->mask();
            cur = (cur & 0xFFFFFF00u) | value;
            m_irq->set_mask(cur);
        }
        return;
    }
    u32 aligned = addr & ~3u;
    u32 cur = read_io32(aligned);
    int shift = (addr & 3) * 8;
    cur = (cur & ~(0xFFu << shift)) | (static_cast<u32>(value) << shift);
    write_io32(aligned, cur);
}

} // namespace ps96
