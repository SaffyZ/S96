#include "dma/DMA.hpp"
#include "memory/Bus.hpp"
#include "gpu/GPU.hpp"
#include "cdrom/CDROM.hpp"
#include "spu/SPU.hpp"
#include "mdec/MDEC.hpp"
#include "emulator/InterruptController.hpp"
#include "common/Log.hpp"

namespace ps96 {

DMA::DMA() { reset(); }

void DMA::reset() {
    for (auto& ch : m_channels) {
        ch.base = 0;
        ch.block_control = 0;
        ch.channel_control = 0;
    }
    m_dpcr = 0x07654321;
    m_dicr = 0;
    m_irq_level = false;
}

u32 DMA::read32(u32 addr) {
    u32 off = addr - 0x1F801080;
    int ch = static_cast<int>(off / 0x10);
    u32 reg = (off % 0x10) / 4;
    if (ch >= 0 && ch < 7) {
        switch (reg) {
            case 0: return m_channels[ch].base;
            case 1: return m_channels[ch].block_control;
            case 2: return m_channels[ch].channel_control;
        }
    }
    if (addr == 0x1F8010F0) return m_dpcr;
    if (addr == 0x1F8010F4) {
        // Bit31 is the DMA controller's derived master request.  Force IRQ
        // (bit15) or master-enabled channel flags (bits24..30) assert it.
        // The I_STAT latch is separate and is never cleared by reading DICR.
        u32 dicr = m_dicr & 0x7FFFFFFF;
        const bool master = (dicr & 0x8000u) != 0 ||
                            ((dicr & 0x00800000u) != 0 && (dicr & 0x7F000000u) != 0);
        if (master) dicr |= 0x80000000u;
        return dicr;
    }
    return 0;
}

void DMA::write32(u32 addr, u32 value) {
    u32 off = addr - 0x1F801080;
    int ch = static_cast<int>(off / 0x10);
    u32 reg = (off % 0x10) / 4;
    if (ch >= 0 && ch < 7) {
        switch (reg) {
            case 0: m_channels[ch].base = value & 0x00FFFFFF; break;
            case 1: m_channels[ch].block_control = value; break;
            case 2:
                m_channels[ch].channel_control = value;
                if (value & 0x01000000) run_channel(ch);
                break;
        }
        return;
    }
    if (addr == 0x1F8010F0) { m_dpcr = value; return; }
    if (addr == 0x1F8010F4) {
        // Writing 1 to bits24-30 acknowledges/clears those channel IRQ flags.
        // Bits0-5,15,16-23 are writable; bit31 is read-only.
        const u32 ack = value & 0x7F000000u;
        const u32 writable = value & 0x00FF803Fu;
        m_dicr = (m_dicr & ~ack);
        m_dicr = (m_dicr & ~0x00FF803Fu) | writable;
        update_irq_line();
        return;
    }
}

void DMA::tick() {
    // MDEC DMA uses the device request lines rather than a blind one-shot
    // transfer. If a channel was started before the decoder had room/data, it
    // remains active and is retried here once the request becomes asserted.
    for (int ch : {0, 1}) {
        if (!m_channels[ch].active()) continue;
        if (ch == 0 && m_mdec && !m_mdec->dma_in_request()) continue;
        if (ch == 1 && m_mdec && !m_mdec->dma_out_request()) continue;
        run_channel(ch);
    }
}

void DMA::run_channel(int ch) {
    if (ch == 3) {
        static int s_d3 = 0;
        if (s_d3 < 15) {
            Log::info("DMA_CH3 base=%08X bc=%08X ctrl=%08X dpcr=%08X",
                m_channels[3].base, m_channels[3].block_control,
                m_channels[3].channel_control, m_dpcr);
            s_d3++;
        }
    }
    if (ch == 0 || ch == 1) {
        static int s_mdec_dma = 0;
        if (s_mdec_dma < 20) {
            Log::info("DMA_MDEC ch=%d base=%08X bc=%08X ctrl=%08X req=%d",
                ch, m_channels[ch].base, m_channels[ch].block_control,
                m_channels[ch].channel_control,
                ch == 0 ? (m_mdec && m_mdec->dma_in_request()) : (m_mdec && m_mdec->dma_out_request()));
            s_mdec_dma++;
        }
    }
    // DPCR: 4-bit field per channel; bit3 of field = enable
    const u32 enable = 1u << (ch * 4 + 3);
    if (!(m_dpcr & enable)) {
        m_channels[ch].channel_control &= ~0x01000000u;
        return;
    }
    u32 sync = (m_channels[ch].channel_control >> 9) & 3;
    if (sync == 2) transfer_linked_list(ch);
    else {
        // DMA0/1 are handshake-driven by MDEC request status. A request that
        // is low means the device is not ready for this block yet; keep the
        // DMA channel active and let tick() retry it.
        if (ch == 0 && m_mdec && !m_mdec->dma_in_request()) return;
        if (ch == 1 && m_mdec && !m_mdec->dma_out_request()) return;
        transfer_block(ch);
    }
}

void DMA::transfer_block(int ch) {
    if (!m_bus) { complete_channel(ch); return; }
    u32 addr = m_channels[ch].base & 0x00FFFFFF;
    u32 bc = m_channels[ch].block_control;
    u32 bs = bc & 0xFFFF;
    u32 ba = (bc >> 16) & 0xFFFF;
    if (bs == 0) bs = 0x10000;
    u32 sync = (m_channels[ch].channel_control >> 9) & 3;
    u32 words = (sync == 1) ? (bs * (ba ? ba : 0x10000)) : bs;
    bool to_device = (m_channels[ch].channel_control & 1) != 0; // 1 = from RAM to device
    s32 step = (m_channels[ch].channel_control & 2) ? -4 : 4;

    // Channel 6 OTC special: clear linked list
    if (ch == 6) {
        u32 count = bs;
        if (count == 0) count = 0x10000;
        for (u32 i = 0; i < count; i++) {
            u32 next = (i == count - 1) ? 0x00FFFFFF : ((addr - 4) & 0x00FFFFFF);
            m_bus->write32(addr, next);
            addr = (addr - 4) & 0x00FFFFFF;
        }
        m_channels[ch].base = addr;
        complete_channel(ch);
        return;
    }

    for (u32 i = 0; i < words; i++) {
        if (to_device) {
            u32 data = m_bus->read32(addr);
            switch (ch) {
                case 0: if (m_mdec) m_mdec->write_data(data); break;
                case 2: if (m_gpu) m_gpu->dma_write(data); break;
                case 4: if (m_spu) m_spu->dma_write(data); break;
                default: break;
            }
        } else {
            u32 data = 0;
            switch (ch) {
                case 1: if (m_mdec) data = m_mdec->read_data(); break;
                case 2: if (m_gpu) data = m_gpu->dma_read(); break;
                case 3: if (m_cdrom) data = m_cdrom->dma_read(); break;
                case 4: if (m_spu) data = m_spu->dma_read(); break;
                default: break;
            }
            m_bus->write32(addr, data);
        }
        addr = static_cast<u32>((static_cast<s32>(addr) + step) & 0x00FFFFFF);
    }
    m_channels[ch].base = addr;
    complete_channel(ch);
}

void DMA::transfer_linked_list(int ch) {
    if (ch != 2 || !m_bus || !m_gpu) {
        complete_channel(ch);
        return;
    }
    u32 addr = m_channels[ch].base & 0x00FFFFFF;
    int packets = 0;
    for (int safety = 0; safety < 100000; safety++) {
        u32 header = m_bus->read32(addr);
        u32 next = header & 0x00FFFFFF;
        u32 count = header >> 24;
        {
            static int s_ot = 0;
            if (s_ot < 12) {
                Log::info("OT_PKT addr=%08X count=%u next=%08X first=%08X",
                    addr, count, next,
                    count ? m_bus->read32((addr + 4) & 0x00FFFFFF) : 0);
                s_ot++;
            }
        }
        for (u32 i = 0; i < count; i++) {
            u32 data = m_bus->read32((addr + 4 + i * 4) & 0x00FFFFFF);
            m_gpu->dma_write(data);
        }
        packets++;
        if (next == 0x00FFFFFF || next == addr) break;
        addr = next & 0x00FFFFFF;
    }
    complete_channel(ch);
}

void DMA::complete_channel(int ch) {
    m_channels[ch].channel_control &= ~0x01000000u;
    // PSX DICR completion flags are armed only when both the channel IRQ
    // enable and master enable are set.  The resulting DICR master request is
    // then latched into I_STAT bit3; clearing DICR must not clear I_STAT.
    const u32 ch_enable = 1u << (16 + ch);
    if ((m_dicr & ch_enable) && (m_dicr & 0x00800000u)) {
        m_dicr |= (1u << (24 + ch));
    }
    update_irq_line();
}

void DMA::update_irq_line() {
    const u32 dicr = m_dicr & 0x7FFFFFFFu;
    const bool level = (dicr & 0x8000u) != 0 ||
                       ((dicr & 0x00800000u) != 0 && (dicr & 0x7F000000u) != 0);
    if (level && !m_irq_level && m_irq) {
        m_irq->raise(InterruptController::DMA);
    }
    // I_STAT is write-0-to-clear.  A DMA DICR acknowledge changes the DMA
    // peripheral's request level but must not pull I_STAT bit3 low.
    m_irq_level = level;
}

} // namespace ps96
