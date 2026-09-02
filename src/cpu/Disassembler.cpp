#include "cpu/Disassembler.hpp"
#include <cstdio>
#include <string>

namespace ps96 {

std::string disassemble(u32 instr, u32 pc) {
    char buf[128];
    u32 op = (instr >> 26) & 0x3F;
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    int sa = (instr >> 6) & 0x1F;
    u32 imm = instr & 0xFFFF;
    u32 target = (pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
    std::snprintf(buf, sizeof(buf), "%08X", instr);
    // Minimal
    if (op == 0) {
        u32 f = instr & 0x3F;
        if (f == 0x00) std::snprintf(buf, sizeof(buf), "sll r%d, r%d, %d", rd, rt, sa);
        else if (f == 0x08) std::snprintf(buf, sizeof(buf), "jr r%d", rs);
        else if (f == 0x09) std::snprintf(buf, sizeof(buf), "jalr r%d, r%d", rd, rs);
        else if (f == 0x20) std::snprintf(buf, sizeof(buf), "add r%d, r%d, r%d", rd, rs, rt);
        else if (f == 0x21) std::snprintf(buf, sizeof(buf), "addu r%d, r%d, r%d", rd, rs, rt);
        else if (f == 0x24) std::snprintf(buf, sizeof(buf), "and r%d, r%d, r%d", rd, rs, rt);
        else if (f == 0x25) std::snprintf(buf, sizeof(buf), "or r%d, r%d, r%d", rd, rs, rt);
        else std::snprintf(buf, sizeof(buf), "special_%02X", f);
    } else if (op == 0x02) std::snprintf(buf, sizeof(buf), "j %08X", target);
    else if (op == 0x03) std::snprintf(buf, sizeof(buf), "jal %08X", target);
    else if (op == 0x04) std::snprintf(buf, sizeof(buf), "beq r%d, r%d, %04X", rs, rt, imm);
    else if (op == 0x05) std::snprintf(buf, sizeof(buf), "bne r%d, r%d, %04X", rs, rt, imm);
    else if (op == 0x09) std::snprintf(buf, sizeof(buf), "addiu r%d, r%d, %04X", rt, rs, imm);
    else if (op == 0x0D) std::snprintf(buf, sizeof(buf), "ori r%d, r%d, %04X", rt, rs, imm);
    else if (op == 0x0F) std::snprintf(buf, sizeof(buf), "lui r%d, %04X", rt, imm);
    else if (op == 0x23) std::snprintf(buf, sizeof(buf), "lw r%d, %04X(r%d)", rt, imm, rs);
    else if (op == 0x2B) std::snprintf(buf, sizeof(buf), "sw r%d, %04X(r%d)", rt, imm, rs);
    else std::snprintf(buf, sizeof(buf), "op_%02X", op);
    return buf;
}

} // namespace ps96
