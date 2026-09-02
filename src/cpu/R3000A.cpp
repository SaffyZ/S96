#include "cpu/R3000A.hpp"
#include "memory/Bus.hpp"
#include "gte/GTE.hpp"
#include "common/Log.hpp"
#include <cstdio>

namespace ps96 {

R3000A::R3000A() {
    reset();
}

void R3000A::reset() {
    for (auto& r : m_gpr) r = 0;
    m_pc = 0xBFC00000;
    m_next_pc = 0xBFC00004;
    m_hi = 0;
    m_lo = 0;
    m_in_delay_slot = false;
    m_took_branch = false;
    m_load_delay_reg = 0;
    m_load_delay_value = 0;
    m_load_delay_reg_next = 0;
    m_load_delay_value_next = 0;
    m_instr_count = 0;
    m_repeat_pc = 0;
    m_repeat_count = 0;
        m_cop0.reset();
}

void R3000A::write_reg(int rd, u32 value) {
    if (rd != 0) {
        // If a load delay is targeting this reg, cancel it
        if (m_load_delay_reg == rd) {
            m_load_delay_reg = 0;
        }
        m_gpr[rd] = value;
    }
}

void R3000A::do_load(int rt, u32 value) {
    if (rt == 0) return; // r0 never loads
    // A newer load to the same reg cancels a still-pending one
    if (m_load_delay_reg == rt)
        m_load_delay_reg = 0;
    m_load_delay_reg_next = rt;
    m_load_delay_value_next = value;
}

u32 R3000A::fetch() {
    if (!m_bus) return 0;
    return m_bus->read32(m_pc);
}

void R3000A::handle_exception(Exception code, u32 bad_vaddr) {
    // EPC must be the faulting instruction (or the branch if in a delay slot)
    u32 fault_pc = m_current_pc ? m_current_pc : (m_pc - 4);
    static int s_ex = 0;
    if (s_ex < 8) {
        u32 fault_instr = m_bus ? m_bus->read32(fault_pc) : 0;
        Log::info("EXCEPTION code=%u fault_pc=%08X instr=%08X next_pc=%08X delay=%d bad=%08X R13=%08X R14=%08X SP=%08X RA=%08X",
            (unsigned)code, fault_pc, fault_instr, m_pc, m_in_delay_slot ? 1 : 0, bad_vaddr,
            m_gpr[13], m_gpr[14], m_gpr[29], m_gpr[31]);
        s_ex++;
    }
    // All exceptions, including SYSCALL, vector through COP0's hardware
    // exception path.  Do not special-case BIOS contents or installed kernel
    // vectors here: the CPU only knows the architectural Cause/EPC/Status
    // behavior and lets the firmware own the exception handler.

    u32 vector = m_cop0.enter_exception(code, fault_pc, m_in_delay_slot, bad_vaddr);
    m_pc = vector;
    m_next_pc = vector + 4;
    m_in_delay_slot = false;
    m_took_branch = false;
    m_load_delay_reg = 0;
    m_load_delay_reg_next = 0;
    m_load_delay_value = 0;
    m_load_delay_value_next = 0;
}

void R3000A::raise_interrupt() {
    if (m_cop0.interrupts_enabled() && m_cop0.interrupt_pending()) {
        handle_exception(Exception::Interrupt);
    }
}

int R3000A::step() {
    // ---- Load delay (R3000A) ----
    // A load in instruction N must NOT be visible to instruction N+1; it becomes
    // visible starting at N+2. We keep the pending value in m_load_delay_* and
    // only commit it to the GPR file AFTER the current instruction executes.
    // (Previously we committed at the START of N+1, which made the loaded value
    // available one instruction too early — breaking jr/lw pairs, A0 dispatch,
    // and exception handlers → AdEL loops at 000000A0.)

    const bool delay_slot = m_took_branch;
    m_in_delay_slot = delay_slot;
    m_took_branch = false;

    // An external interrupt is recognized between instructions, never in the
    // branch delay slot.  The R3000A always executes the delay-slot instruction
    // after a taken branch before taking a pending interrupt.  Pre-empting the
    // delay slot corrupts EPC/BD and can strand the BIOS in its SCE/CD-ROM loops.
    if (!delay_slot && m_cop0.interrupts_enabled() && m_cop0.interrupt_pending()) {
        m_current_pc = m_pc;
        if (m_load_delay_reg != 0) {
            m_gpr[m_load_delay_reg] = m_load_delay_value;
            m_load_delay_reg = 0;
        }
        handle_exception(Exception::Interrupt);
        m_current_pc = 0;
        return 1;
    }

    m_current_pc = m_pc;

    if (m_pc & 3) {
        handle_exception(Exception::AddrErrorLoad, m_pc);
        m_current_pc = 0;
        return 1;
    }

    u32 p = phys_addr(m_pc);
    bool in_ram  = (p < 0x00800000);
    bool in_bios = (p >= ADDR_BIOS && p < ADDR_BIOS + BIOS_SIZE);
    bool in_scratch = (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE);
    if (!in_ram && !in_bios && !in_scratch) {
        static int s_bus_err_logs = 0;
        if (s_bus_err_logs < 8) {
            Log::warn("Instruction fetch outside memory at PC=%08X (phys %08X)", m_pc, p);
            s_bus_err_logs++;
        }
        handle_exception(Exception::BusErrorInstr, m_pc);
        m_current_pc = 0;
        return 1;
    }

    u32 instr = m_bus ? m_bus->read32(m_pc) : 0;
    u32 fetch_pc = m_pc;

    // A repeated-PC watchdog is only diagnostically active at debug level.
    // Long identical PC loops are a useful signal for BIOS/game hangs without
    // hard-coding any address or title-specific behavior. Emit the architectural
    // interrupt state so the failing wait loop can be traced to the missing
    // device/IRQ response.
    if (Log::level() <= LogLevel::Debug) {
        if (fetch_pc == m_repeat_pc) {
            ++m_repeat_count;
        } else {
            m_repeat_pc = fetch_pc;
            m_repeat_count = 1;
        }
        if ((m_repeat_count == 1000000 || m_repeat_count == 5000000 ||
             (m_repeat_count > 5000000 && (m_repeat_count % 5000000) == 0))) {
            Log::debug("CPU PC WATCHDOG pc=%08X count=%llu instr=%08X cause=%08X status=%08X epc=%08X bad=%08X",
                fetch_pc, static_cast<unsigned long long>(m_repeat_count), instr,
                m_cop0.cause(), m_cop0.status(), m_cop0.epc(), 0u);
        }
    }

    // Advance PC before execute (branch delay semantics).
    m_pc = m_next_pc;
    m_next_pc += 4;

    // Execute uses GPRs WITHOUT this instruction's load result (and without the
    // still-pending previous load — that stays invisible until after execute).
    execute(instr);

    // Commit the load that was produced by the PREVIOUS instruction so the
    // NEXT instruction will see it. Then promote any load this instruction
    // just scheduled into the pending slot.
    if (m_load_delay_reg != 0) {
        m_gpr[m_load_delay_reg] = m_load_delay_value;
        m_load_delay_reg = 0;
    }
    if (m_load_delay_reg_next != 0) {
        m_load_delay_reg = m_load_delay_reg_next;
        m_load_delay_value = m_load_delay_value_next;
        m_load_delay_reg_next = 0;
    }

    m_instr_count++;

    m_current_pc = 0;

    if (Log::cpu_trace()) {
        Log::cpu("%08X: %08X", fetch_pc, instr);
    }

    return 1;
}

void R3000A::execute(u32 instr) {
    u32 opcode = (instr >> 26) & 0x3F;

    switch (opcode) {
        case 0x00: op_special(instr); break;
        case 0x01: op_regimm(instr); break;
        case 0x02: op_j(instr); break;
        case 0x03: op_jal(instr); break;
        case 0x04: op_beq(instr); break;
        case 0x05: op_bne(instr); break;
        case 0x06: op_blez(instr); break;
        case 0x07: op_bgtz(instr); break;
        case 0x08: op_addi(instr); break;
        case 0x09: op_addiu(instr); break;
        case 0x0A: op_slti(instr); break;
        case 0x0B: op_sltiu(instr); break;
        case 0x0C: op_andi(instr); break;
        case 0x0D: op_ori(instr); break;
        case 0x0E: op_xori(instr); break;
        case 0x0F: op_lui(instr); break;
        case 0x10: op_cop0(instr); break;
        case 0x12: op_cop2(instr); break;
        case 0x20: op_lb(instr); break;
        case 0x21: op_lh(instr); break;
        case 0x22: op_lwl(instr); break;
        case 0x23: op_lw(instr); break;
        case 0x24: op_lbu(instr); break;
        case 0x25: op_lhu(instr); break;
        case 0x26: op_lwr(instr); break;
        case 0x28: op_sb(instr); break;
        case 0x29: op_sh(instr); break;
        case 0x2A: op_swl(instr); break;
        case 0x2B: op_sw(instr); break;
        case 0x2E: op_swr(instr); break;
        case 0x32: op_lwc2(instr); break;
        case 0x3A: op_swc2(instr); break;
        default: {
            static int s_unk_logs = 0;
            static u32 s_last_pc = 0xFFFFFFFF;
            u32 at = m_current_pc ? m_current_pc : (m_pc - 4);
            if (s_unk_logs < 16 && at != s_last_pc) {
                Log::warn("Unknown opcode %02X at %08X (instr may be open-bus)", opcode, at);
                s_unk_logs++;
                s_last_pc = at;
            }
            handle_exception(Exception::ReservedInstr);
            break;
        }
    }
}

void R3000A::branch(bool condition, s32 offset) {
    if (condition) {
        m_next_pc = m_pc + (offset << 2);
        m_took_branch = true;
    }
}

void R3000A::jump(u32 target) {
    m_next_pc = target;
    m_took_branch = true;
}

// ---- SPECIAL ----
void R3000A::op_special(u32 instr) {
    u32 funct = instr & 0x3F;
    switch (funct) {
        case 0x00: special_sll(instr); break;
        case 0x02: special_srl(instr); break;
        case 0x03: special_sra(instr); break;
        case 0x04: special_sllv(instr); break;
        case 0x06: special_srlv(instr); break;
        case 0x07: special_srav(instr); break;
        case 0x08: special_jr(instr); break;
        case 0x09: special_jalr(instr); break;
        case 0x0C: special_syscall(instr); break;
        case 0x0D: special_break(instr); break;
        case 0x10: special_mfhi(instr); break;
        case 0x11: special_mthi(instr); break;
        case 0x12: special_mflo(instr); break;
        case 0x13: special_mtlo(instr); break;
        case 0x18: special_mult(instr); break;
        case 0x19: special_multu(instr); break;
        case 0x1A: special_div(instr); break;
        case 0x1B: special_divu(instr); break;
        case 0x20: special_add(instr); break;
        case 0x21: special_addu(instr); break;
        case 0x22: special_sub(instr); break;
        case 0x23: special_subu(instr); break;
        case 0x24: special_and(instr); break;
        case 0x25: special_or(instr); break;
        case 0x26: special_xor(instr); break;
        case 0x27: special_nor(instr); break;
        case 0x2A: special_slt(instr); break;
        case 0x2B: special_sltu(instr); break;
        default:
            Log::warn("Unknown SPECIAL funct %02X", funct);
            handle_exception(Exception::ReservedInstr);
            break;
    }
}

void R3000A::special_sll(u32 instr) {
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    int sa = (instr >> 6) & 0x1F;
    write_reg(rd, m_gpr[rt] << sa);
}
void R3000A::special_srl(u32 instr) {
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    int sa = (instr >> 6) & 0x1F;
    write_reg(rd, m_gpr[rt] >> sa);
}
void R3000A::special_sra(u32 instr) {
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    int sa = (instr >> 6) & 0x1F;
    write_reg(rd, static_cast<u32>(static_cast<s32>(m_gpr[rt]) >> sa));
}
void R3000A::special_sllv(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rt] << (m_gpr[rs] & 0x1F));
}
void R3000A::special_srlv(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rt] >> (m_gpr[rs] & 0x1F));
}
void R3000A::special_srav(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, static_cast<u32>(static_cast<s32>(m_gpr[rt]) >> (m_gpr[rs] & 0x1F)));
}
void R3000A::special_jr(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    jump(m_gpr[rs]);
}
void R3000A::special_jalr(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    // Target must be sampled BEFORE writing the link register — when rd==rs
    // (or rd aliases the source) writing first would corrupt the jump address.
    // m_next_pc is already the instruction after the delay slot (correct link).
    u32 target = m_gpr[rs];
    write_reg(rd, m_next_pc);
    jump(target);
}
void R3000A::special_syscall(u32) {
    handle_exception(Exception::Syscall);
}
void R3000A::special_break(u32) {
    handle_exception(Exception::Breakpoint);
}
void R3000A::special_mfhi(u32 instr) {
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_hi);
}
void R3000A::special_mthi(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    m_hi = m_gpr[rs];
}
void R3000A::special_mflo(u32 instr) {
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_lo);
}
void R3000A::special_mtlo(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    m_lo = m_gpr[rs];
}
void R3000A::special_mult(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s64 result = static_cast<s64>(static_cast<s32>(m_gpr[rs])) * static_cast<s64>(static_cast<s32>(m_gpr[rt]));
    m_lo = static_cast<u32>(result);
    m_hi = static_cast<u32>(result >> 32);
}
void R3000A::special_multu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u64 result = static_cast<u64>(m_gpr[rs]) * static_cast<u64>(m_gpr[rt]);
    m_lo = static_cast<u32>(result);
    m_hi = static_cast<u32>(result >> 32);
}
void R3000A::special_div(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 dividend = static_cast<s32>(m_gpr[rs]);
    s32 divisor  = static_cast<s32>(m_gpr[rt]);
    if (divisor == 0) {
        // PS1 behavior: lo = 0xFFFFFFFF or 1 depending on sign, hi = dividend
        m_hi = static_cast<u32>(dividend);
        m_lo = (dividend >= 0) ? 0xFFFFFFFFu : 1u;
    } else if (dividend == static_cast<s32>(0x80000000) && divisor == -1) {
        m_lo = 0x80000000;
        m_hi = 0;
    } else {
        m_lo = static_cast<u32>(dividend / divisor);
        m_hi = static_cast<u32>(dividend % divisor);
    }
}
void R3000A::special_divu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u32 dividend = m_gpr[rs];
    u32 divisor  = m_gpr[rt];
    if (divisor == 0) {
        m_hi = dividend;
        m_lo = 0xFFFFFFFFu;
    } else {
        m_lo = dividend / divisor;
        m_hi = dividend % divisor;
    }
}
void R3000A::special_add(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    s32 a = static_cast<s32>(m_gpr[rs]);
    s32 b = static_cast<s32>(m_gpr[rt]);
    s32 result = a + b;
    // Overflow detection
    if (((a ^ result) & (b ^ result)) < 0) {
        handle_exception(Exception::Overflow);
        return;
    }
    write_reg(rd, static_cast<u32>(result));
}
void R3000A::special_addu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rs] + m_gpr[rt]);
}
void R3000A::special_sub(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    s32 a = static_cast<s32>(m_gpr[rs]);
    s32 b = static_cast<s32>(m_gpr[rt]);
    s32 result = a - b;
    if (((a ^ b) & (a ^ result)) < 0) {
        handle_exception(Exception::Overflow);
        return;
    }
    write_reg(rd, static_cast<u32>(result));
}
void R3000A::special_subu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rs] - m_gpr[rt]);
}
void R3000A::special_and(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rs] & m_gpr[rt]);
}
void R3000A::special_or(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rs] | m_gpr[rt]);
}
void R3000A::special_xor(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, m_gpr[rs] ^ m_gpr[rt]);
}
void R3000A::special_nor(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, ~(m_gpr[rs] | m_gpr[rt]));
}
void R3000A::special_slt(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, (static_cast<s32>(m_gpr[rs]) < static_cast<s32>(m_gpr[rt])) ? 1u : 0u);
}
void R3000A::special_sltu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    write_reg(rd, (m_gpr[rs] < m_gpr[rt]) ? 1u : 0u);
}

