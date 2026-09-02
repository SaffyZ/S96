#include "gte/GTE.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps96 {

namespace {

static inline s32 sar64(s64 v, unsigned shift) {
    return shift ? static_cast<s32>(v >> shift) : static_cast<s32>(v);
}

static inline s64 mul_shift(s64 a, s64 b, unsigned shift) {
    return (a * b) >> shift;
}

}

GTE::GTE() { reset(); }

void GTE::reset() {
    m_data.fill(0);
    m_control.fill(0);
    m_mac[0] = m_mac[1] = m_mac[2] = m_mac[3] = 0;
    m_ir[0] = m_ir[1] = m_ir[2] = m_ir[3] = 0;
    m_flag = 0;
    m_lzcr = 0;
    m_sf = 1;
    m_lm = true;
    m_control[26] = 0x00000100;
}

void GTE::set_flag(u32 bit) {
    m_flag |= (1u << bit);
    if (bit >= 23 && bit <= 30) m_flag |= (1u << 31);
}

s32 GTE::saturate_ir(s32 v, int i, bool lm) {
    const s32 minv = lm ? 0 : -0x8000;
    const s32 maxv = 0x7FFF;
    if (v > maxv) { set_flag(24 - (i - 1)); return maxv; }
    if (v < minv) { set_flag(24 - (i - 1)); return minv; }
    return v;
}

s32 GTE::saturate_rgb(s32 v) {
    if (v < 0) { set_flag(21); return 0; }
    if (v > 0xFF) { set_flag(21); return 0xFF; }
    return v;
}

u32 GTE::read_data(u32 reg) const {
    if (reg >= 32) return 0;
    if (reg >= 8 && reg <= 11)
        return static_cast<u32>(static_cast<s32>(static_cast<s16>(m_data[reg] & 0xFFFF)));
    if (reg == 7 || (reg >= 16 && reg <= 19))
        return m_data[reg] & 0xFFFFu;
    if (reg == 28) return m_data[28] & 0x7FFFu;
    if (reg == 29) {
        const int r = std::clamp(static_cast<int>(static_cast<s16>(m_data[9])) / 0x80, 0, 31);
        const int g = std::clamp(static_cast<int>(static_cast<s16>(m_data[10])) / 0x80, 0, 31);
        const int b = std::clamp(static_cast<int>(static_cast<s16>(m_data[11])) / 0x80, 0, 31);
        return static_cast<u32>(r | (g << 5) | (b << 10));
    }
    if (reg == 31) return m_lzcr;
    return m_data[reg];
}

void GTE::write_data(u32 reg, u32 value) {
    if (reg >= 32) return;
    switch (reg) {
        case 8:
        case 9:
        case 10:
        case 11:
            m_data[reg] = value;
            m_ir[reg - 8] = static_cast<s16>(value & 0xFFFF);
            break;
        case 15:
            m_data[12] = m_data[13];
            m_data[13] = m_data[14];
            m_data[14] = value;
            m_data[15] = value;
            break;
        case 28:
            m_data[28] = value & 0x7FFF;
            m_data[9]  = static_cast<u32>((value & 0x1F) << 7);
            m_data[10] = static_cast<u32>(((value >> 5) & 0x1F) << 7);
            m_data[11] = static_cast<u32>(((value >> 10) & 0x1F) << 7);
            m_ir[1] = static_cast<s16>(m_data[9]);
            m_ir[2] = static_cast<s16>(m_data[10]);
            m_ir[3] = static_cast<s16>(m_data[11]);
            break;
        case 30: {
            m_data[30] = value;
            const u32 sign = value >> 31;
            u32 n = 0;
            while (n < 32 && ((value >> (31 - n)) & 1u) == sign) ++n;
            m_lzcr = n;
            m_data[31] = n;
            break;
        }
        default:
            m_data[reg] = value;
            break;
    }
}

u32 GTE::read_control(u32 reg) const {
    if (reg >= 32) return 0;
    if (reg == 31) return m_flag;
    return m_control[reg];
}

void GTE::write_control(u32 reg, u32 value) {
    if (reg >= 32 || reg == 31) return;
    m_control[reg] = value;
}

