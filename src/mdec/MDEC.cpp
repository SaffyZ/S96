#include "mdec/MDEC.hpp"
#include "common/Log.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps96 {

namespace {
constexpr std::array<u8, 64> kZagzig = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

// PSX MDEC default scale matrix. Values are 14-bit fixed point and are
// deliberately kept in the exact signed 16-bit representation used by the
// chip. This is also the matrix normally installed by DecDCTReset().
constexpr std::array<s16, 64> kDefaultScale = {
    s16(0x5A82),s16(0x5A82),s16(0x5A82),s16(0x5A82),s16(0x5A82),s16(0x5A82),s16(0x5A82),s16(0x5A82),
    s16(0x7D8A),s16(0x6A6D),s16(0x471C),s16(0x18F8),s16(0xE707),s16(0xB8E3),s16(0x9592),s16(0x8275),
    s16(0x7641),s16(0x30FB),s16(0xCF04),s16(0x89BE),s16(0x89BE),s16(0xCF04),s16(0x30FB),s16(0x7641),
    s16(0x6A6D),s16(0xE707),s16(0x8275),s16(0xB8E3),s16(0x471C),s16(0x7D8A),s16(0x18F8),s16(0x9592),
    s16(0x5A82),s16(0xA57D),s16(0xA57D),s16(0x5A82),s16(0x5A82),s16(0xA57D),s16(0xA57D),s16(0x5A82),
    s16(0x471C),s16(0x8275),s16(0x18F8),s16(0x6A6D),s16(0x9592),s16(0xE707),s16(0x7D8A),s16(0xB8E3),
    s16(0x30FB),s16(0x89BE),s16(0x7641),s16(0xCF04),s16(0xCF04),s16(0x7641),s16(0x89BE),s16(0x30FB),
    s16(0x18F8),s16(0xB8E3),s16(0x6A6D),s16(0x8275),s16(0x7D8A),s16(0x9592),s16(0x471C),s16(0xE707)
};

constexpr std::array<u8, 64> kDefaultIq = {
     2,16,19,22,26,27,29,34,
    16,16,22,24,27,29,34,37,
    19,22,26,27,29,34,34,38,
    22,22,26,27,29,34,37,40,
    22,26,27,29,32,35,40,48,
    26,27,29,32,35,40,48,58,
    26,27,29,34,38,46,56,69,
    27,29,35,38,46,56,69,83
};

inline u8 byte_at(u32 v, int i) { return static_cast<u8>((v >> (i * 8)) & 0xFF); }
}

MDEC::MDEC() { reset(); }

void MDEC::reset() {
    m_in_fifo.clear();
    m_out_fifo.clear();
    m_cmd = 0;
    m_have_cmd = false;
    m_busy = false;
    m_dma_in_enable = false;
    m_dma_out_enable = false;
    m_remaining_halfwords = 0;
    m_output_depth = 3;
    m_output_signed = false;
    m_output_bit15 = false;
    m_iq_y = kDefaultIq;
    m_iq_uv = kDefaultIq;
    m_scale = kDefaultScale;
    for (auto& b : m_blocks) b.fill(0);
    m_rgb.fill(0);
    m_status = 0x80040000;
}

s32 MDEC::sign_extend10(u16 v) {
    s32 x = static_cast<s32>(v & 0x03FF);
    if (x & 0x0200) x -= 0x0400;
    return x;
}

s32 MDEC::clamp_s32(s32 v, s32 lo, s32 hi) {
    return std::min(hi, std::max(lo, v));
}

u32 MDEC::read_data() {
    if (m_out_fifo.empty()) return 0;
    u32 v = m_out_fifo.front();
    m_out_fifo.pop_front();
    process();
    return v;
}

bool MDEC::dma_in_request() const {
    return m_dma_in_enable && m_in_fifo.size() < 0x200;
}

bool MDEC::dma_out_request() const {
    return m_dma_out_enable && !m_out_fifo.empty();
}

u32 MDEC::read_status() {
    u32 s = 0;
    if (m_out_fifo.empty()) s |= 1u << 31;
    if (m_in_fifo.size() >= 0x200) s |= 1u << 30;
    if (m_busy || m_have_cmd) s |= 1u << 29;
    if (dma_in_request()) s |= 1u << 28;
    if (dma_out_request()) s |= 1u << 27;
    s |= (u32(m_output_depth & 3) << 25);
    if (m_output_signed) s |= 1u << 24;
    if (m_output_bit15) s |= 1u << 23;
    // The status register reports remaining *32-bit parameter words* minus
    // one. Internally we keep the FIFO count in 16-bit halfwords.
    s |= (m_have_cmd && m_remaining_halfwords != 0)
        ? (((m_remaining_halfwords / 2) - 1) & 0xFFFFu) : 0xFFFFu;
    return s;
}

