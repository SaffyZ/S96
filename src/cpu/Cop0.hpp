#pragma once

#include "common/Types.hpp"

namespace ps96 {

class R3000A;

// COP0 registers relevant to PS1
enum class Cop0Reg : u32 {
    BPC      = 3,   // Breakpoint on execute
    BDA      = 5,   // Breakpoint on data access
    JumpDest = 6,   // Randomly memorized jump address
    DCIC     = 7,   // Breakpoint control
    BadVAddr = 8,   // Bad Virtual Address
    BDAM     = 9,   // Data Access breakpoint mask
    BPCM     = 11,  // Execute breakpoint mask
    Status   = 12,  // Status register
    Cause    = 13,  // Cause of last exception
    EPC      = 14,  // Exception Program Counter
    PRId     = 15,  // Processor Revision Identifier
};

// Exception codes
enum class Exception : u32 {
    Interrupt       = 0x00,
    TLBMod          = 0x01,
    TLBLoad         = 0x02,
    TLBStore        = 0x03,
    AddrErrorLoad   = 0x04,
    AddrErrorStore  = 0x05,
    BusErrorInstr   = 0x06,
    BusErrorData    = 0x07,
    Syscall         = 0x08,
    Breakpoint      = 0x09,
    ReservedInstr   = 0x0A,
    CopUnusable     = 0x0B,
    Overflow        = 0x0C,
};

class Cop0 {
public:
    Cop0();

    void reset();

    u32 read(u32 reg) const;
    void write(u32 reg, u32 value);

    u32 status() const { return m_status; }
    u32 cause()  const { return m_cause; }
    u32 epc()    const { return m_epc; }

    void set_cause_ip(u32 bits);
    void clear_cause_ip(u32 bits);
    u32  cause_ip() const { return (m_cause >> 8) & 0xFF; }

    bool interrupts_enabled() const;
    bool interrupt_pending() const;

    // Enter exception: sets EPC, Cause, Status, returns new PC
    u32 enter_exception(Exception code, u32 pc, bool in_delay_slot, u32 bad_vaddr = 0);

    // RFE - Restore From Exception (partial Status restore)
    void rfe();

    // Isolate cache bit
    bool isolate_cache() const { return (m_status & (1u << 16)) != 0; }

private:
    u32 m_regs[32]{};
    u32 m_status = 0;
    u32 m_cause  = 0;
    u32 m_epc    = 0;
    u32 m_badvaddr = 0;
    u32 m_prid   = 0x00000002; // R3000A
};

} // namespace ps96