void GTE::sync_results() {
    m_data[24] = static_cast<u32>(m_mac[0]);
    m_data[25] = static_cast<u32>(m_mac[1]);
    m_data[26] = static_cast<u32>(m_mac[2]);
    m_data[27] = static_cast<u32>(m_mac[3]);
    m_data[8] = static_cast<u32>(m_ir[0]);
    m_data[9] = static_cast<u32>(m_ir[1]);
    m_data[10] = static_cast<u32>(m_ir[2]);
    m_data[11] = static_cast<u32>(m_ir[3]);
    m_control[31] = m_flag;
}

void GTE::execute(u32 instr) {
    m_flag = 0;
    m_sf = ((instr >> 19) & 1u) != 0;
    m_lm = ((instr >> 10) & 1u) != 0;
    switch (instr & 0x3F) {
        case 0x01: cmd_rtps(instr); break;
        case 0x06: cmd_nclip(); break;
        case 0x0C: cmd_op(); break;
        case 0x10: cmd_dpcs(); break;
        case 0x11: cmd_intpl(); break;
        case 0x12: cmd_mvmva(instr); break;
        case 0x13: cmd_ncds(); break;
        case 0x14: cmd_cdp(); break;
        case 0x16: cmd_ncdt(); break;
        case 0x1B: cmd_nccs(); break;
        case 0x1C: cmd_cc(); break;
        case 0x1E: cmd_ncs(); break;
        case 0x20: cmd_nct(); break;
        case 0x28: cmd_sqr(); break;
        case 0x29: cmd_dcpl(); break;
        case 0x2A: cmd_dpct(); break;
        case 0x2D: cmd_avsz3(); break;
        case 0x2E: cmd_avsz4(); break;
        case 0x30: cmd_rtpt(instr); break;
        case 0x3D: cmd_gpf(); break;
        case 0x3E: cmd_gpl(); break;
        case 0x3F: cmd_ncct(); break;
        default: break;
    }
    sync_results();
}

void GTE::load_vector(int idx, s32& x, s32& y, s32& z) {
    const u32 xy = m_data[idx * 2];
    const u32 zr = m_data[idx * 2 + 1];
    x = static_cast<s16>(xy & 0xFFFF);
    y = static_cast<s16>(xy >> 16);
    z = static_cast<s16>(zr & 0xFFFF);
}

void GTE::store_screen(s32 x, s32 y, s32 z) {
    m_data[12] = m_data[13];
    m_data[13] = m_data[14];
    if (x < -0x400) { x = -0x400; set_flag(14); }
    if (x >  0x3FF) { x =  0x3FF; set_flag(13); }
    if (y < -0x400) { y = -0x400; set_flag(12); }
    if (y >  0x3FF) { y =  0x3FF; set_flag(11); }
    m_data[14] = (static_cast<u32>(y & 0xFFFF) << 16) | static_cast<u32>(x & 0xFFFF);
    m_data[15] = m_data[14];

    m_data[16] = m_data[17];
    m_data[17] = m_data[18];
    m_data[18] = m_data[19];
    if (z < 0) { z = 0; set_flag(18); }
    if (z > 0xFFFF) { z = 0xFFFF; set_flag(18); }
    m_data[19] = static_cast<u32>(z);
}

u32 GTE::gte_divide(u32 h, u32 sz, bool& overflow) {
    overflow = false;
    if (sz == 0 || h >= (sz << 1)) {
        overflow = true;
        return 0x1FFFFu;
    }
    const u64 n = static_cast<u64>(h) * 0x10000ull + (sz >> 1);
    const u64 q = n / sz;
    if (q > 0x1FFFFu) {
        overflow = true;
        return 0x1FFFFu;
    }
    return static_cast<u32>(q);
}

