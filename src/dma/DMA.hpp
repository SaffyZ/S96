#pragma once

#include "common/Types.hpp"
#include <functional>
#include <array>

namespace ps96 {

class Bus;
class GPU;
class CDROM;
class SPU;
class MDEC;
class InterruptController;

class DMA {
public:
    DMA();
    void reset();
    void set_bus(Bus* bus) { m_bus = bus; }
    void set_gpu(GPU* gpu) { m_gpu = gpu; }
    void set_cdrom(CDROM* cd) { m_cdrom = cd; }
    void set_spu(SPU* spu) { m_spu = spu; }
    void set_mdec(MDEC* mdec) { m_mdec = mdec; }
    void set_irq(InterruptController* irq) { m_irq = irq; }

    u32 read32(u32 addr);
    void write32(u32 addr, u32 value);

    void tick();

private:
    struct Channel {
        u32 base = 0;
        u32 block_control = 0;
        u32 channel_control = 0;
        bool active() const { return (channel_control & 0x01000000) != 0; }
    };

    std::array<Channel, 7> m_channels{};
    u32 m_dpcr = 0x07654321;
    u32 m_dicr = 0;

    Bus* m_bus = nullptr;
    GPU* m_gpu = nullptr;
    CDROM* m_cdrom = nullptr;
    SPU* m_spu = nullptr;
    MDEC* m_mdec = nullptr;
    InterruptController* m_irq = nullptr;
    bool m_irq_level = false;

    void run_channel(int ch);
    void transfer_block(int ch);
    void transfer_linked_list(int ch);
    void complete_channel(int ch);
    void update_irq_line();
};

} // namespace ps96