// ---- REGIMM ----
void R3000A::op_regimm(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    s32 val = static_cast<s32>(m_gpr[rs]);
    switch (rt) {
        case 0x00: branch(val < 0, offset); break;  // BLTZ
        case 0x01: branch(val >= 0, offset); break; // BGEZ
        case 0x10: // BLTZAL
            write_reg(31, m_next_pc);
            branch(val < 0, offset);
            break;
        case 0x11: // BGEZAL
            write_reg(31, m_next_pc);
            branch(val >= 0, offset);
            break;
        default:
            handle_exception(Exception::ReservedInstr);
            break;
    }
}

void R3000A::op_j(u32 instr) {
    u32 target = (m_pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
    jump(target);
}
void R3000A::op_jal(u32 instr) {
    write_reg(31, m_next_pc);
    u32 target = (m_pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
    jump(target);
}
void R3000A::op_beq(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    branch(m_gpr[rs] == m_gpr[rt], offset);
}
void R3000A::op_bne(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    branch(m_gpr[rs] != m_gpr[rt], offset);
}
void R3000A::op_blez(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    branch(static_cast<s32>(m_gpr[rs]) <= 0, offset);
}
void R3000A::op_bgtz(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    branch(static_cast<s32>(m_gpr[rs]) > 0, offset);
}

void R3000A::op_addi(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 imm = sex16(instr & 0xFFFF);
    s32 a = static_cast<s32>(m_gpr[rs]);
    s32 result = a + imm;
    if (((a ^ result) & (imm ^ result)) < 0) {
        handle_exception(Exception::Overflow);
        return;
    }
    write_reg(rt, static_cast<u32>(result));
}
void R3000A::op_addiu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 imm = sex16(instr & 0xFFFF);
    write_reg(rt, static_cast<u32>(static_cast<s32>(m_gpr[rs]) + imm));
}
void R3000A::op_slti(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 imm = sex16(instr & 0xFFFF);
    write_reg(rt, (static_cast<s32>(m_gpr[rs]) < imm) ? 1u : 0u);
}
void R3000A::op_sltiu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u32 imm = static_cast<u32>(sex16(instr & 0xFFFF));
    write_reg(rt, (m_gpr[rs] < imm) ? 1u : 0u);
}
void R3000A::op_andi(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u32 imm = instr & 0xFFFF;
    write_reg(rt, m_gpr[rs] & imm);
}
void R3000A::op_ori(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u32 imm = instr & 0xFFFF;
    write_reg(rt, m_gpr[rs] | imm);
}
void R3000A::op_xori(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    u32 imm = instr & 0xFFFF;
    write_reg(rt, m_gpr[rs] ^ imm);
}
void R3000A::op_lui(u32 instr) {
    int rt = (instr >> 16) & 0x1F;
    write_reg(rt, (instr & 0xFFFF) << 16);
}