void GTE::load_matrix(int base, s16 m[3][3]) const {
    m[0][0] = static_cast<s16>(m_control[base + 0] & 0xFFFF);
    m[0][1] = static_cast<s16>(m_control[base + 0] >> 16);
    m[0][2] = static_cast<s16>(m_control[base + 1] & 0xFFFF);
    m[1][0] = static_cast<s16>(m_control[base + 1] >> 16);
    m[1][1] = static_cast<s16>(m_control[base + 2] & 0xFFFF);
    m[1][2] = static_cast<s16>(m_control[base + 2] >> 16);
    m[2][0] = static_cast<s16>(m_control[base + 3] & 0xFFFF);
    m[2][1] = static_cast<s16>(m_control[base + 3] >> 16);
    m[2][2] = static_cast<s16>(m_control[base + 4] & 0xFFFF);
}

void GTE::push_color_from_mac(u32 code) {
    m_data[20] = m_data[21];
    m_data[21] = m_data[22];
    const u32 r = static_cast<u32>(saturate_rgb(m_mac[1] >> 4));
    const u32 g = static_cast<u32>(saturate_rgb(m_mac[2] >> 4));
    const u32 b = static_cast<u32>(saturate_rgb(m_mac[3] >> 4));
    m_data[22] = r | (g << 8) | (b << 16) | ((code & 0xFFu) << 24);
}

void GTE::light_transform() {
    s32 vx, vy, vz;
    load_vector(0, vx, vy, vz);
    s16 m[3][3];
    load_matrix(8, m);
    for (int row = 0; row < 3; ++row) {
        const s64 raw = s64(m[row][0]) * vx + s64(m[row][1]) * vy + s64(m[row][2]) * vz;
        const s32 mac = sar64(raw, m_sf ? 12 : 0);
        m_mac[row + 1] = mac;
        m_ir[row + 1] = static_cast<s16>(saturate_ir(mac, row + 1, m_lm));
    }
}

void GTE::color_matrix() {
    s16 m[3][3];
    load_matrix(16, m);
    const s32 bk[3] = { static_cast<s32>(m_control[13]), static_cast<s32>(m_control[14]), static_cast<s32>(m_control[15]) };
    for (int row = 0; row < 3; ++row) {
        const s64 raw = s64(bk[row]) * 0x1000ll
                      + s64(m[row][0]) * m_ir[1]
                      + s64(m[row][1]) * m_ir[2]
                      + s64(m[row][2]) * m_ir[3];
        const s32 mac = sar64(raw, m_sf ? 12 : 0);
        m_mac[row + 1] = mac;
        m_ir[row + 1] = static_cast<s16>(saturate_ir(mac, row + 1, m_lm));
    }
}

void GTE::color_apply() {
    const s32 r = static_cast<s32>(m_data[6] & 0xFF);
    const s32 g = static_cast<s32>((m_data[6] >> 8) & 0xFF);
    const s32 b = static_cast<s32>((m_data[6] >> 16) & 0xFF);
    m_mac[1] = sar64(s64(r) * m_ir[1] << 4, m_sf ? 12 : 0);
    m_mac[2] = sar64(s64(g) * m_ir[2] << 4, m_sf ? 12 : 0);
    m_mac[3] = sar64(s64(b) * m_ir[3] << 4, m_sf ? 12 : 0);
    m_ir[1] = static_cast<s16>(saturate_ir(m_mac[1], 1, m_lm));
    m_ir[2] = static_cast<s16>(saturate_ir(m_mac[2], 2, m_lm));
    m_ir[3] = static_cast<s16>(saturate_ir(m_mac[3], 3, m_lm));
}

void GTE::depth_cue_from_mac(s64 base1, s64 base2, s64 base3) {
    const s64 fc[3] = { s64(static_cast<s32>(m_control[21])) << 12,
                        s64(static_cast<s32>(m_control[22])) << 12,
                        s64(static_cast<s32>(m_control[23])) << 12 };
    const s64 base[3] = {base1, base2, base3};
    for (int i = 0; i < 3; ++i) {
        // The intermediate FC-MAC difference is saturated as if lm=0 before
        // being multiplied by IR0. This is an easy-to-miss hardware quirk and
        // is important for signed interpolation paths.
        s64 delta = (fc[i] - base[i]) >> (m_sf ? 12 : 0);
        delta = std::clamp(delta, s64(-0x8000), s64(0x7FFF));
        const s64 raw = base[i] + delta * m_ir[0];
        m_mac[i + 1] = sar64(raw, m_sf ? 12 : 0);
        m_ir[i + 1] = static_cast<s16>(saturate_ir(m_mac[i + 1], i + 1, m_lm));
    }
}

