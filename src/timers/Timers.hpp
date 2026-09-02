#pragma once

#include "common/Types.hpp"
#include <array>
#include <functional>

namespace ps96 {

class InterruptController;

class Timers {
public:
    Timers();
    void reset();
    void set_irq(InterruptController* irq) { m_irq = irq; }

    u32 read32(u32 addr);
    void write32(u32 addr, u32 value);
    void tick(int cycles);

private:
    struct Timer {
        u32 counter = 0;
        u32 target = 0;
        u32 mode = 0;
        bool irq_done = false;
        double div_accum = 0.0;
    };
    std::array<Timer, 3> m_timers{};
    InterruptController* m_irq = nullptr;

    void check_irq(int idx);
};

} // namespace ps96