void MDEC::write_data(u32 value) {
    // The register is a 32-bit port, but MDEC parameters are consumed as
    // halfwords (or bytes for the quantization-table command).
    m_in_fifo.push_back(static_cast<u16>(value & 0xFFFF));
    m_in_fifo.push_back(static_cast<u16>(value >> 16));
    process();
}

void MDEC::write_control(u32 value) {
    if (value & 0x80000000u) {
        reset();
        // Reset does not disable the DMA request enables on real software paths
        // that immediately reprogram the control register, so use the written
        // enable bits after reset.
    }
    m_dma_in_enable = (value & 0x40000000u) != 0;
    m_dma_out_enable = (value & 0x20000000u) != 0;
    process();
}

void MDEC::process() {
    if (m_busy) return;
    m_busy = true;

    // Iterate rather than recursively calling process() for every decoded
    // macroblock.  Long FMV commands contain thousands of parameter words;
    // recursive chaining would eventually exhaust the host call stack.
    for (;;) {
        // A new command starts with the next 32-bit write, represented here by
        // two halfwords. The command itself is not part of the parameter count.
        if (!m_have_cmd) {
        if (m_in_fifo.size() < 2) break;
        u16 lo = m_in_fifo.front(); m_in_fifo.pop_front();
        u16 hi = m_in_fifo.front(); m_in_fifo.pop_front();
        m_cmd = u32(lo) | (u32(hi) << 16);
        m_have_cmd = true;
        // Preserve already-produced output words. DMA1 may still be draining
        // the previous macroblock while the next command header arrives.
        m_output_depth = static_cast<u8>((m_cmd >> 27) & 3);
        m_output_signed = (m_cmd & (1u << 26)) != 0;
        m_output_bit15 = (m_cmd & (1u << 25)) != 0;

        const u32 op = (m_cmd >> 29) & 7;
        {
            static int s_cmd_log = 0;
            if (s_cmd_log < 20) {
                Log::info("MDEC cmd op=%u words=%u depth=%u signed=%d bit15=%d",
                    op, m_cmd & 0xFFFFu, m_output_depth, m_output_signed ? 1 : 0,
                    m_output_bit15 ? 1 : 0);
                ++s_cmd_log;
            }
        }
        // Decode command parameter count is in 32-bit words, while the MDEC
        // input FIFO is consumed as 16-bit halfwords. The old implementation
        // forgot this conversion, so every video stream was truncated to half
        // its compressed input and the next macroblock started in garbage.
        if (op == 1) m_remaining_halfwords = (m_cmd & 0xFFFFu) * 2u;
        else if (op == 2) m_remaining_halfwords = (m_cmd & 1) ? 64 : 32; // bytes
        else if (op == 3) m_remaining_halfwords = 64;
        else m_remaining_halfwords = 0;

            if (m_remaining_halfwords == 0 && op != 1 && op != 2 && op != 3) {
                m_have_cmd = false;
                continue;
            }
        }

        const bool done = execute_command();
        if (!done)
            break;

        // Decode commands may contain many macroblocks. Keep the command alive
        // until its parameter-halfword count reaches zero.
        if (((m_cmd >> 29) & 7) == 1 && m_remaining_halfwords != 0)
            continue;

        m_have_cmd = false;
        m_remaining_halfwords = 0;
    }

    // process() is synchronous in this emulator, but the latch must represent
    // the end of this invocation. Leaving it set after the input FIFO becomes
    // empty deadlocks every later MDEC command and makes FMV frames go black.
    m_busy = false;
}

bool MDEC::execute_command() {
    const u32 op = (m_cmd >> 29) & 7;
    if (op == 0 || op >= 4) {
        m_in_fifo.clear();
        return true;
    }

    if (op == 2) {
        const size_t need_bytes = (m_cmd & 1) ? 128 : 64;
        const size_t need_halfwords = need_bytes / 2;
        if (m_in_fifo.size() < need_halfwords) return false;
        std::array<u8, 128> bytes{};
        for (size_t i = 0; i < need_halfwords; ++i) {
            const u16 h = m_in_fifo.front();
            m_in_fifo.pop_front();
            bytes[i * 2 + 0] = static_cast<u8>(h & 0xFF);
            bytes[i * 2 + 1] = static_cast<u8>(h >> 8);
        }
        std::copy_n(bytes.begin(), 64, m_iq_y.begin());
        if (need_bytes == 128)
            std::copy_n(bytes.begin() + 64, 64, m_iq_uv.begin());
        else
            m_iq_uv = m_iq_y;
        return true;
    }

    if (op == 3) {
        if (m_in_fifo.size() < 64) return false;
        for (size_t i = 0; i < 64; ++i) {
            m_scale[i] = static_cast<s16>(m_in_fifo.front());
            m_in_fifo.pop_front();
        }
        return true;
    }

    // Decode commands can be much larger than the 512-halfword input FIFO.
    // Decode one macroblock whenever enough input is present and leave the
    // command alive for subsequent DMA writes. decode_macroblock() is
    // transactional, so a partial block never consumes half a stream and then
    // loses its place.
    return decode_macroblock();
}