void GTE::cmd_rtps(u32) {
    s32 vx, vy, vz;
    load_vector(0, vx, vy, vz);
    s16 r[3][3];
    load_matrix(0, r);
    const s32 tr[3] = { static_cast<s32>(m_control[5]), static_cast<s32>(m_control[6]), static_cast<s32>(m_control[7]) };
    s64 raw[3]{};
    for (int row = 0; row < 3; ++row)
        raw[row] = s64(tr[row]) * 0x1000ll + s64(r[row][0]) * vx + s64(r[row][1]) * vy + s64(r[row][2]) * vz;

    for (int i = 0; i < 3; ++i) {
        m_mac[i + 1] = sar64(raw[i], m_sf ? 12 : 0);
        // RTPS IR saturation honors lm; IR3 overflow flag however checks the
        // signed lm=0 range on hardware.
        if (i == 2) {
            if (m_mac[3] < -0x8000 || m_mac[3] > 0x7FFF) set_flag(22);
        }
        m_ir[i + 1] = static_cast<s16>(saturate_ir(m_mac[i + 1], i + 1, m_lm));
    }

    s32 sz3 = sar64(m_mac[3], m_sf ? 0 : 12);
    if (sz3 < 0) { sz3 = 0; set_flag(18); }
    if (sz3 > 0xFFFF) { sz3 = 0xFFFF; set_flag(18); }
    m_data[16] = m_data[17];
    m_data[17] = m_data[18];
    m_data[18] = m_data[19];
    m_data[19] = static_cast<u32>(sz3);

    const u32 h = m_control[26] & 0xFFFFu;
    bool div_overflow = false;
    const u32 div = gte_divide(h, static_cast<u32>(sz3), div_overflow);
    if (div_overflow) set_flag(17);

    m_data[12] = m_data[13];
    m_data[13] = m_data[14];
    const s64 xmac = static_cast<s64>(m_ir[1]) * div + static_cast<s32>(m_control[24]);
    const s64 ymac = static_cast<s64>(m_ir[2]) * div + static_cast<s32>(m_control[25]);
    m_mac[0] = static_cast<s32>(xmac);
    s32 sx = static_cast<s32>(xmac >> 16);
    if (sx < -0x400) { sx = -0x400; set_flag(14); }
    if (sx >  0x3FF) { sx =  0x3FF; set_flag(13); }
    m_mac[0] = static_cast<s32>(ymac);
    s32 sy = static_cast<s32>(ymac >> 16);
    if (sy < -0x400) { sy = -0x400; set_flag(12); }
    if (sy >  0x3FF) { sy =  0x3FF; set_flag(11); }
    m_data[14] = (static_cast<u32>(sy & 0xFFFF) << 16) | static_cast<u32>(sx & 0xFFFF);
    m_data[15] = m_data[14];

    const s32 dqa = static_cast<s16>(m_control[27] & 0xFFFF);
    const s32 dqb = static_cast<s32>(m_control[28]);
    const s64 ir0mac = static_cast<s64>(div) * dqa + dqb;
    m_mac[0] = static_cast<s32>(ir0mac);
    s32 ir0 = static_cast<s32>(ir0mac >> 12);
    if (ir0 < 0) { ir0 = 0; set_flag(12); }
    if (ir0 > 0x1000) { ir0 = 0x1000; set_flag(12); }
    m_ir[0] = static_cast<s16>(ir0);
}

void GTE::cmd_rtpt(u32) {
    const u32 saved0 = m_data[0], saved1 = m_data[1];
    const u32 v0_0 = m_data[0], v0_1 = m_data[1];
    const u32 v1_0 = m_data[2], v1_1 = m_data[3];
    const u32 v2_0 = m_data[4], v2_1 = m_data[5];
    m_data[0] = v0_0; m_data[1] = v0_1; cmd_rtps(0);
    m_data[0] = v1_0; m_data[1] = v1_1; cmd_rtps(0);
    m_data[0] = v2_0; m_data[1] = v2_1; cmd_rtps(0);
    m_data[0] = saved0; m_data[1] = saved1;
}