void R3000A::op_cop0(u32 instr) {
    u32 rs = (instr >> 21) & 0x1F;
    switch (rs) {
        case 0x00: { // MFC0
            int rt = (instr >> 16) & 0x1F;
            int rd = (instr >> 11) & 0x1F;
            do_load(rt, m_cop0.read(rd));
            break;
        }
        case 0x04: { // MTC0
            int rt = (instr >> 16) & 0x1F;
            int rd = (instr >> 11) & 0x1F;
            m_cop0.write(rd, m_gpr[rt]);
            // R3000A samples external interrupts when Status.IEc becomes 1.
            // step() already advanced PC past this MTC0, so EPC must be the
            // *next* instruction (MTC0 has completed). Without this check,
            // VBlank latched during IE=0 is often lost and software locks
            // (e.g. 0x8004445C) never clear — permanent LOCK_WAIT spins.
            if (rd == 12 && m_cop0.interrupts_enabled() && m_cop0.interrupt_pending()) {
                m_current_pc = m_pc; // already next instr
                m_in_delay_slot = false;
                handle_exception(Exception::Interrupt);
                m_next_pc = m_pc; // do not skip first handler insn
            }
            break;
        }
        case 0x10: { // RFE (or other CO)
            u32 funct = instr & 0x3F;
            if (funct == 0x10) {
                m_cop0.rfe();
                if (m_cop0.interrupts_enabled() && m_cop0.interrupt_pending()) {
                    m_current_pc = m_pc;
                    m_in_delay_slot = false;
                    handle_exception(Exception::Interrupt);
                    m_next_pc = m_pc;
                }
            } else {
                Log::warn("Unknown COP0 CO funct %02X", funct);
            }
            break;
        }
        default:
            Log::warn("Unknown COP0 rs %02X", rs);
            break;
    }
}