bool MDEC::decode_block(std::array<s16, 64>& block, const std::array<u8, 64>& qt) {
    block.fill(0);
    // FE00 at the beginning of a block is padding and is ignored.
    u16 n = 0;
    do {
        if (m_in_fifo.empty() || m_remaining_halfwords == 0) return false;
        n = m_in_fifo.front(); m_in_fifo.pop_front();
        --m_remaining_halfwords;
    } while (n == 0xFE00);

    const u32 qscale = (n >> 10) & 0x3F;
    int k = 0;
    s32 val = sign_extend10(n) * qt[0];
    if (qscale == 0) val = sign_extend10(n) * 2;
    val = clamp_s32(val, -0x400, 0x3FF);
    block[qscale ? kZagzig[0] : 0] = static_cast<s16>(val);

    while (k < 63) {
        if (m_in_fifo.empty() || m_remaining_halfwords == 0) return false;
        n = m_in_fifo.front(); m_in_fifo.pop_front();
        --m_remaining_halfwords;
        if (n == 0xFE00) break;
        k += ((n >> 10) & 0x3F) + 1;
        if (k >= 64) break;
        val = sign_extend10(n);
        if (qscale == 0) val *= 2;
        else val = (val * qt[k] * static_cast<s32>(qscale) + 4) / 8;
        val = clamp_s32(val, -0x400, 0x3FF);
        block[qscale ? kZagzig[k] : k] = static_cast<s16>(val);
    }
    return true;
}

void MDEC::idct(std::array<s16, 64>& block) {
    std::array<s64, 64> tmp{};
    // This is the documented 2-pass MDEC matrix multiply. It uses the actual
    // programmable scale table rather than a host JPEG implementation.
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            s64 sum = 0;
            for (int z = 0; z < 8; ++z)
                sum += s64(block[z * 8 + x]) * s64(m_scale[y * 8 + z]);
            tmp[x + y * 8] = sum;
        }
    }
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            s64 sum = 0;
            for (int z = 0; z < 8; ++z)
                sum += tmp[z + y * 8] * s64(m_scale[x * 8 + z]);
            s64 rounded = (sum >> 32) + ((sum >> 31) & 1);
            block[x + y * 8] = static_cast<s16>(clamp_s32(static_cast<s32>(rounded), -128, 127));
        }
    }
}

void MDEC::yuv_to_rgb(int ox, int oy, const std::array<s16, 64>& cr,
                      const std::array<s16, 64>& cb, const std::array<s16, 64>& y) {
    const int add = m_output_signed ? 0 : 128;
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            const int ci = ((px + ox) / 2) + ((py + oy) / 2) * 8;
            int r = static_cast<int>(std::lround(1.402 * cr[ci]));
            int g = static_cast<int>(std::lround(-0.3437 * cb[ci] - 0.7143 * cr[ci]));
            int b = static_cast<int>(std::lround(1.772 * cb[ci]));
            const int yy = y[px + py * 8];
            r = clamp_s32(yy + r, -128, 127) + add;
            g = clamp_s32(yy + g, -128, 127) + add;
            b = clamp_s32(yy + b, -128, 127) + add;
            m_rgb[(px + ox) + (py + oy) * 16] =
                u32(static_cast<u8>(clamp_s32(r, 0, 255))) |
                (u32(static_cast<u8>(clamp_s32(g, 0, 255))) << 8) |
                (u32(static_cast<u8>(clamp_s32(b, 0, 255))) << 16);
        }
    }
}

void MDEC::y_to_mono(const std::array<s16, 64>& y) {
    for (int i = 0; i < 64; ++i) {
        int v = y[i] & 0x1FF;
        if (v & 0x100) v -= 0x200;
        v = clamp_s32(v, -128, 127);
        if (!m_output_signed) v += 128;
        m_rgb[i] = static_cast<u8>(clamp_s32(v, 0, 255));
    }
}