void GTE::cmd_nclip() {
    const s32 sx0 = static_cast<s16>(m_data[12] & 0xFFFF);
    const s32 sy0 = static_cast<s16>(m_data[12] >> 16);
    const s32 sx1 = static_cast<s16>(m_data[13] & 0xFFFF);
    const s32 sy1 = static_cast<s16>(m_data[13] >> 16);
    const s32 sx2 = static_cast<s16>(m_data[14] & 0xFFFF);
    const s32 sy2 = static_cast<s16>(m_data[14] >> 16);
    m_mac[0] = static_cast<s32>(s64(sx0) * sy1 + s64(sx1) * sy2 + s64(sx2) * sy0
                              - s64(sx0) * sy2 - s64(sx1) * sy0 - s64(sx2) * sy1);
}

void GTE::cmd_avsz3() {
    const s32 zsf3 = static_cast<s16>(m_control[29] & 0xFFFF);
    m_mac[0] = static_cast<s32>(s64(zsf3) * ((m_data[17] & 0xFFFF) + (m_data[18] & 0xFFFF) + (m_data[19] & 0xFFFF)));
    s32 otz = m_mac[0] >> 12;
    if (otz < 0) { otz = 0; set_flag(18); }
    if (otz > 0xFFFF) { otz = 0xFFFF; set_flag(18); }
    m_data[7] = static_cast<u32>(otz);
}

void GTE::cmd_avsz4() {
    const s32 zsf4 = static_cast<s16>(m_control[30] & 0xFFFF);
    m_mac[0] = static_cast<s32>(s64(zsf4) * ((m_data[16] & 0xFFFF) + (m_data[17] & 0xFFFF) + (m_data[18] & 0xFFFF) + (m_data[19] & 0xFFFF)));
    s32 otz = m_mac[0] >> 12;
    if (otz < 0) { otz = 0; set_flag(18); }
    if (otz > 0xFFFF) { otz = 0xFFFF; set_flag(18); }
    m_data[7] = static_cast<u32>(otz);
}

void GTE::cmd_mvmva(u32 instr) {
    const int mx = (instr >> 17) & 3;
    const int vx = (instr >> 15) & 3;
    const int cv = (instr >> 13) & 3;
    const bool lm = ((instr >> 10) & 1u) != 0;
    const bool sf = ((instr >> 19) & 1u) != 0;

    s32 v[3];
    if (vx == 3) { v[0] = m_ir[1]; v[1] = m_ir[2]; v[2] = m_ir[3]; }
    else load_vector(vx, v[0], v[1], v[2]);

    s16 mat[3][3]{};
    if (mx == 0) load_matrix(0, mat);
    else if (mx == 1) load_matrix(8, mat);
    else if (mx == 2) load_matrix(16, mat);
    else {
        const s32 r = static_cast<s32>(m_data[6] & 0xFF) << 4;
        mat[0][0] = static_cast<s16>(-r);
        mat[0][1] = static_cast<s16>(r);
        mat[0][2] = static_cast<s16>(m_ir[0]);
        mat[1][0] = mat[1][1] = mat[1][2] = static_cast<s16>(m_control[1] & 0xFFFF);
        mat[2][0] = mat[2][1] = mat[2][2] = static_cast<s16>(m_control[2] & 0xFFFF);
    }

    s32 t[3] = {0,0,0};
    if (cv == 0) { t[0] = static_cast<s32>(m_control[5]); t[1] = static_cast<s32>(m_control[6]); t[2] = static_cast<s32>(m_control[7]); }
    else if (cv == 1) { t[0] = static_cast<s32>(m_control[13]); t[1] = static_cast<s32>(m_control[14]); t[2] = static_cast<s32>(m_control[15]); }
    else if (cv == 2) {
        // Hardware bug: FC translation is not actually added; the first column
        // is omitted while overflow state behaves as though it was attempted.
        mat[0][0] = 0; mat[1][0] = 0; mat[2][0] = 0;
    }

    for (int row = 0; row < 3; ++row) {
        const s64 raw = s64(t[row]) * 0x1000ll + s64(mat[row][0]) * v[0] + s64(mat[row][1]) * v[1] + s64(mat[row][2]) * v[2];
        m_mac[row + 1] = sar64(raw, sf ? 12 : 0);
        m_ir[row + 1] = static_cast<s16>(saturate_ir(m_mac[row + 1], row + 1, lm));
    }
}

