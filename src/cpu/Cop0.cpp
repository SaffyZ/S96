#include "cpu/Cop0.hpp"
#include "common/Log.hpp"

namespace ps96 {

Cop0::Cop0() {
    reset();
}

void Cop0::reset() {
    for (auto& r : m_regs) r = 0;
    // BEV=1 (bit 22): use boot exception vectors in BIOS (0xBFC00xxx)
    // until the BIOS relocates handlers into RAM.
    m_status = (1u << 22);
    m_cause  = 0;
    m_epc    = 0;
    m_badvaddr = 0;
    m_prid   = 0x00000002;
    m_regs[static_cast<u32>(Cop0Reg::Status)] = m_status;
    m_regs[static_cast<u32>(Cop0Reg::PRId)] = m_prid;
}

u32 Cop0::read(u32 reg) const {
    switch (static_cast<Cop0Reg>(reg)) {
        case Cop0Reg::Status:   return m_status;
        case Cop0Reg::Cause:    return m_cause;
        case Cop0Reg::EPC:      return m_epc;
        case Cop0Reg::BadVAddr: return m_badvaddr;
        case Cop0Reg::PRId:     return m_prid;
        case Cop0Reg::BPC:
        case Cop0Reg::BDA:
        case Cop0Reg::JumpDest:
        case Cop0Reg::DCIC:
        case Cop0Reg::BDAM:
        case Cop0Reg::BPCM:
            return m_regs[reg];
        default:
            return m_regs[reg];
    }
}

void Cop0::write(u32 reg, u32 value) {
    switch (static_cast<Cop0Reg>(reg)) {
        case Cop0Reg::Status:
            // Writable status; preserve read-only hard bits if any
            m_status = value;
            m_regs[reg] = m_status;
            break;
        case Cop0Reg::Cause:
            // Software can only write bits 8-9 (SW interrupts)
            m_cause = (m_cause & ~0x300u) | (value & 0x300u);
            m_regs[reg] = m_cause;
            break;
        case Cop0Reg::EPC:
            m_epc = value;
            m_regs[reg] = value;
            break;
        case Cop0Reg::BadVAddr:
            // Read-only from software perspective usually
            m_badvaddr = value;
            m_regs[reg] = value;
            break;
        case Cop0Reg::PRId:
            // Read-only
            break;
        default:
            m_regs[reg] = value;
            break;
    }
}

void Cop0::set_cause_ip(u32 bits) {
    m_cause |= (bits & 0xFF) << 8;
}

void Cop0::clear_cause_ip(u32 bits) {
    m_cause &= ~((bits & 0xFF) << 8);
}

bool Cop0::interrupts_enabled() const {
    // R3000A: IEc (bit0) must be set. After enter_exception, bits are shifted
    // so IEc is 0 until RFE — that alone prevents nesting.
    return (m_status & 1u) != 0;
}

bool Cop0::interrupt_pending() const {
    // Cause.IP & Status.IM
    u32 ip = (m_cause >> 8) & 0xFF;
    u32 im = (m_status >> 8) & 0xFF;
    return (ip & im) != 0;
}

u32 Cop0::enter_exception(Exception code, u32 pc, bool in_delay_slot, u32 bad_vaddr) {
    // Push interrupt enable / mode bits
    // Status: bits 0-5 shift left by 2, clear current
    u32 mode = m_status & 0x3F;
    m_status = (m_status & ~0x3Fu) | ((mode << 2) & 0x3F);

    // Cause
    m_cause = (m_cause & ~0x8000007Cu); // clear BD and ExcCode
    m_cause |= (static_cast<u32>(code) << 2);
    if (in_delay_slot) {
        m_cause |= (1u << 31); // BD
        m_epc = pc - 4;
    } else {
        m_epc = pc;
    }

    if (code == Exception::AddrErrorLoad || code == Exception::AddrErrorStore ||
        code == Exception::TLBLoad || code == Exception::TLBStore) {
        m_badvaddr = bad_vaddr;
    }

    m_regs[static_cast<u32>(Cop0Reg::Status)] = m_status;
    m_regs[static_cast<u32>(Cop0Reg::Cause)]  = m_cause;
    m_regs[static_cast<u32>(Cop0Reg::EPC)]    = m_epc;
    m_regs[static_cast<u32>(Cop0Reg::BadVAddr)] = m_badvaddr;

    // BEV bit (bit 22 of Status) selects exception vector
    bool bev = (m_status & (1u << 22)) != 0;
    u32 vector;
    if (code == Exception::TLBLoad || code == Exception::TLBStore || code == Exception::TLBMod) {
        vector = bev ? 0xBFC00100 : 0x80000000;
    } else {
        vector = bev ? 0xBFC00180 : 0x80000080;
    }
    return vector;
}

void Cop0::rfe() {
    // R3000A RFE (nocash):
    //   bit0 <- bit2, bit1 <- bit3, bit2 <- bit4, bit3 <- bit5
    //   bit4 and bit5 unchanged.
    // Previous code wrote only bits 0-3 and zeroed 4-5, collapsing the KU/IE
    // stack after nested syscall/IRQ — SCE logo loop never exited cleanly.
    u32 mode = m_status & 0x3Fu;
    u32 popped = ((mode >> 2) & 0x0Fu) | (mode & 0x30u);
    m_status = (m_status & ~0x3Fu) | popped;
    m_regs[static_cast<u32>(Cop0Reg::Status)] = m_status;
}

} // namespace ps96
