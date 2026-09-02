#pragma once

#include "common/Types.hpp"

namespace ps96 {

class R3000A;
class Bus;

class InterruptController {
public:
    enum Irq : u32 {
        VBLANK   = 0,
        GPU      = 1,
        CDROM    = 2,
        DMA      = 3,
        TIMER0   = 4,
        TIMER1   = 5,
        TIMER2   = 6,
        CONTROLLER = 7,
        SIO      = 8,
        SPU      = 9,
        PIO      = 10
    };

    InterruptController();
    void reset();
    void set_cpu(R3000A* cpu) { m_cpu = cpu; }
    void set_bus(Bus* bus) { m_bus = bus; }

    u32 status() const { return m_status; }
    u32 mask() const { return m_mask; }
    void set_mask(u32 v);
    void acknowledge(u32 v);

    void raise(Irq irq);
    void lower(Irq irq);

    void update_cpu();

private:
    u32 m_status = 0;
    u32 m_mask = 0;
    R3000A* m_cpu = nullptr;
    Bus* m_bus = nullptr;
};

} // namespace ps96