void GTE::cmd_sqr() {
    m_mac[1] = sar64(s64(m_ir[1]) * m_ir[1], m_sf ? 12 : 0);
    m_mac[2] = sar64(s64(m_ir[2]) * m_ir[2], m_sf ? 12 : 0);
    m_mac[3] = sar64(s64(m_ir[3]) * m_ir[3], m_sf ? 12 : 0);
    m_ir[1] = static_cast<s16>(saturate_ir(m_mac[1], 1, true));
    m_ir[2] = static_cast<s16>(saturate_ir(m_mac[2], 2, true));
    m_ir[3] = static_cast<s16>(saturate_ir(m_mac[3], 3, true));
}

void GTE::cmd_op() {
    const s16 d1 = static_cast<s16>(m_control[0] & 0xFFFF);
    const s16 d2 = static_cast<s16>(m_control[2] & 0xFFFF);
    const s16 d3 = static_cast<s16>(m_control[4] & 0xFFFF);
    m_mac[1] = sar64(s64(m_ir[3]) * d2 - s64(m_ir[2]) * d3, m_sf ? 12 : 0);
    m_mac[2] = sar64(s64(m_ir[1]) * d3 - s64(m_ir[3]) * d1, m_sf ? 12 : 0);
    m_mac[3] = sar64(s64(m_ir[2]) * d1 - s64(m_ir[1]) * d2, m_sf ? 12 : 0);
    m_ir[1] = static_cast<s16>(saturate_ir(m_mac[1], 1, m_lm));
    m_ir[2] = static_cast<s16>(saturate_ir(m_mac[2], 2, m_lm));
    m_ir[3] = static_cast<s16>(saturate_ir(m_mac[3], 3, m_lm));
}