void R3000A::op_cop2(u32 instr) {
    if (!m_gte) return;
    u32 rs = (instr >> 21) & 0x1F;
    if (rs & 0x10) {
        // COP2 command
        m_gte->execute(instr);
    } else {
        switch (rs) {
            case 0x00: { // MFC2
                int rt = (instr >> 16) & 0x1F;
                int rd = (instr >> 11) & 0x1F;
                do_load(rt, m_gte->read_data(rd));
                break;
            }
            case 0x02: { // CFC2
                int rt = (instr >> 16) & 0x1F;
                int rd = (instr >> 11) & 0x1F;
                do_load(rt, m_gte->read_control(rd));
                break;
            }
            case 0x04: { // MTC2
                int rt = (instr >> 16) & 0x1F;
                int rd = (instr >> 11) & 0x1F;
                m_gte->write_data(rd, m_gpr[rt]);
                break;
            }
            case 0x06: { // CTC2
                int rt = (instr >> 16) & 0x1F;
                int rd = (instr >> 11) & 0x1F;
                m_gte->write_control(rd, m_gpr[rt]);
                break;
            }
            default:
                break;
        }
    }
}

// ---- Loads / Stores ----
void R3000A::op_lb(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    u8 v = m_bus->read8(addr);
    do_load(rt, static_cast<u32>(sex8(v)));
}
void R3000A::op_lh(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 1) {
        handle_exception(Exception::AddrErrorLoad, addr);
        return;
    }
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    u16 v = m_bus->read16(addr);
    do_load(rt, static_cast<u32>(sex16(v)));
}
void R3000A::op_lw(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 3) {
        handle_exception(Exception::AddrErrorLoad, addr);
        return;
    }
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    do_load(rt, m_bus->read32(addr));
}
void R3000A::op_lbu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    do_load(rt, m_bus->read8(addr));
}
void R3000A::op_lhu(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 1) {
        handle_exception(Exception::AddrErrorLoad, addr);
        return;
    }
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    do_load(rt, m_bus->read16(addr));
}
void R3000A::op_lwl(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    u32 aligned = addr & ~3u;
    u32 word = m_bus->read32(aligned);
    // Merge against the architectural register (load delay not yet visible).
    // LWL/LWR pairs are deliberately adjacent on MIPS. The second half must
    // merge against the first half's still-pending load result, even though
    // ordinary loads obey the one-instruction load delay.
    u32 cur = (m_load_delay_reg == rt) ? m_load_delay_value : m_gpr[rt];
    static const u32 lwl_mask[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
    static const int lwl_shift[4] = {24, 16, 8, 0};
    int align = addr & 3;
    u32 value = (cur & lwl_mask[align]) | (word << lwl_shift[align]);
    // A load to the same rt cancels any still-pending load to that reg.
    if (m_load_delay_reg == rt)
        m_load_delay_reg = 0;
    do_load(rt, value);
}
void R3000A::op_lwr(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC only affects data cache. KSEG1 (uncached) still reads RAM.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000 || (p >= ADDR_SCRATCHPAD && p < ADDR_SCRATCHPAD + SCRATCHPAD_SIZE)) {
                do_load(rt, 0);
                return;
            }
        }
    }
    u32 aligned = addr & ~3u;
    u32 word = m_bus->read32(aligned);
    u32 cur = (m_load_delay_reg == rt) ? m_load_delay_value : m_gpr[rt];
    static const u32 lwr_mask[4] = {0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
    static const int lwr_shift[4] = {0, 8, 16, 24};
    int align = addr & 3;
    u32 value = (cur & lwr_mask[align]) | (word >> lwr_shift[align]);
    if (m_load_delay_reg == rt)
        m_load_delay_reg = 0;
    do_load(rt, value);
}