bool MDEC::decode_macroblock() {
    if (m_remaining_halfwords == 0) return true;
    // Hardware has a finite output FIFO. Do not decode more macroblocks while
    // it is full; read_data()/DMA will call process() again as space appears.
    if (m_out_fifo.size() >= 768) return false;

    // The DMA producer may feed the compressed stream in chunks smaller than
    // one macroblock. Keep this attempt atomic: if a block runs out of input,
    // restore the FIFO/counter/block state and wait for the next DMA write.
    const auto fifo_before = m_in_fifo;
    const u32 remaining_before = m_remaining_halfwords;
    std::array<std::array<s16, 64>, 6> blocks_before{};
    for (size_t i = 0; i < 6; ++i) blocks_before[i] = m_blocks[i];
    const auto rgb_before = m_rgb;

    const bool mono = m_output_depth <= 1;
    if (mono) {
        if (!decode_block(m_blocks[2], m_iq_y)) {
            m_in_fifo = fifo_before; m_remaining_halfwords = remaining_before;
            for (size_t i = 0; i < 6; ++i) m_blocks[i] = blocks_before[i];
            m_rgb = rgb_before;
            return false;
        }
        idct(m_blocks[2]);
        y_to_mono(m_blocks[2]);
        emit_output();
        return true;
    }

    if (!decode_block(m_blocks[0], m_iq_uv)) {
        m_in_fifo = fifo_before; m_remaining_halfwords = remaining_before;
        for (size_t i = 0; i < 6; ++i) m_blocks[i] = blocks_before[i];
        m_rgb = rgb_before;
        return false;
    } // Cr
    if (!decode_block(m_blocks[1], m_iq_uv)) {
        m_in_fifo = fifo_before; m_remaining_halfwords = remaining_before;
        for (size_t i = 0; i < 6; ++i) m_blocks[i] = blocks_before[i];
        m_rgb = rgb_before;
        return false;
    } // Cb
    for (int i = 2; i < 6; ++i) {
        if (!decode_block(m_blocks[i], m_iq_y)) {
            m_in_fifo = fifo_before; m_remaining_halfwords = remaining_before;
            for (size_t i = 0; i < 6; ++i) m_blocks[i] = blocks_before[i];
            m_rgb = rgb_before;
            return false;
        }
        idct(m_blocks[i]);
    }
    idct(m_blocks[0]);
    idct(m_blocks[1]);
    m_rgb.fill(0);
    yuv_to_rgb(0, 0, m_blocks[0], m_blocks[1], m_blocks[2]);
    yuv_to_rgb(8, 0, m_blocks[0], m_blocks[1], m_blocks[3]);
    yuv_to_rgb(0, 8, m_blocks[0], m_blocks[1], m_blocks[4]);
    yuv_to_rgb(8, 8, m_blocks[0], m_blocks[1], m_blocks[5]);
    emit_output();
    {
        static int s_block_log = 0;
        if (s_block_log < 12) {
            Log::info("MDEC decoded macroblock depth=%u remaining_hw=%u out_words=%zu",
                m_output_depth, m_remaining_halfwords, m_out_fifo.size());
            ++s_block_log;
        }
    }
    return true;
}

void MDEC::emit_output() {
    if (m_output_depth == 3) {
        const u32 a = m_output_bit15 ? 0x8000u : 0;
        for (int i = 0; i < 256; i += 2) {
            auto pack = [&](int p) -> u16 {
                const u32 c = m_rgb[p];
                const u16 r = static_cast<u16>(std::min(31u, ((c & 0xFFu) + 4u) >> 3));
                const u16 g = static_cast<u16>(std::min(31u, (((c >> 8) & 0xFFu) + 4u) >> 3));
                const u16 b = static_cast<u16>(std::min(31u, (((c >> 16) & 0xFFu) + 4u) >> 3));
                return static_cast<u16>(r | (g << 5) | (b << 10) | a);
            };
            m_out_fifo.push_back(u32(pack(i)) | (u32(pack(i + 1)) << 16));
        }
    } else if (m_output_depth == 2) {
        // 24bpp is tightly packed RGB bytes, three bytes per pixel.
        u32 word = 0;
        int count = 0;
        for (int i = 0; i < 256; ++i) {
            const u32 c = m_rgb[i];
            for (int s = 0; s < 3; ++s) {
                word |= ((c >> (s * 8)) & 0xFFu) << (count * 8);
                if (++count == 4) {
                    m_out_fifo.push_back(word);
                    word = 0;
                    count = 0;
                }
            }
        }
        if (count) m_out_fifo.push_back(word);
    } else if (m_output_depth == 1) {
        for (int i = 0; i < 64; i += 4)
            m_out_fifo.push_back(m_rgb[i] | (m_rgb[i+1] << 8) | (m_rgb[i+2] << 16) | (m_rgb[i+3] << 24));
    } else {
        for (int i = 0; i < 64; i += 8) {
            u32 w = 0;
            for (int n = 0; n < 8; ++n) w |= ((m_rgb[i+n] >> 4) & 0xF) << (n * 4);
            m_out_fifo.push_back(w);
        }
    }
}

} // namespace ps96
