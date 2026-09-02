#pragma once

#include "common/Types.hpp"
#include "cpu/Cop0.hpp"

namespace ps96 {

class Bus;
class GTE;

class R3000A {
public:
    R3000A();
    ~R3000A() = default;

    void reset();
    void set_bus(Bus* bus) { m_bus = bus; }
    void set_gte(GTE* gte) { m_gte = gte; }

    // Execute one instruction, returns cycles roughly used (usually 1)
    int step();

    // Force exception (external interrupt)
    void raise_interrupt();

    // Registers
    u32& reg(int i) { return m_gpr[i]; }
    u32  reg(int i) const { return m_gpr[i]; }
    u32  pc() const { return m_pc; }
    void set_pc(u32 pc) { m_pc = pc; m_next_pc = pc + 4; }

    u32 hi() const { return m_hi; }
    u32 lo() const { return m_lo; }

    Cop0& cop0() { return m_cop0; }
    const Cop0& cop0() const { return m_cop0; }

    // Load delay slot state
    bool in_delay_slot() const { return m_in_delay_slot; }

    // Stats
    u64 instruction_count() const { return m_instr_count; }

private:
    Bus*  m_bus = nullptr;
    GTE*  m_gte = nullptr;
    Cop0  m_cop0;

    u32 m_gpr[32]{};
    u32 m_pc = 0xBFC00000;      // BIOS entry
    u32 m_next_pc = 0xBFC00004;
    u32 m_hi = 0;
    u32 m_lo = 0;

    // Branch delay
    bool m_in_delay_slot = false;
    bool m_took_branch = false;

    // Load delay: register written one instruction later
    int  m_load_delay_reg = 0;
    u32  m_load_delay_value = 0;
    int  m_load_delay_reg_next = 0;
    u32  m_load_delay_value_next = 0;

    u64  m_instr_count = 0;
    u32  m_current_pc = 0; // PC of instruction currently in execute()
    u32  m_repeat_pc = 0;
    u64  m_repeat_count = 0;

    void write_reg(int rd, u32 value);
    void do_load(int rt, u32 value); // scheduled load

    u32 fetch();
    void execute(u32 instr);
    void handle_exception(Exception code, u32 bad_vaddr = 0);

    // Instruction helpers
    void op_special(u32 instr);
    void op_regimm(u32 instr);
    void op_j(u32 instr);
    void op_jal(u32 instr);
    void op_beq(u32 instr);
    void op_bne(u32 instr);
    void op_blez(u32 instr);
    void op_bgtz(u32 instr);
    void op_addi(u32 instr);
    void op_addiu(u32 instr);
    void op_slti(u32 instr);
    void op_sltiu(u32 instr);
    void op_andi(u32 instr);
    void op_ori(u32 instr);
    void op_xori(u32 instr);
    void op_lui(u32 instr);
    void op_cop0(u32 instr);
    void op_cop2(u32 instr);
    void op_lb(u32 instr);
    void op_lh(u32 instr);
    void op_lwl(u32 instr);
    void op_lw(u32 instr);
    void op_lbu(u32 instr);
    void op_lhu(u32 instr);
    void op_lwr(u32 instr);
    void op_sb(u32 instr);
    void op_sh(u32 instr);
    void op_swl(u32 instr);
    void op_sw(u32 instr);
    void op_swr(u32 instr);
    void op_lwc2(u32 instr);
    void op_swc2(u32 instr);

    // SPECIAL
    void special_sll(u32 instr);
    void special_srl(u32 instr);
    void special_sra(u32 instr);
    void special_sllv(u32 instr);
    void special_srlv(u32 instr);
    void special_srav(u32 instr);
    void special_jr(u32 instr);
    void special_jalr(u32 instr);
    void special_syscall(u32 instr);
    void special_break(u32 instr);
    void special_mfhi(u32 instr);
    void special_mthi(u32 instr);
    void special_mflo(u32 instr);
    void special_mtlo(u32 instr);
    void special_mult(u32 instr);
    void special_multu(u32 instr);
    void special_div(u32 instr);
    void special_divu(u32 instr);
    void special_add(u32 instr);
    void special_addu(u32 instr);
    void special_sub(u32 instr);
    void special_subu(u32 instr);
    void special_and(u32 instr);
    void special_or(u32 instr);
    void special_xor(u32 instr);
    void special_nor(u32 instr);
    void special_slt(u32 instr);
    void special_sltu(u32 instr);

    void branch(bool condition, s32 offset);
    void jump(u32 target);
};

} // namespace ps96
