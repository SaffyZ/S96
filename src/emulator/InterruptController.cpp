#include "emulator/InterruptController.hpp"
#include "cpu/R3000A.hpp"
#include "memory/Bus.hpp"
#include "common/Log.hpp"

namespace ps96 {

InterruptController::InterruptController() { reset(); }

void InterruptController::reset() {
    m_status = 0;
    m_mask = 0;
}

void InterruptController::set_mask(u32 v) {
    m_mask = v & 0x7FF;
    update_cpu();
}

void InterruptController::acknowledge(u32 v) {
    // I_STAT is write-0-to-clear: writing a 0 clears that IRQ latch;
    // writing a 1 leaves the latch unchanged. (psx-spx / real hardware)
    static int s_acks = 0;
    if (s_acks < 40) {
        u32 pc = m_cpu ? m_cpu->pc() : 0;
        Log::info("ISTAT_ACK v=%03X old=%03X pc=%08X", v & 0x7FF, m_status & 0x7FF, pc);
        s_acks++;
    }
    m_status &= (v | ~0x7FFu);
    update_cpu();
}

void InterruptController::raise(Irq irq) {
    // Interrupt latches are real hardware state and must be raised regardless
    // of whether the BIOS has installed its RAM exception vectors yet.  The
    // previous C80-based gate was an emulator-side bootstrap hack: it silently
    // dropped VBlank/timer/DMA IRQs during BIOS initialization, which changes
    // the machine state and can deadlock the retail BIOS.
    m_status |= (1u << static_cast<u32>(irq));
    {
        static int s_r = 0;
        if (irq == CDROM && s_r < 20) {
            Log::info("IRQ_RAISE_STAT CDROM status=%03X mask=%03X", m_status & 0x7FF, m_mask & 0x7FF);
            s_r++;
        }
    }
    update_cpu();
}

void InterruptController::lower(Irq irq) {
    m_status &= ~(1u << static_cast<u32>(irq));
    update_cpu();
}

void InterruptController::update_cpu() {
    if (!m_cpu) return;
    if (m_status & m_mask) {
        m_cpu->cop0().set_cause_ip(0x04);
    } else {
        m_cpu->cop0().clear_cause_ip(0x04);
    }
}

} // namespace ps96