void R3000A::op_sb(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC isolates the *data cache*. KSEG1 (uncached) must still hit RAM —
        // BIOS kernel copy uses KSEG1 while IsC is set.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000) return;
        }
    }
    m_bus->write8(addr, static_cast<u8>(m_gpr[rt]));
}
void R3000A::op_sh(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 1) {
        handle_exception(Exception::AddrErrorStore, addr);
        return;
    }
    if (m_cop0.isolate_cache()) {
        // IsC isolates the *data cache*. KSEG1 (uncached) must still hit RAM —
        // BIOS kernel copy uses KSEG1 while IsC is set.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000) return;
        }
    }
    m_bus->write16(addr, static_cast<u16>(m_gpr[rt]));
}
void R3000A::op_sw(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 3) {
        handle_exception(Exception::AddrErrorStore, addr);
        return;
    }
    if (m_cop0.isolate_cache()) {
        // IsC isolates the *data cache*. KSEG1 (uncached) must still hit RAM —
        // BIOS kernel copy uses KSEG1 while IsC is set.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000) return;
        }
    }
    m_bus->write32(addr, m_gpr[rt]);
}
void R3000A::op_swl(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC isolates the *data cache*. KSEG1 (uncached) must still hit RAM —
        // BIOS kernel copy uses KSEG1 while IsC is set.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000) return;
        }
    }
    u32 aligned = addr & ~3u;
    u32 word = m_bus->read32(aligned);
    int align = addr & 3;
    static const u32 swl_mask[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};
    static const int swl_shift[4] = {24, 16, 8, 0};
    u32 value = (word & swl_mask[align]) | (m_gpr[rt] >> swl_shift[align]);
    m_bus->write32(aligned, value);
}
void R3000A::op_swr(u32 instr) {
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (m_cop0.isolate_cache()) {
        // IsC isolates the *data cache*. KSEG1 (uncached) must still hit RAM —
        // BIOS kernel copy uses KSEG1 while IsC is set.
        if (!is_kseg1(addr)) {
            u32 p = phys_addr(addr);
            if (p < 0x00800000) return;
        }
    }
    u32 aligned = addr & ~3u;
    u32 word = m_bus->read32(aligned);
    int align = addr & 3;
    static const u32 swr_mask[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
    static const int swr_shift[4] = {0, 8, 16, 24};
    u32 value = (word & swr_mask[align]) | (m_gpr[rt] << swr_shift[align]);
    m_bus->write32(aligned, value);
}

void R3000A::op_lwc2(u32 instr) {
    if (!m_gte) return;
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 3) {
        handle_exception(Exception::AddrErrorLoad, addr);
        return;
    }
    u32 value = m_bus->read32(addr);
    m_gte->write_data(rt, value);
}
void R3000A::op_swc2(u32 instr) {
    if (!m_gte) return;
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    s32 offset = sex16(instr & 0xFFFF);
    u32 addr = m_gpr[rs] + offset;
    if (addr & 3) {
        handle_exception(Exception::AddrErrorStore, addr);
        return;
    }
    m_bus->write32(addr, m_gte->read_data(rt));
}

} // namespace ps96