void GTE::cmd_ncs() { light_transform(); color_matrix(); push_color_from_mac(m_data[6] >> 24); }
void GTE::cmd_nct() {
    const u32 v[6] = {m_data[0],m_data[1],m_data[2],m_data[3],m_data[4],m_data[5]};
    for (int i=0;i<3;i++) { m_data[0]=v[i*2]; m_data[1]=v[i*2+1]; cmd_ncs(); }
    m_data[0]=v[0]; m_data[1]=v[1]; m_data[2]=v[2]; m_data[3]=v[3]; m_data[4]=v[4]; m_data[5]=v[5];
}
void GTE::cmd_nccs() { light_transform(); color_matrix(); color_apply(); push_color_from_mac(m_data[6] >> 24); }
void GTE::cmd_ncct() {
    const u32 v[6] = {m_data[0],m_data[1],m_data[2],m_data[3],m_data[4],m_data[5]};
    for (int i=0;i<3;i++) { m_data[0]=v[i*2]; m_data[1]=v[i*2+1]; cmd_nccs(); }
    std::copy(std::begin(v), std::end(v), &m_data[0]);
}
void GTE::cmd_ncds() {
    light_transform(); color_matrix();
    const s64 r = (s64(static_cast<s32>(m_data[6] & 0xFF)) * m_ir[1]) << 4;
    const s64 g = (s64(static_cast<s32>((m_data[6] >> 8) & 0xFF)) * m_ir[2]) << 4;
    const s64 b = (s64(static_cast<s32>((m_data[6] >> 16) & 0xFF)) * m_ir[3]) << 4;
    depth_cue_from_mac(r,g,b);
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_ncdt() {
    const u32 v[6] = {m_data[0],m_data[1],m_data[2],m_data[3],m_data[4],m_data[5]};
    for (int i=0;i<3;i++) { m_data[0]=v[i*2]; m_data[1]=v[i*2+1]; cmd_ncds(); }
    std::copy(std::begin(v), std::end(v), &m_data[0]);
}
void GTE::cmd_cc() { color_matrix(); color_apply(); push_color_from_mac(m_data[6] >> 24); }
void GTE::cmd_cdp() {
    color_matrix();
    const s64 r = (s64(static_cast<s32>(m_data[6] & 0xFF)) * m_ir[1]) << 4;
    const s64 g = (s64(static_cast<s32>((m_data[6] >> 8) & 0xFF)) * m_ir[2]) << 4;
    const s64 b = (s64(static_cast<s32>((m_data[6] >> 16) & 0xFF)) * m_ir[3]) << 4;
    depth_cue_from_mac(r,g,b);
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_dcpl() {
    const s64 r = (s64(static_cast<s32>(m_data[6] & 0xFF)) * m_ir[1]) << 4;
    const s64 g = (s64(static_cast<s32>((m_data[6] >> 8) & 0xFF)) * m_ir[2]) << 4;
    const s64 b = (s64(static_cast<s32>((m_data[6] >> 16) & 0xFF)) * m_ir[3]) << 4;
    depth_cue_from_mac(r,g,b);
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_dpcs() {
    const s64 r = s64(static_cast<s32>(m_data[6] & 0xFF)) << 16;
    const s64 g = s64(static_cast<s32>((m_data[6] >> 8) & 0xFF)) << 16;
    const s64 b = s64(static_cast<s32>((m_data[6] >> 16) & 0xFF)) << 16;
    depth_cue_from_mac(r,g,b);
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_dpct() {
    for (int i=0;i<3;i++) {
        const u32 rgb0 = m_data[20];
        const s64 r = s64(rgb0 & 0xFF) << 16;
        const s64 g = s64((rgb0 >> 8) & 0xFF) << 16;
        const s64 b = s64((rgb0 >> 16) & 0xFF) << 16;
        depth_cue_from_mac(r,g,b);
        push_color_from_mac(m_data[6] >> 24);
    }
}
void GTE::cmd_intpl() {
    const s64 r = s64(m_ir[1]) << 12;
    const s64 g = s64(m_ir[2]) << 12;
    const s64 b = s64(m_ir[3]) << 12;
    depth_cue_from_mac(r,g,b);
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_gpf() {
    m_mac[1] = sar64(s64(m_ir[0]) * m_ir[1], m_sf ? 12 : 0);
    m_mac[2] = sar64(s64(m_ir[0]) * m_ir[2], m_sf ? 12 : 0);
    m_mac[3] = sar64(s64(m_ir[0]) * m_ir[3], m_sf ? 12 : 0);
    m_ir[1] = static_cast<s16>(saturate_ir(m_mac[1],1,m_lm));
    m_ir[2] = static_cast<s16>(saturate_ir(m_mac[2],2,m_lm));
    m_ir[3] = static_cast<s16>(saturate_ir(m_mac[3],3,m_lm));
    push_color_from_mac(m_data[6] >> 24);
}
void GTE::cmd_gpl() {
    const s64 base1 = m_sf ? (s64(m_mac[1]) << 12) : s64(m_mac[1]);
    const s64 base2 = m_sf ? (s64(m_mac[2]) << 12) : s64(m_mac[2]);
    const s64 base3 = m_sf ? (s64(m_mac[3]) << 12) : s64(m_mac[3]);
    m_mac[1] = sar64(base1 + s64(m_ir[0])*m_ir[1], m_sf ? 12 : 0);
    m_mac[2] = sar64(base2 + s64(m_ir[0])*m_ir[2], m_sf ? 12 : 0);
    m_mac[3] = sar64(base3 + s64(m_ir[0])*m_ir[3], m_sf ? 12 : 0);
    m_ir[1] = static_cast<s16>(saturate_ir(m_mac[1],1,m_lm));
    m_ir[2] = static_cast<s16>(saturate_ir(m_mac[2],2,m_lm));
    m_ir[3] = static_cast<s16>(saturate_ir(m_mac[3],3,m_lm));
    push_color_from_mac(m_data[6] >> 24);
}

} // namespace ps96
