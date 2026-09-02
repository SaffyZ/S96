#pragma once

#include "common/Types.hpp"
#include <array>

namespace ps96 {

class GTE {
public:
    GTE();
    void reset();

    void execute(u32 instr);
    u32 read_data(u32 reg) const;
    void write_data(u32 reg, u32 value);
    u32 read_control(u32 reg) const;
    void write_control(u32 reg, u32 value);

private:
    std::array<u32, 32> m_data{};
    std::array<u32, 32> m_control{};
    s32 m_mac[4]{};
    s16 m_ir[4]{};
    u32 m_flag = 0;
    u32 m_lzcr = 0;
    bool m_sf = true;
    bool m_lm = true;

    void set_flag(u32 bit);
    s32 saturate_ir(s32 v, int i, bool lm);
    s32 saturate_rgb(s32 v);
    void sync_results();
    void load_matrix(int base, s16 m[3][3]) const;
    void push_color_from_mac(u32 code);
    void light_transform();
    void color_matrix();
    void color_apply();
    void depth_cue_from_mac(s64 base1, s64 base2, s64 base3);
    static u32 gte_divide(u32 h, u32 sz, bool& overflow);

    void cmd_rtpt(u32 instr);
    void cmd_rtps(u32 instr);
    void cmd_nclip();
    void cmd_avsz3();
    void cmd_avsz4();
    void cmd_mvmva(u32 instr);
    void cmd_ncds();
    void cmd_ncdt();
    void cmd_nccs();
    void cmd_ncct();
    void cmd_cdp();
    void cmd_cc();
    void cmd_ncs();
    void cmd_nct();
    void cmd_sqr();
    void cmd_dcpl();
    void cmd_intpl();
    void cmd_gpf();
    void cmd_gpl();
    void cmd_op();
    void cmd_dpcs();
    void cmd_dpct();

    void load_vector(int idx, s32& x, s32& y, s32& z);
    void store_screen(s32 x, s32 y, s32 z);
};

} // namespace ps96
