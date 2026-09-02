#include "timers/Timers.hpp"
#include "emulator/InterruptController.hpp"
#include <cmath>

namespace ps96 {

Timers::Timers() { reset(); }

void Timers::reset() {
    for (auto& t : m_timers) {
        t.counter = 0;
        t.target = 0;
        t.mode = 0x0400; // bit10 idle (active-low IRQ-request, no request pending)
        t.irq_done = false;
        t.div_accum = 0;
    }
}

u32 Timers::read32(u32 addr) {
    int idx = static_cast<int>((addr - 0x1F801100) / 0x10);
    if (idx < 0 || idx > 2) return 0;
    u32 reg = ((addr - 0x1F801100) % 0x10) / 4;
    switch (reg) {
        case 0: return m_timers[idx].counter & 0xFFFF;
        case 1: {
            u32 mode = m_timers[idx].mode;
            // Reading mode clears reached-target / reached-overflow sticky
            // bits, and resets the (active-low) IRQ-request bit10 back to
            // idle (1) — real hardware auto-acknowledges bit10 on read.
            m_timers[idx].mode = (m_timers[idx].mode & ~0x1800u) | 0x0400u;
            return mode;
        }
        case 2: return m_timers[idx].target & 0xFFFF;
    }
    return 0;
}

void Timers::write32(u32 addr, u32 value) {
    int idx = static_cast<int>((addr - 0x1F801100) / 0x10);
    if (idx < 0 || idx > 2) return;
    u32 reg = ((addr - 0x1F801100) % 0x10) / 4;
    switch (reg) {
        case 0:
            m_timers[idx].counter = value & 0xFFFF;
            break;
        case 1:
            m_timers[idx].mode = (value & 0x3FF) | 0x0400u; // bit10 idle after (re)configure
            m_timers[idx].irq_done = false;
            m_timers[idx].counter = 0;
            m_timers[idx].div_accum = 0;
            // Reconfiguring the timer clears its internal request state.
            // I_STAT is a separate write-0-to-clear system interrupt latch.
            break;
        case 2:
            m_timers[idx].target = value & 0xFFFF;
            break;
    }
}

void Timers::tick(int cycles) {
    // NTSC timing used by this core: exactly 564480 CPU cycles per 263-line
    // frame. Keep the fractional line period instead of truncating to 2146;
    // otherwise Timer1 drifts by 82 CPU cycles every frame.
    constexpr double cycles_per_hline = 564480.0 / 263.0;

    for (int i = 0; i < 3; i++) {
        auto& t = m_timers[i];
        u32 src = (t.mode >> 8) & 3;

        // Clock source:
        //  Timer0: 00=sysclk, 01=dotclock (~sysclk/7), 10/11=sysclk
        //  Timer1: 00=sysclk, 01=hblank, 10/11=sysclk
        //  Timer2: 00=sysclk, 01=sysclk, 10=sysclk/8, 11=sysclk/8
        int tick_count = 0;
        if (i == 1 && src == 1) {
            // HBlank: one tick per scanline
            t.div_accum += cycles;
            tick_count = static_cast<int>(t.div_accum / cycles_per_hline);
            t.div_accum = std::fmod(t.div_accum, cycles_per_hline);
        } else if (i == 2 && (src & 2)) {
            // Sysclk / 8
            t.div_accum += cycles;
            tick_count = t.div_accum / 8;
            t.div_accum = std::fmod(t.div_accum, 8.0);
        } else if (i == 0 && src == 1) {
            // Dotclock ~ sysclk/7
            t.div_accum += cycles;
            tick_count = t.div_accum / 7;
            t.div_accum = std::fmod(t.div_accum, 7.0);
        } else {
            tick_count = cycles; // system clock
        }

        if (tick_count <= 0) continue;

        u32 prev = t.counter;
        u32 next = (prev + static_cast<u32>(tick_count)) & 0xFFFF;

        bool hit_target = false;
        bool hit_overflow = false;

        // Target reached: counter passes through exact target value this step.
        // Using prev < tgt <= end avoids continuous IRQs when target==0 and
        // counter starts at 0 (a common BIOS init state).
        {
            u32 tgt = t.target;
            u32 sum = prev + (u32)tick_count;
            if (tick_count > 0) {
                if (sum <= 0xFFFF) {
                    if (prev < tgt && tgt <= sum)
                        hit_target = true;
                } else {
                    u32 after = sum & 0xFFFF;
                    if (tgt > prev || tgt <= after)
                        hit_target = true;
                }
            }
        }
        if (next < prev)
            hit_overflow = true;

        t.counter = next;

        if (hit_target)
            t.mode |= 0x0800; // reached target
        if (hit_overflow)
            t.mode |= 0x1000; // reached overflow

        if ((t.mode & 0x08) && hit_target)
            t.counter = 0;

        bool irq = false;
        if ((t.mode & 0x10) && hit_target) irq = true;
        if ((t.mode & 0x20) && hit_overflow) irq = true;
        if (irq)
            check_irq(i);
    }
}

void Timers::check_irq(int idx) {
    if (!m_irq) return;
    auto& t = m_timers[idx];
    bool repeat = (t.mode & 0x40) != 0; // bit6: 0=one-shot, 1=repeat
    // One-shot mode: only the first qualifying event raises an IRQ; the
    // timer stays silent until its mode register is fully rewritten.
    // Repeat mode: every qualifying event (each target/overflow hit) is a
    // fresh IRQ pulse — real hardware does NOT require software to
    // re-arm a repeating timer between ticks. Gating repeat-mode timers
    // the same way as one-shot ones means anything driven by a repeating
    // timer (screen-hold timers, animation/audio pacing) advances once
    // and then hangs forever, which is exactly the bug this fixes.
    if (!repeat && t.irq_done)
        return;
    t.irq_done = true;
    t.mode &= ~0x0400u; // bit10 active-low: 0 = IRQ request pending
    static const InterruptController::Irq irqs[] = {
        InterruptController::TIMER0,
        InterruptController::TIMER1,
        InterruptController::TIMER2
    };
    m_irq->raise(irqs[idx]);
}

} // namespace ps96
