#pragma once

#include "common/Types.hpp"
#include <array>
#include <deque>
#include <vector>

namespace ps96 {

// PlayStation 1 Motion Decoder.  The implementation intentionally keeps the
// device synchronous: DMA feeds the command/parameter FIFO and once a complete
// command is present the decoder produces the corresponding output FIFO.  The
// CPU-visible protocol and data format are the same as the hardware; timing is
// left to the surrounding DMA scheduler.
class MDEC {
public:
    MDEC();
    void reset();

    u32 read_data();
    u32 read_status();
    void write_data(u32 value);
    void write_control(u32 value);

    // DMA request lines exposed by MDEC1 status. DMA0 (input) may proceed
    // when the input FIFO has room; DMA1 (output) may proceed when decoded
    // words are available.
    bool dma_in_request() const;
    bool dma_out_request() const;

private:
    std::deque<u16> m_in_fifo;
    std::deque<u32> m_out_fifo;

    u32 m_status = 0x80040000;
    u32 m_cmd = 0;
    bool m_have_cmd = false;
    bool m_busy = false;
    bool m_dma_in_enable = false;
    bool m_dma_out_enable = false;
    u32 m_remaining_halfwords = 0;
    u8 m_output_depth = 3; // 3 = 15-bit, the normal PS1 video mode
    bool m_output_signed = false;
    bool m_output_bit15 = false;

    std::array<u8, 64> m_iq_y{};
    std::array<u8, 64> m_iq_uv{};
    std::array<s16, 64> m_scale{};

    std::array<s16, 64> m_blocks[6]{}; // Cr, Cb, Y1, Y2, Y3, Y4
    std::array<u32, 256> m_rgb{};      // RGB888-ish bytes, one pixel/entry

    void process();
    bool execute_command();
    bool decode_macroblock();
    bool decode_block(std::array<s16, 64>& block, const std::array<u8, 64>& qt);
    void idct(std::array<s16, 64>& block);
    void yuv_to_rgb(int ox, int oy, const std::array<s16, 64>& cr,
                    const std::array<s16, 64>& cb, const std::array<s16, 64>& y);
    void y_to_mono(const std::array<s16, 64>& y);
    void emit_output();

    static s32 sign_extend10(u16 v);
    static s32 clamp_s32(s32 v, s32 lo, s32 hi);
};

} // namespace ps96
