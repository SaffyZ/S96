#include "gpu/GPU.hpp"
#include "gpu/Renderer.hpp"
#include "common/Log.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace ps96 {

GPU::GPU() {
    reset();
}

void GPU::reset() {
    m_polyline = false;
    m_vram.fill(0);
    m_cmd_fifo.clear();
    m_cmd_remaining = 0;
    m_current_cmd = 0;
    m_drawing_x1 = 0; m_drawing_y1 = 0;
    m_drawing_x2 = 1023; m_drawing_y2 = 511;
    m_offset_x = 0; m_offset_y = 0;
    m_texture_page = 0;
    m_texture_page_x = 0;
    m_texture_page_y = 0;
    m_texture_depth = 0;
    m_semi_transparency = 0;
    m_dither = false;
    m_draw_to_display = false;
    m_texture_page_y_base2 = false;
    m_texture_rect_x_flip = false;
    m_texture_rect_y_flip = false;
    m_mask_test = false;
    m_mask_set = false;
    m_tex_window_mask_x = 0;
    m_tex_window_mask_y = 0;
    m_tex_window_offset_x = 0;
    m_tex_window_offset_y = 0;
    m_texture_window = 0;
    m_display_enable = false;
    m_display_x = 0;
    m_display_y = 0;
    m_display_depth_24 = false;
    m_vertical_interlace = false;
    m_video_mode = 0;
    m_reverse_flag = false;
    m_dma_direction = 0;
    m_transfer = TransferMode::Idle;
    m_ready_cmd = true;
    m_ready_vram = true;
    m_ready_dma = true;
    m_busy_cycles = 0;
    m_irq = false;
    m_in_vblank = false;
    m_scanline = 0;
    m_interlace_field = false;
    m_interlaced_display_field = false;
    m_active_line_lsb = 0;
    m_cycle_counter = 0;
    m_hres = 1; // 320
    m_vres = 0;
    m_display_x1 = 0x200;
    m_display_x2 = 0xC00;
    m_display_y1 = 0x10;
    m_display_y2 = 0x100;
    m_gpuread_latch = 0;
    m_debug_cmd_count = 0;
    m_debug_last_cmd = 0;
    m_debug_textured_pixels_skipped = 0;
    m_debug_draw_primitives = 0;
    m_debug_textured_primitives = 0;
    m_debug_unknown_commands = 0;
    m_debug_cmd_hist.fill(0);
}

u32 GPU::read_gpuread() {
    if (m_transfer == TransferMode::VRAMToCPU) {
        auto take = [&]() -> u16 {
            if (m_transfer_pos >= m_transfer_w * m_transfer_h) return 0;
            int x = m_transfer_x + (m_transfer_pos % m_transfer_w);
            int y = m_transfer_y + (m_transfer_pos / m_transfer_w);
            u16 p = read_vram(x, y);
            m_transfer_pos++;
            return p;
        };
        u16 lo = take();
        u16 hi = take();
        if (m_transfer_pos >= m_transfer_w * m_transfer_h) {
            m_transfer = TransferMode::Idle;
            m_ready_cmd = m_ready_vram = m_ready_dma = true;
            static int s_v2c = 0;
            if (s_v2c < 4) { /* log silenced */ s_v2c++; }
        }
        return static_cast<u32>(lo) | (static_cast<u32>(hi) << 16);
    }
    return m_gpuread_latch;
}

u32 GPU::read_gpustat() {
    u32 stat = 0;
    stat |= (m_texture_page & 0x7FF);
    stat |= (m_dither ? 1u : 0u) << 9;
    stat |= (m_draw_to_display ? 1u : 0u) << 10;
    stat |= (m_texture_page_y_base2 ? 1u : 0u) << 15;
    stat |= (m_mask_set ? 1u : 0u) << 11;
    stat |= (m_mask_test ? 1u : 0u) << 12;
    // Bit13 Interlace Field (nocash): when GP1(08h).5 vertical-interlace is
    // OFF this bit is always 1. When interlace is ON it follows the current
    // field. BIOS logo code compares GPUSTAT against masks that expect bit13=1
    // in progressive 240-line mode (see v1=1C4E220A vs observed 1C4E020A).
    if (m_vertical_interlace)
        stat |= (m_interlace_field ? 1u : 0u) << 13;
    else
        stat |= 1u << 13;
    stat |= (m_reverse_flag ? 1u : 0u) << 14;
    if (m_hres == 4) stat |= 1u << 16;
    stat |= (static_cast<u32>(m_hres & 3)) << 17;
    if (m_vres) stat |= 1u << 19;
    if (m_video_mode) stat |= 1u << 20;
    if (m_display_depth_24) stat |= 1u << 21;
    if (m_vertical_interlace) stat |= 1u << 22;
    if (!m_display_enable) stat |= 1u << 23;

    // GPUSTAT ready bits describe interface/FIFO availability, not whether
    // the internal raster engine is still drawing the previous primitive. The
    // previous S96 implementation cleared bit26/28 for every draw and then
    // imposed a fixed busy window, forcing games to spin hundreds of cycles per
    // primitive. That is dramatically slower than the real GPU FIFO and is a
    // major source of the slideshow behaviour seen in complex 3D scenes.
    // Commands are executed synchronously by S96, so the frontend can safely
    // keep the command/DMA receive bits ready except while a VRAM transfer is
    // actually occupying the interface.
    if (m_cmd_remaining == 0 && !m_polyline && m_transfer == TransferMode::Idle)
        stat |= 1u << 26;
    if (m_ready_vram && m_transfer != TransferMode::CPUToVRAM)
        stat |= 1u << 27;
    if (m_ready_dma)
        stat |= 1u << 28;

    // Bit25 DMA request mirrors relevant ready bit (nocash)
    u32 dir = m_dma_direction & 3;
    if (dir == 2 && (stat & (1u << 28)))
        stat |= 1u << 25;
    else if (dir == 3 && (stat & (1u << 27)))
        stat |= 1u << 25;
    else if (dir == 1 && (stat & (1u << 28)))
        stat |= 1u << 25;

    stat |= dir << 29;
    if (m_irq) stat |= 1u << 24;

    // Bit31 reports the currently displayed VRAM line parity. For 480-line
    // interlace the field selected for scanout is a separate CRTC state from
    // the field the renderer is currently drawing, so do not derive this from
    // the live scanline. This matches the field model used by DuckStation.
    bool displayed_odd = false;
    if (m_vertical_interlace && m_vres) {
        displayed_odd = ((m_display_y + (m_in_vblank ? 0 : (m_interlaced_display_field ? 1 : 0))) & 1) != 0;
    } else if (m_vertical_interlace) {
        displayed_odd = ((m_display_y + m_scanline) & 1) != 0;
    } else if (!m_in_vblank) {
        displayed_odd = ((m_display_y + m_scanline) & 1) != 0;
    } else {
        displayed_odd = (m_display_y & 1) != 0;
    }
    if (displayed_odd)
        stat |= 1u << 31;
    return stat;
}

void GPU::write_gp0(u32 value) {
    // CPU → VRAM: each 32-bit word carries TWO 16-bit pixels (low then high)
    if (m_transfer == TransferMode::CPUToVRAM) {
        auto put = [&](u16 pix) {
            if (m_transfer_pos >= m_transfer_w * m_transfer_h) return;
            int x = m_transfer_x + (m_transfer_pos % m_transfer_w);
            int y = m_transfer_y + (m_transfer_pos / m_transfer_w);
            u16 old = read_vram(x, y);
            // E6 mask rules apply to CPU->VRAM transfers too.
            if (!m_mask_test || !(old & 0x8000u)) {
                if (m_mask_set) pix |= 0x8000u;
                write_vram(x, y, pix);
            }
            m_transfer_pos++;
        };
        put(static_cast<u16>(value & 0xFFFF));
        put(static_cast<u16>(value >> 16));
        if (m_transfer_pos >= m_transfer_w * m_transfer_h) {
            m_transfer = TransferMode::Idle;
            m_ready_cmd = true;
            m_ready_vram = true;
            m_ready_dma = true;
            static int s_xfer_log = 0;
            if (s_xfer_log < 0) {
                Log::info("GPU: CPU->VRAM complete %d pixels", m_transfer_pos);
                s_xfer_log++;
            }
        }
        return;
    }

    if (m_cmd_remaining == 0) {
        // New command aborts an unfinished VRAM->CPU stream
        if (m_transfer == TransferMode::VRAMToCPU) {
            m_transfer = TransferMode::Idle;
            m_ready_cmd = m_ready_vram = m_ready_dma = true;
        }
        m_current_cmd = value >> 24;
        m_cmd_fifo.clear();
        m_cmd_fifo.push_back(value);
        // Determine argument count (low bits of cmd are flags — match by range)
        {
            u8 c = static_cast<u8>(m_current_cmd);
            auto in = [&](u8 a, u8 b) { return c >= a && c <= b; };
            m_polyline = false;
            if (c == 0x00 || c == 0x01) m_cmd_remaining = 0;
            else if (c == 0x02) m_cmd_remaining = 2;
            else if (in(0x20, 0x23)) m_cmd_remaining = 3;
            else if (in(0x24, 0x27)) m_cmd_remaining = 6;
            else if (in(0x28, 0x2B)) m_cmd_remaining = 4;
            else if (in(0x2C, 0x2F)) m_cmd_remaining = 8;
            else if (in(0x30, 0x33)) m_cmd_remaining = 5;
            else if (in(0x34, 0x37)) m_cmd_remaining = 8;
            else if (in(0x38, 0x3B)) m_cmd_remaining = 7;
            else if (in(0x3C, 0x3F)) m_cmd_remaining = 11;
            // Lines: 0x40-47 mono two-point (3 words); 0x48-4F mono polyline (var)
            else if (in(0x40, 0x47)) m_cmd_remaining = 2;
            else if (in(0x48, 0x4F)) { m_polyline = true; m_cmd_remaining = 0x7FFFFFFF; }
            // Shaded lines: 0x50-57 two-point (4 words); 0x58-5F shaded polyline (var)
            else if (in(0x50, 0x57)) m_cmd_remaining = 3;
            else if (in(0x58, 0x5F)) { m_polyline = true; m_cmd_remaining = 0x7FFFFFFF; }
            else if (in(0x60, 0x63)) m_cmd_remaining = 2;
            else if (in(0x64, 0x67)) m_cmd_remaining = 3;
            else if (in(0x68, 0x6B)) m_cmd_remaining = 1;
            else if (in(0x6C, 0x6F)) m_cmd_remaining = 2;
            else if (in(0x70, 0x73)) m_cmd_remaining = 1;
            else if (in(0x74, 0x77)) m_cmd_remaining = 2;
            else if (in(0x78, 0x7B)) m_cmd_remaining = 1;
            else if (in(0x7C, 0x7F)) m_cmd_remaining = 2;
            else if (c == 0x80) m_cmd_remaining = 3;
            else if (c == 0xA0) m_cmd_remaining = 2;
            else if (c == 0xC0) m_cmd_remaining = 2;
            else if (in(0xE0, 0xE8)) m_cmd_remaining = 0;
            else {
                // Log full FIFO context for the FIRST unknown only — this is the
                // desync smoking gun. Subsequent unknowns are cascade noise.
                static int s_unk = 0;
                m_debug_unknown_commands++;
                if (s_unk < 3) {
                    Log::info("GPU: FIRST_UNKNOWN cmd=%02X word=%08X fifo_n=%d prev_cmd=%02X poly=%d",
                        c, value, (int)m_cmd_fifo.size(), m_current_cmd, (int)m_polyline);
                    s_unk++;
                }
                m_cmd_remaining = 0;
            }
        }
        // The command FIFO remains writable while the internal raster engine
        // is working. Only an unfinished packet is not ready for a *new* GP0
        // command, and the status logic above exposes that state.
        m_ready_cmd = true;
        const u8 accepted_cmd = static_cast<u8>(m_current_cmd);
        (void)accepted_cmd;
        if (m_cmd_remaining == 0 && !m_polyline) {
            process_gp0_command();
        }
    } else {
        m_cmd_fifo.push_back(value);
        // Polyline packets (0x48-4F / 0x58-5F) continue until the 0x55555555
        // terminator word.  Treating them as fixed-length was the primary
        // FIFO desync: trailing vertices/colors were parsed as new GP0 opcodes
        // (the "unknown GP0 BB/CB" cascade).
        if (m_polyline) {
            // Polyline terminator is a pattern, not one literal value:
            // bits 12..15 and 28..31 must both equal 5. Real games commonly
            // emit 0x50005000, while 0x55555555 is only one possible value.
            if ((value & 0xF000F000u) == 0x50005000u) {
                m_cmd_remaining = 0;
                m_polyline = false;
                process_gp0_command();
            }
            // else keep accumulating; remaining stays large
        } else {
            m_cmd_remaining--;
            if (m_cmd_remaining == 0) {
                process_gp0_command();
            }
        }
    }
}

void GPU::write_gp1(u32 value) {
    exec_gp1(value);
}

void GPU::dma_write(u32 value) {
    write_gp0(value);
}

u32 GPU::dma_read() {
    return read_gpuread();
}

bool GPU::dma_ready() const {
    return m_ready_dma;
}

void GPU::process_gp0_command() {
    if (m_cmd_fifo.empty()) return;
    u32 cmd = m_cmd_fifo[0] >> 24;
    exec_gp0(m_cmd_fifo[0]);
    m_cmd_fifo.clear();
    m_polyline = false;
    m_cmd_remaining = 0;
    // Pulse busy so software that waits for !ready then ready can proceed.
    // Draw/fill commands take longer; setup commands are brief.
    if (cmd >= 0x02 && cmd <= 0x7F) {
        // Keep a small internal completion time for diagnostics/timing state,
        // but do NOT clear GPUSTAT.26/28. The real GPU has a FIFO and games do
        // not have to stall for a fixed 512 CPU cycles after every primitive.
        m_busy_cycles = 32;
        m_ready_cmd = true;
        m_ready_dma = true;
    } else if (cmd == 0xA0 || cmd == 0xC0 || cmd == 0x80) {
        m_busy_cycles = 16;
        m_ready_cmd = true;
        m_ready_dma = true;
    }
}

void GPU::exec_gp0(u32 cmd_word) {
    u32 cmd = cmd_word >> 24;
    const u32* d = m_cmd_fifo.data();
    size_t n = m_cmd_fifo.size();
    m_debug_cmd_count++;
    m_debug_last_cmd = cmd;
    m_debug_cmd_hist[cmd]++;
    if (m_debug_trace) {
        char words[512];
        size_t pos = 0;
        const size_t shown = std::min<size_t>(n, 16);
        for (size_t i = 0; i < shown && pos + 12 < sizeof(words); ++i)
            pos += std::snprintf(words + pos, sizeof(words) - pos, "%08X%s", d[i], (i + 1 < shown) ? " " : "");
        Log::debug("GPU_TRACE cmd=%02X words=%zu [%s]", cmd, n, words);
    }

    if (cmd >= 0x20 && cmd <= 0xFF) {
        static int s_draw_logs = 0;
        // Log textured primitives, fills, transfers; skip repetitive setup
        bool interesting = (cmd >= 0x24 && cmd <= 0x27) || (cmd >= 0x2C && cmd <= 0x2F) ||
            (cmd >= 0x34 && cmd <= 0x37) || (cmd >= 0x3C && cmd <= 0x3F) ||
            (cmd >= 0x64 && cmd <= 0x67) || (cmd >= 0x6C && cmd <= 0x6F) ||
            (cmd >= 0x74 && cmd <= 0x77) || (cmd >= 0x7C && cmd <= 0x7F) ||
            cmd == 0x02 || cmd == 0xA0 || cmd == 0x80 ||
            (cmd >= 0x60 && cmd <= 0x63);
        if (interesting && s_draw_logs < 0) {
            Log::info("GPU GP0 cmd=%02X n=%d d0=%08X d1=%08X d2=%08X d3=%08X",
                cmd, n, n>0?d[0]:0, n>1?d[1]:0, n>2?d[2]:0, n>3?d[3]:0);
            s_draw_logs++;
        }
    }

    switch (cmd) {
        case 0x00: break; // NOP
        case 0x01: break; // Clear texture cache
        case 0x1F: // Interrupt request (sets GPUSTAT.24 + IRQ1)
            m_irq = true;
            if (m_irq_cb) m_irq_cb();
            break;
        case 0x02: { // Fill rectangle in VRAM
            if (n < 3) break;
            u32 color = d[0] & 0xFFFFFF;
            int x = d[1] & 0xFFFF;
            int y = (d[1] >> 16) & 0xFFFF;
            int w = d[2] & 0xFFFF;
            int h = (d[2] >> 16) & 0xFFFF;
            // X/Y are 10/9-bit coordinates. Width rounds up to the next
            // 16-pixel boundary (fast-fill hardware quirk) and is then
            // masked to 10 bits. Height must ALSO be masked to 9 bits —
            // it previously passed through unmasked, so any garbage in
            // the upper bits of the height word (which real hardware
            // ignores) was taken as a literal fill height up to 65535
            // pixels tall. That produced huge bogus VRAM fills (seen as
            // giant solid-color walls/bars overwriting real scene
            // content) any time a game's height field had stray high
            // bits set.
            // FillVram has its own parameter masks. X is aligned to a
            // 16-pixel boundary (bits 0..3 are ignored), while width is
            // rounded up in 16-pixel units after a 10-bit mask. In
            // particular, a raw width of 0x400 masks to zero and therefore
            // performs no fill; it is NOT the same as a 1024-pixel width.
            // Height is an independent 9-bit field, so raw 0x200 also means
            // zero rows.
            x &= 0x3F0;
            y &= 0x1FF;
            w = ((w & 0x3FF) + 0x0F) & ~0x0F;
            h &= 0x1FF;
            static int s_fill_log = 0;
            if (s_fill_log < 32) {
                Log::info("GPU: Fill VRAM rgb=%06X %dx%d @ (%d,%d)", color, w, h, x, y);
                ++s_fill_log;
            }
            fill_rectangle(color, x, y, w, h);
            break;
        }
        case 0x20: case 0x21: case 0x22: case 0x23:
            if (n >= 4) draw_polygon(false, false, (cmd & 2) != 0, (cmd & 1) != 0, 3, d);
            break;
        case 0x24: case 0x25: case 0x26: case 0x27:
            if (n >= 7) draw_polygon(true, false, (cmd & 2) != 0, (cmd & 1) != 0, 3, d);
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            if (n >= 5) draw_polygon(false, false, (cmd & 2) != 0, (cmd & 1) != 0, 4, d);
            break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            if (n >= 9) draw_polygon(true, false, (cmd & 2) != 0, (cmd & 1) != 0, 4, d);
            break;
        case 0x30: case 0x31: case 0x32: case 0x33:
            if (n >= 6) draw_polygon(false, true, (cmd & 2) != 0, (cmd & 1) != 0, 3, d);
            break;
        case 0x34: case 0x35: case 0x36: case 0x37:
            if (n >= 9) draw_polygon(true, true, (cmd & 2) != 0, (cmd & 1) != 0, 3, d);
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            if (n >= 8) draw_polygon(false, true, (cmd & 2) != 0, (cmd & 1) != 0, 4, d);
            break;
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            if (n >= 12) draw_polygon(true, true, (cmd & 2) != 0, (cmd & 1) != 0, 4, d);
            break;
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            if (n >= 3) draw_line(false, (cmd & 2) != 0, d);
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            // Mono polyline — vertices already consumed up to 0x55555555.
            // Draw consecutive segments (skip terminator word at end).
            if (n >= 3) {
                for (size_t i = 1; i + 1 < n; i++) {
                    if ((d[i] & 0xF000F000u) == 0x50005000u ||
                        (d[i+1] & 0xF000F000u) == 0x50005000u) break;
                    u32 seg[3] = { d[0], d[i], d[i+1] };
                    draw_line(false, (cmd & 2) != 0, seg);
                }
            }
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            if (n >= 4) draw_line(true, (cmd & 2) != 0, d);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            // Shaded polyline: (color,vertex) pairs after the command word.
            // Packet: [cmd+c0][v0][c1][v1]...[cN][vN][55555555]
            if (n >= 4) {
                for (size_t i = 0; i + 3 < n; i += 2) {
                    if (d[i+1] == 0x55555555u || d[i+3] == 0x55555555u) break;
                    u32 seg[4] = { d[i], d[i+1], d[i+2], d[i+3] };
                    // First segment uses cmd word as color0
                    if (i == 0) seg[0] = d[0];
                    draw_line(true, (cmd & 2) != 0, seg);
                }
            }
            break;
        case 0x60: case 0x61: case 0x62: case 0x63:
            if (n >= 3) draw_rect(false, (cmd & 2) != 0, (cmd & 1) != 0, 0, d);
            break;
        case 0x64: case 0x65: case 0x66: case 0x67:
            if (n >= 4) draw_rect(true, (cmd & 2) != 0, (cmd & 1) != 0, 0, d);
            break;
        case 0x68: case 0x69: case 0x6A: case 0x6B:
            if (n >= 2) draw_rect(false, (cmd & 2) != 0, (cmd & 1) != 0, 1, d);
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            if (n >= 3) draw_rect(true, (cmd & 2) != 0, (cmd & 1) != 0, 1, d);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73:
            if (n >= 2) draw_rect(false, (cmd & 2) != 0, (cmd & 1) != 0, 8, d);
            break;
        case 0x74: case 0x75: case 0x76: case 0x77:
            if (n >= 3) draw_rect(true, (cmd & 2) != 0, (cmd & 1) != 0, 8, d);
            break;
        case 0x78: case 0x79: case 0x7A: case 0x7B:
            if (n >= 2) draw_rect(false, (cmd & 2) != 0, (cmd & 1) != 0, 16, d);
            break;
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            if (n >= 3) draw_rect(true, (cmd & 2) != 0, (cmd & 1) != 0, 16, d);
            break;
        case 0x80: { // VRAM to VRAM blit
            if (n < 4) break;

            // The command fields are 16-bit, but hardware masks COPY
            // parameters independently: X/Y are coordinates, while size is
            // ((raw-1)&mask)+1 with zero meaning the maximum size.  This is
            // deliberately different from GP0(02h) fill rounding.
            const int sx = d[1] & 0x3FF;
            const int sy = (d[1] >> 16) & 0x1FF;
            const int dx = d[2] & 0x3FF;
            const int dy = (d[2] >> 16) & 0x1FF;
            const u32 raw_w = d[3] & 0xFFFFu;
            const u32 raw_h = (d[3] >> 16) & 0xFFFFu;
            const int w = raw_w ? static_cast<int>(((raw_w - 1u) & 0x3FFu) + 1u) : 1024;
            const int h = raw_h ? static_cast<int>(((raw_h - 1u) & 0x1FFu) + 1u) : 512;

            // GP0(80h) is allowed to copy intersecting rectangles.  A naive
            // forward loop behaves like memcpy() and makes the source depend
            // on pixels that were already overwritten by the same blit.  The
            // real GPU effectively has a read/cache phase for these cases; a
            // temporary source rectangle gives the correct snapshot semantics
            // and also handles X/Y wrapping without carrying into the other
            // axis.
            std::vector<u16> source(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x)
                    source[static_cast<size_t>(y) * w + x] = read_vram(sx + x, sy + y);
            }

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const u16 old = read_vram(dx + x, dy + y);
                    if (m_mask_test && (old & 0x8000u)) continue;
                    u16 pix = source[static_cast<size_t>(y) * w + x];
                    if (m_mask_set) pix |= 0x8000u;
                    write_vram(dx + x, dy + y, pix);
                }
            }
            break;
        }
        case 0xA0: { // CPU to VRAM
            if (n < 3) break;
            m_transfer_x = d[1] & 0x3FF;
            m_transfer_y = (d[1] >> 16) & 0x1FF;
            // GP0(A0h) uses the same masked-size rules as the other VRAM
            // transfer commands: width is an effective 1..1024 pixels and
            // height is an effective 1..512 rows. A raw zero means the full
            // dimension. Crucially, bits above the hardware mask are ignored
            // instead of becoming extra rows/columns; otherwise a perfectly
            // legal upload such as raw height 0x201 becomes 513 rows here,
            // consuming subsequent GP0 words as pixel data and desynchronizing
            // the command stream. That kind of desync can make whole batches
            // of textured geometry disappear.
            const u32 raw_w = d[2] & 0xFFFFu;
            const u32 raw_h = (d[2] >> 16) & 0xFFFFu;
            m_transfer_w = raw_w ? static_cast<int>(((raw_w - 1u) & 0x3FFu) + 1u) : 1024;
            m_transfer_h = raw_h ? static_cast<int>(((raw_h - 1u) & 0x1FFu) + 1u) : 512;
            // Do not round the width to an even number: the pixel rectangle is
            // exact. If W*H is odd, the unused high halfword of the final DMA
            // word is simply padding and write_gp0()'s per-pixel guard ignores it.
            m_transfer_pos = 0;
            m_transfer = TransferMode::CPUToVRAM;
            m_ready_cmd = false;
            m_ready_vram = false;
            m_ready_dma = true; // DMA can feed us
            {
                static int s_a0 = 0;
                if (s_a0 < 30) {
                    Log::info("GPU: CPU->VRAM %dx%d @ (%d,%d)", m_transfer_w, m_transfer_h, m_transfer_x, m_transfer_y);
                    s_a0++;
                }
            }
            break;
        }
        case 0xC0: { // VRAM to CPU
            if (n < 3) break;
            m_transfer_x = d[1] & 0x3FF;
            m_transfer_y = (d[1] >> 16) & 0x1FF;
            const u32 raw_w = d[2] & 0xFFFFu;
            const u32 raw_h = (d[2] >> 16) & 0xFFFFu;
            m_transfer_w = raw_w ? static_cast<int>(((raw_w - 1u) & 0x3FFu) + 1u) : 1024;
            m_transfer_h = raw_h ? static_cast<int>(((raw_h - 1u) & 0x1FFu) + 1u) : 512;
            m_transfer_pos = 0;
            m_transfer = TransferMode::VRAMToCPU;
            // Real hardware sets GPUSTAT bit27 ("ready to send VRAM to CPU")
            // TRUE the instant this command is issued — that's the signal the
            // BIOS polls for before it starts reading GPUREAD. Data is ready
            // to stream immediately, so this must be true, not false.
            m_ready_vram = true;
            break;
        }
        case 0xE1: { // Draw mode setting
            m_texture_page = cmd_word & 0x7FF;
            m_texture_page_x = cmd_word & 0xF;
            m_texture_page_y = (cmd_word >> 4) & 1;
            m_semi_transparency = (cmd_word >> 5) & 3;
            m_texture_depth = (cmd_word >> 7) & 3;
            m_dither = (cmd_word >> 9) & 1;
            m_draw_to_display = (cmd_word >> 10) & 1;
            // E1.11 is the high bit of the 2-bit texture-page Y base. It is
            // NOT a texture-enable control. On retail 1 MiB consoles the bit
            // is ignored by the VRAM address decoder; on v2/2 MiB hardware it
            // selects the upper 512-line bank. It is reflected directly in
            // GPUSTAT.15.
            m_texture_page_y_base2 = ((cmd_word >> 11) & 1) != 0;
            m_texture_rect_x_flip = (cmd_word >> 12) & 1;
            m_texture_rect_y_flip = (cmd_word >> 13) & 1;
            static int s_e1 = 0;
            if (s_e1 < 12) {
                /* log silenced */
                s_e1++;
            }
            break;
        }
        case 0xE2: {
            m_tex_window_mask_x = cmd_word & 0x1F;
            m_tex_window_mask_y = (cmd_word >> 5) & 0x1F;
            m_tex_window_offset_x = (cmd_word >> 10) & 0x1F;
            m_tex_window_offset_y = (cmd_word >> 15) & 0x1F;
            break;
        }
        case 0xE3: {
            m_drawing_x1 = cmd_word & 0x3FF;
            m_drawing_y1 = (cmd_word >> 10) & 0x1FF;
            break;
        }
        case 0xE4: {
            m_drawing_x2 = cmd_word & 0x3FF;
            m_drawing_y2 = (cmd_word >> 10) & 0x1FF;
            break;
        }
        case 0xE5: {
            m_offset_x = static_cast<s16>((cmd_word & 0x7FF) << 5) >> 5;
            m_offset_y = static_cast<s16>(((cmd_word >> 11) & 0x7FF) << 5) >> 5;
            break;
        }
        case 0xE6: {
            m_mask_set = (cmd_word & 1) != 0;
            m_mask_test = (cmd_word & 2) != 0;
            break;
        }
        default:
            break;
    }
}

void GPU::exec_gp1(u32 cmd_word) {
    u32 cmd = (cmd_word >> 24) & 0xFF;
    switch (cmd) {
        case 0x00: // Reset GPU
            reset();
            break;
        case 0x01: // Reset command buffer
            m_cmd_fifo.clear();
            m_cmd_remaining = 0;
            m_polyline = false;
            m_cmd_remaining = 0;
            m_transfer = TransferMode::Idle;
            m_ready_cmd = true;
            m_ready_vram = true;
            break;
        case 0x02: // Ack IRQ
            m_irq = false;
            if (m_irq_ack_cb) m_irq_ack_cb();
            break;
        case 0x03: // Display enable (0=on, 1=off)
            m_display_enable = (cmd_word & 1) == 0;
            static int s_display_logs = 0;
            if (s_display_logs < 40) {
                Log::info("GPU: Display %s", m_display_enable ? "ON" : "OFF");
                ++s_display_logs;
            }
            break;
        case 0x04: // DMA direction
            m_dma_direction = cmd_word & 3;
            break;
        case 0x05: { // Start of display area in VRAM
            int nx = cmd_word & 0x3FF;
            int ny = (cmd_word >> 10) & 0x1FF;
            m_display_x = nx;
            m_display_y = ny;
            // The display origin affects CRTC parity, but it does not directly
            // mutate the renderer's current field. The latter advances on the
            // emulated field boundary.
            if (m_vertical_interlace && m_vres)
                m_active_line_lsb = static_cast<u8>((m_display_y + (m_interlaced_display_field ? 1 : 0)) & 1);
            break;
        }
        case 0x06: { // Horizontal display range
            const int nx1 = cmd_word & 0xFFF;
            const int nx2 = (cmd_word >> 12) & 0xFFF;
            const bool changed = (nx1 != m_display_x1) || (nx2 != m_display_x2);
            m_display_x1 = nx1;
            m_display_x2 = nx2;
            if (changed && Log::level() <= LogLevel::Debug)
                Log::debug("GPU DISPLAY H: x1=%03X x2=%03X width=%d hres=%d", m_display_x1, m_display_x2, display_width(), m_hres);
            break;
        }
        case 0x07: { // Vertical display range
            const int ny1 = cmd_word & 0x3FF;
            const int ny2 = (cmd_word >> 10) & 0x3FF;
            const bool changed = (ny1 != m_display_y1) || (ny2 != m_display_y2);
            m_display_y1 = ny1;
            m_display_y2 = ny2;
            if (changed && Log::level() <= LogLevel::Debug)
                Log::debug("GPU DISPLAY V: y1=%03X y2=%03X height=%d vres=%d interlace=%d", m_display_y1, m_display_y2, display_height(), m_vres, m_vertical_interlace);
            break;
        }
        case 0x08: { // Display mode
            const int nhres = (cmd_word & 0x40) ? 4 : int(cmd_word & 3);
            const int nvres = (cmd_word >> 2) & 1;
            const int nvideo = (cmd_word >> 3) & 1;
            const int ndepth = (cmd_word >> 4) & 1;
            const int ninterlace = (cmd_word >> 5) & 1;
            const int nreverse = (cmd_word >> 7) & 1;
            const bool changed =
                nhres != m_hres || nvres != m_vres || nvideo != m_video_mode ||
                ndepth != m_display_depth_24 || ninterlace != m_vertical_interlace ||
                nreverse != m_reverse_flag;
            m_hres = nhres;
            m_vres = nvres;
            m_video_mode = nvideo;
            m_display_depth_24 = ndepth;
            m_vertical_interlace = ninterlace;
            m_reverse_flag = nreverse;
            // Recompute the active rendering parity from the CRTC state. Field
            // selection itself advances only at the real emulated field
            // boundary in tick(); changing GP1(08) must not reset it.
            if (m_vertical_interlace && m_vres)
                m_active_line_lsb = static_cast<u8>((m_display_y + (m_interlaced_display_field ? 1 : 0)) & 1);
            else
                m_active_line_lsb = 0;
            if (changed && Log::level() <= LogLevel::Debug)
                Log::debug("GPU DISPLAY MODE: hres=%d vres=%d pal=%d 24bpp=%d interlace=%d reverse=%d", m_hres, m_vres, m_video_mode, m_display_depth_24, m_vertical_interlace, m_reverse_flag);
            break;
        }
        case 0x09: // Set VRAM size / upper-bank addressing enable (v2)
            // PS96 currently models a retail 1 MiB VRAM device. The register
            // is still accepted so BIOS probing sees a stable GPU interface;
            // E1.11 remains a real texture-page-Y bit and is never repurposed
            // as a texture-off switch.
            break;
        case 0x10: // Get GPU info
            switch (cmd_word & 0xFFFFFF) {
                case 0x02: m_gpuread_latch = m_texture_window; break;
                case 0x03: m_gpuread_latch = (m_drawing_y1 << 10) | m_drawing_x1; break;
                case 0x04: m_gpuread_latch = (m_drawing_y2 << 10) | m_drawing_x2; break;
                case 0x05: m_gpuread_latch = ((m_offset_y & 0x7FF) << 11) | (m_offset_x & 0x7FF); break;
                case 0x07: m_gpuread_latch = 2; break; // GPU type
                case 0x08: m_gpuread_latch = 0; break;
                default: m_gpuread_latch = 0; break;
            }
            break;
        default:
            break;
    }
}

u16 GPU::read_vram(int x, int y) const {
    x &= 1023;
    y &= 511;
    return m_vram[y * 1024 + x];
}

void GPU::write_vram(int x, int y, u16 value) {
    x &= 1023;
    y &= 511;
    m_vram[y * 1024 + x] = value;
}

void GPU::put_pixel(int x, int y, u16 color, bool semi) {
    x += m_offset_x;
    y += m_offset_y;
    // Drawing area is inclusive on both edges (PS1 hardware)
    if (x < m_drawing_x1 || x > m_drawing_x2 || y < m_drawing_y1 || y > m_drawing_y2)
        return;
    if (x < 0 || y < 0 || x > 1023 || y > 511)
        return;

    // Interlaced rendering is a raster rule, not a presentation effect.
    // DuckStation models this with an active_line_lsb supplied by the CRTC:
    // the drawing engine skips the currently displayed parity unless the
    // draw-to-displayed-field bit is set. Keeping this state separate from the
    // host frame is what prevents half-screen output and field tearing.
    if (m_vertical_interlace && ((y & 1) == static_cast<int>(m_active_line_lsb)))
        return;

    u16 back = read_vram(x, y);
    if (m_mask_test && (back & 0x8000))
        return;
    if (semi) {
        // Real GPU semi-transparency blend modes (GP0(E1) bits 5-6, m_semi_transparency):
        //   0: 0.5*back + 0.5*front   1: back + front   2: back - front   3: back + 0.25*front
        auto chan = [](u16 v, int shift) { return (v >> shift) & 0x1F; };
        int br = chan(back, 0), bg = chan(back, 5), bb = chan(back, 10);
        int fr = chan(color, 0), fg = chan(color, 5), fb = chan(color, 10);
        int r, g, b;
        switch (m_semi_transparency & 3) {
            case 0: r = (br + fr) / 2;         g = (bg + fg) / 2;         b = (bb + fb) / 2;         break;
            case 1: r = br + fr;               g = bg + fg;               b = bb + fb;               break;
            case 2: r = br - fr;               g = bg - fg;               b = bb - fb;               break;
            default: r = br + fr / 4;          g = bg + fg / 4;           b = bb + fb / 4;           break;
        }
        auto clamp5 = [](int v) { return static_cast<u16>(v < 0 ? 0 : (v > 31 ? 31 : v)); };
        color = clamp5(r) | (clamp5(g) << 5) | (clamp5(b) << 10) | (color & 0x8000);
    }
    if (m_mask_set) color |= 0x8000;
    write_vram(x, y, color);
}

void GPU::fill_rectangle(u32 color, int x, int y, int w, int h) {
    // GP0 colors are RGB888 in the low/mid/high bytes and are quantized to
    // PS1 BGR555 storage. Keep this path identical to primitive colors so BIOS
    // logo colors (including saturated red) do not take a different conversion.
    u16 c = static_cast<u16>(((color >> 3) & 0x1F) | (((color >> 11) & 0x1F) << 5) | (((color >> 19) & 0x1F) << 10));
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++)
            put_pixel(x + px - m_offset_x, y + py - m_offset_y, c, false);
}

static inline u16 rgb888_to_555(u32 c) {
    return static_cast<u16>(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
}

static inline u16 rgb888_to_555_dither(u32 c, int x, int y) {
    // Exact PS1 4x4 ordered dither matrix. RGB is saturated in 8-bit space,
    // then reduced to 5 bits. This is the hardware operation, rather than
    // adding a 5-bit-only bias after quantisation.
    static constexpr int dither[16] = {
        -4,  0, -3,  1,
         2, -2,  3, -1,
        -3,  1, -4,  0,
         3, -1,  2, -2
    };
    const int d = dither[(x & 3) + ((y & 3) << 2)];
    int r = std::clamp(int(c & 0xFF) + d, 0, 255);
    int g = std::clamp(int((c >> 8) & 0xFF) + d, 0, 255);
    int b = std::clamp(int((c >> 16) & 0xFF) + d, 0, 255);
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

void GPU::draw_polygon(bool textured, bool gouraud, bool semi, bool raw, int verts, const u32* data) {
    ++m_debug_draw_primitives;
    if (textured) ++m_debug_textured_primitives;
    struct V { int x, y; u32 color; int u, v; };
    V vs[4]{};
    u16 clut_x = 0, clut_y = 0;
    u16 texpage = m_texture_page;

    if (!gouraud && !textured) {
        u32 color = data[0] & 0xFFFFFF;
        for (int i = 0; i < verts; i++) {
            vs[i].x = static_cast<s16>(data[1 + i] & 0xFFFF);
            vs[i].y = static_cast<s16>((data[1 + i] >> 16) & 0xFFFF);
            vs[i].color = color;
        }
    } else if (gouraud && !textured) {
        for (int i = 0; i < verts; i++) {
            vs[i].color = data[i * 2] & 0xFFFFFF;
            vs[i].x = static_cast<s16>(data[i * 2 + 1] & 0xFFFF);
            vs[i].y = static_cast<s16>((data[i * 2 + 1] >> 16) & 0xFFFF);
        }
    } else if (!gouraud && textured) {
        u32 color = data[0] & 0xFFFFFF;
        vs[0].color = color;
        vs[0].x = static_cast<s16>(data[1] & 0xFFFF);
        vs[0].y = static_cast<s16>((data[1] >> 16) & 0xFFFF);
        vs[0].u = data[2] & 0xFF; vs[0].v = (data[2] >> 8) & 0xFF;
        clut_x = (data[2] >> 16) & 0x3F; clut_y = (data[2] >> 22) & 0x1FF;
        for (int i = 1; i < verts; i++) {
            vs[i].color = color;
            vs[i].x = static_cast<s16>(data[i * 2 + 1] & 0xFFFF);
            vs[i].y = static_cast<s16>((data[i * 2 + 1] >> 16) & 0xFFFF);
            vs[i].u = data[i * 2 + 2] & 0xFF; vs[i].v = (data[i * 2 + 2] >> 8) & 0xFF;
            if (i == 1) texpage = (data[i * 2 + 2] >> 16) & 0xFFFF;
        }
    } else {
        for (int i = 0; i < verts; i++) {
            vs[i].color = data[i * 3] & 0xFFFFFF;
            vs[i].x = static_cast<s16>(data[i * 3 + 1] & 0xFFFF);
            vs[i].y = static_cast<s16>((data[i * 3 + 1] >> 16) & 0xFFFF);
            vs[i].u = data[i * 3 + 2] & 0xFF; vs[i].v = (data[i * 3 + 2] >> 8) & 0xFF;
            if (i == 0) { clut_x = (data[2] >> 16) & 0x3F; clut_y = (data[2] >> 22) & 0x1FF; }
            if (i == 1) texpage = (data[5] >> 16) & 0xFFFF;
        }
    }

    /* poly log silenced */

    // Per-polygon tpage (from UV word of vertex 1) overrides GP0(E1) for this draw only.
    // IMPORTANT: texture page 0 is valid. Treating it as "not present" leaves a
    // previous draw's texture depth/page active, which can turn FMV/UI geometry
    // into opaque white blocks or incorrectly sampled textures.
    u16 saved_page = m_texture_page;
    u8  saved_depth = m_texture_depth;
    u8  saved_semi  = m_semi_transparency;
    if (textured) {
        m_texture_page = texpage & 0x7FF;
        m_texture_page_x = texpage & 0xF;
        m_texture_page_y = (texpage >> 4) & 1;
        m_texture_depth = (texpage >> 7) & 3;
        m_semi_transparency = (texpage >> 5) & 3;
    }

    // Axis-aligned mono quad → fast fill. Only valid when the quad's 4
    // vertices are genuinely the 4 corners of its own bounding box (i.e. it
    // really is an axis-aligned rectangle) — otherwise this must fall
    // through to real triangulation below, or skewed/rotated flat quads get
    // incorrectly stamped as solid rectangles.
    if (!textured && !gouraud && verts == 4) {
        int minx = std::min(std::min(vs[0].x, vs[1].x), std::min(vs[2].x, vs[3].x));
        int maxx = std::max(std::max(vs[0].x, vs[1].x), std::max(vs[2].x, vs[3].x));
        int miny = std::min(std::min(vs[0].y, vs[1].y), std::min(vs[2].y, vs[3].y));
        int maxy = std::max(std::max(vs[0].y, vs[1].y), std::max(vs[2].y, vs[3].y));
        bool axis = true;
        for (int i = 0; i < 4; i++) {
            bool on_edge = (vs[i].x == minx || vs[i].x == maxx) && (vs[i].y == miny || vs[i].y == maxy);
            if (!on_edge) { axis = false; break; }
        }
        if (axis && maxx > minx && maxy > miny && (maxx - minx) <= 1024 && (maxy - miny) <= 512) {
            u16 c = rgb888_to_555(vs[0].color);
            int written = 0;
            for (int y = miny; y < maxy; y++) {
                for (int x = minx; x < maxx; x++) {
                    int px = x + m_offset_x;
                    int py = y + m_offset_y;
                    if (px < 0 || py < 0 || px > 1023 || py > 511) continue;
                    if (px < m_drawing_x1 || px > m_drawing_x2 ||
                        py < m_drawing_y1 || py > m_drawing_y2) continue;
                    // Route both opaque and semi-transparent fills through the
                    // normal pixel path so GP0(E6) mask-test/set semantics are
                    // honored exactly like textured/triangle rendering.
                    put_pixel(x, y, c, semi);
                    written++;
                }
            }
            static int s_fill_log = 0;
            if (s_fill_log < 6) {
                /* log silenced */
                s_fill_log++;
            }
            m_texture_page = saved_page;
            m_texture_page_x = saved_page & 0xF;
            m_texture_page_y = (saved_page >> 4) & 1;
            m_texture_depth = saved_depth;
            m_semi_transparency = saved_semi;
            return;
        }
    }

    auto draw_tri = [&](V a, V b, V c) {
        // Flat untextured triangles are common enough to keep a dedicated
        // scanline path. Everything else uses incremental edge equations.
        // The old barycentric loop recomputed three 64-bit edge products and
        // six interpolation products for every pixel; that made textured
        // animation/FMV scenes crawl and present as a slideshow. Here the
        // edge weights and all affine attributes are advanced by constants
        // across each scanline, while retaining the same pixel-centre
        // convention and arithmetic precision.
        if (!textured && !gouraud) {
            if (a.y > b.y) std::swap(a, b);
            if (a.y > c.y) std::swap(a, c);
            if (b.y > c.y) std::swap(b, c);
            if (c.y == a.y) return;
            const u16 color = rgb888_to_555(a.color);
            auto edge = [](const V& p0, const V& p1, int y) -> int {
                if (p1.y == p0.y) return p0.x;
                return p0.x + (int)((s64(p1.x - p0.x) * (y - p0.y)) / (p1.y - p0.y));
            };
            for (int y = a.y; y <= c.y; y++) {
                int x0 = (y < b.y) ? edge(a, b, y) : edge(b, c, y);
                int x1 = edge(a, c, y);
                if (x0 > x1) std::swap(x0, x1);
                for (int x = x0; x <= x1; x++) put_pixel(x, y, color, semi);
            }
            return;
        }

        int minx = std::min(std::min(a.x, b.x), c.x);
        int maxx = std::max(std::max(a.x, b.x), c.x);
        int miny = std::min(std::min(a.y, b.y), c.y);
        int maxy = std::max(std::max(a.y, b.y), c.y);
        if (maxx < minx || maxy < miny) return;

        // Clamp BEFORE calculating the edge/attribute origin. The old code
        // calculated all interpolation values at the unclamped bounding-box
        // origin and only then moved minx/miny into VRAM. That made texture
        // coordinates and Gouraud colours depend on how far a primitive was
        // clipped off-screen, producing black/dark strips and displaced texture
        // blocks on large polygons.
        minx = std::max(minx, -m_offset_x);
        maxx = std::min(maxx, 1023 - m_offset_x);
        miny = std::max(miny, -m_offset_y);
        maxy = std::min(maxy, 511 - m_offset_y);
        minx = std::max(minx, m_drawing_x1 - m_offset_x);
        maxx = std::min(maxx, m_drawing_x2 - m_offset_x);
        miny = std::max(miny, m_drawing_y1 - m_offset_y);
        maxy = std::min(maxy, m_drawing_y2 - m_offset_y);
        if (minx > maxx || miny > maxy) return;

        auto edge_fn = [](int ax, int ay, int bx, int by, int px, int py) -> s64 {
            return s64(bx - ax) * s64(py - ay) - s64(by - ay) * s64(px - ax);
        };

        const int ax2 = a.x * 2, ay2 = a.y * 2;
        const int bx2 = b.x * 2, by2 = b.y * 2;
        const int cx2 = c.x * 2, cy2 = c.y * 2;
        s64 area = edge_fn(ax2, ay2, bx2, by2, cx2, cy2);
        if (area == 0) return;
        const bool flip = area < 0;
        if (flip) area = -area;

        // Edge weights at a pixel centre, plus exact constant increments for
        // moving one pixel horizontally or vertically.
        s64 row_w0 = edge_fn(bx2, by2, cx2, cy2, minx * 2 + 1, miny * 2 + 1);
        s64 row_w1 = edge_fn(cx2, cy2, ax2, ay2, minx * 2 + 1, miny * 2 + 1);
        s64 row_w2 = edge_fn(ax2, ay2, bx2, by2, minx * 2 + 1, miny * 2 + 1);

        s64 w0_dx = -s64(cy2 - by2) * 2;
        s64 w1_dx = -s64(ay2 - cy2) * 2;
        s64 w2_dx = -s64(by2 - ay2) * 2;
        s64 w0_dy =  s64(cx2 - bx2) * 2;
        s64 w1_dy =  s64(ax2 - cx2) * 2;
        s64 w2_dy =  s64(bx2 - ax2) * 2;

        if (flip) {
            row_w0 = -row_w0; row_w1 = -row_w1; row_w2 = -row_w2;
            w0_dx = -w0_dx; w1_dx = -w1_dx; w2_dx = -w2_dx;
            w0_dy = -w0_dy; w1_dy = -w1_dy; w2_dy = -w2_dy;
        }

        // Convert barycentric attribute numerators into incremental fixed-point
        // values once per triangle. This removes the 64-bit multiply/divide
        // pair from every covered pixel. Q12 is more than enough for the PS1's
        // 8-bit UVs and 8-bit vertex colours while staying safely inside s64.
        constexpr int ATTR_SHIFT = 12;
        auto to_fp = [&](s64 n) -> s64 {
            return (n << ATTR_SHIFT) / area;
        };

        const bool needs_uv = textured;
        const bool needs_rgb = gouraud;

        s64 row_u = 0, row_v = 0, u_dx = 0, v_dx = 0, u_dy = 0, v_dy = 0;
        s64 row_r = 0, row_g = 0, row_b = 0, r_dx = 0, g_dx = 0, b_dx = 0;
        s64 r_dy = 0, g_dy = 0, b_dy = 0;
        if (needs_uv) {
            row_u = row_w0 * a.u + row_w1 * b.u + row_w2 * c.u;
            row_v = row_w0 * a.v + row_w1 * b.v + row_w2 * c.v;
            u_dx = w0_dx * a.u + w1_dx * b.u + w2_dx * c.u;
            v_dx = w0_dx * a.v + w1_dx * b.v + w2_dx * c.v;
            u_dy = w0_dy * a.u + w1_dy * b.u + w2_dy * c.u;
            v_dy = w0_dy * a.v + w1_dy * b.v + w2_dy * c.v;
        }
        if (needs_rgb) {
            row_r = row_w0 * s64(a.color & 0xFF) + row_w1 * s64(b.color & 0xFF) + row_w2 * s64(c.color & 0xFF);
            row_g = row_w0 * s64((a.color >> 8) & 0xFF) + row_w1 * s64((b.color >> 8) & 0xFF) + row_w2 * s64((c.color >> 8) & 0xFF);
            row_b = row_w0 * s64((a.color >> 16) & 0xFF) + row_w1 * s64((b.color >> 16) & 0xFF) + row_w2 * s64((c.color >> 16) & 0xFF);
            r_dx = w0_dx * s64(a.color & 0xFF) + w1_dx * s64(b.color & 0xFF) + w2_dx * s64(c.color & 0xFF);
            g_dx = w0_dx * s64((a.color >> 8) & 0xFF) + w1_dx * s64((b.color >> 8) & 0xFF) + w2_dx * s64((c.color >> 8) & 0xFF);
            b_dx = w0_dx * s64((a.color >> 16) & 0xFF) + w1_dx * s64((b.color >> 16) & 0xFF) + w2_dx * s64((c.color >> 16) & 0xFF);
            r_dy = w0_dy * s64(a.color & 0xFF) + w1_dy * s64(b.color & 0xFF) + w2_dy * s64(c.color & 0xFF);
            g_dy = w0_dy * s64((a.color >> 8) & 0xFF) + w1_dy * s64((b.color >> 8) & 0xFF) + w2_dy * s64((c.color >> 8) & 0xFF);
            b_dy = w0_dy * s64((a.color >> 16) & 0xFF) + w1_dy * s64((b.color >> 16) & 0xFF) + w2_dy * s64((c.color >> 16) & 0xFF);
        }

        s64 scan_w0 = row_w0, scan_w1 = row_w1, scan_w2 = row_w2;
        s64 scan_u = to_fp(row_u), scan_v = to_fp(row_v);
        s64 scan_u_dx = to_fp(u_dx), scan_v_dx = to_fp(v_dx);
        s64 scan_u_dy = to_fp(u_dy), scan_v_dy = to_fp(v_dy);
        s64 scan_r = to_fp(row_r), scan_g = to_fp(row_g), scan_b = to_fp(row_b);
        s64 scan_r_dx = to_fp(r_dx), scan_g_dx = to_fp(g_dx), scan_b_dx = to_fp(b_dx);
        s64 scan_r_dy = to_fp(r_dy), scan_g_dy = to_fp(g_dy), scan_b_dy = to_fp(b_dy);

        for (int y = miny; y <= maxy; y++) {
            s64 w0 = scan_w0, w1 = scan_w1, w2 = scan_w2;
            s64 un = scan_u, vn = scan_v;
            s64 rn = scan_r, gn = scan_g, bn = scan_b;

            for (int x = minx; x <= maxx; x++) {
                if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                    u16 color = 0;
                    bool pixel_semi = semi;

                    int r8 = 0, g8 = 0, b8 = 0;
                    if (needs_rgb) {
                        r8 = std::clamp(int(rn >> ATTR_SHIFT), 0, 255);
                        g8 = std::clamp(int(gn >> ATTR_SHIFT), 0, 255);
                        b8 = std::clamp(int(bn >> ATTR_SHIFT), 0, 255);
                    }

                    if (!textured) {
                        const u32 rgb = gouraud
                            ? (u32(r8) | (u32(g8) << 8) | (u32(b8) << 16))
                            : a.color;
                        color = m_dither
                            ? rgb888_to_555_dither(rgb, x + m_offset_x, y + m_offset_y)
                            : rgb888_to_555(rgb);
                        put_pixel(x, y, color, pixel_semi);
                    } else {
                        const int uu = int(un >> ATTR_SHIFT);
                        const int vv = int(vn >> ATTR_SHIFT);
                        const TextureSample sample = sample_texture(uu, vv, clut_x * 16, clut_y);
                        if (!sample.transparent) {
                            const u16 tex = sample.color;
                            pixel_semi = semi && ((tex & 0x8000u) != 0);
                            if (!raw) {
                                const int cr8 = gouraud ? r8 : int(a.color & 0xFF);
                                const int cg8 = gouraud ? g8 : int((a.color >> 8) & 0xFF);
                                const int cb8 = gouraud ? b8 : int((a.color >> 16) & 0xFF);
                                auto modulate5 = [](int tex5, int color8) {
                                    const int tex8 = (tex5 * 255 + 15) / 31;
                                    const int out8 = (tex8 * color8 + 64) >> 7;
                                    return std::clamp((out8 * 31 + 127) / 255, 0, 31);
                                };
                                const int vr = modulate5(tex & 0x1F, cr8);
                                const int vg = modulate5((tex >> 5) & 0x1F, cg8);
                                const int vb = modulate5((tex >> 10) & 0x1F, cb8);
                                color = static_cast<u16>(vr | (vg << 5) | (vb << 10));
                                if (m_dither) {
                                    const u32 modrgb = u32(vr * 255 / 31) |
                                                        (u32(vg * 255 / 31) << 8) |
                                                        (u32(vb * 255 / 31) << 16);
                                    color = rgb888_to_555_dither(modrgb, x + m_offset_x, y + m_offset_y);
                                }
                                color = static_cast<u16>(color | (tex & 0x8000));
                            } else {
                                color = tex;
                            }
                            put_pixel(x, y, color, pixel_semi);
                        } else {
                            ++m_debug_textured_pixels_skipped;
                        }
                    }
                }
                w0 += w0_dx; w1 += w1_dx; w2 += w2_dx;
                if (needs_uv) { un += scan_u_dx; vn += scan_v_dx; }
                if (needs_rgb) { rn += scan_r_dx; gn += scan_g_dx; bn += scan_b_dx; }
            }

            scan_w0 += w0_dy; scan_w1 += w1_dy; scan_w2 += w2_dy;
            if (needs_uv) { scan_u += scan_u_dy; scan_v += scan_v_dy; }
            if (needs_rgb) { scan_r += scan_r_dy; scan_g += scan_g_dy; scan_b += scan_b_dy; }
        }
    };

    // PS1 hardware splits a 4-vertex polygon into exactly two triangles:
    // (v0,v1,v2) and (v1,v2,v3). Drawing any more than that (previously this
    // also rendered (v0,v2,v3) and (v0,v1,v3)) double-covers parts of the
    // quad with two bogus/overlapping triangles — the main source of stray
    // pixels: semi-transparent blends get applied twice over the overlap,
    // and textured quads sample UVs outside the real shape along the extra
    // diagonal triangle.
    draw_tri(vs[0], vs[1], vs[2]);
    if (verts == 4) {
        draw_tri(vs[1], vs[2], vs[3]);
    }

    m_texture_page = saved_page;
    m_texture_page_x = saved_page & 0xF;
    m_texture_page_y = (saved_page >> 4) & 1;
    m_texture_depth = saved_depth;
    m_semi_transparency = saved_semi;
}

void GPU::draw_rect(bool textured, bool semi, bool raw, int size, const u32* data) {
    ++m_debug_draw_primitives;
    if (textured) ++m_debug_textured_primitives;
    u32 color = data[0] & 0xFFFFFF;
    int x = static_cast<s16>(data[1] & 0xFFFF);
    int y = static_cast<s16>((data[1] >> 16) & 0xFFFF);
    int w = size, h = size;
    int u = 0, v = 0;
    u16 clut_x = 0, clut_y = 0;
    if (size == 0) {
        if (textured) {
            u = data[2] & 0xFF; v = (data[2] >> 8) & 0xFF;
            clut_x = (data[2] >> 16) & 0x3F; clut_y = (data[2] >> 22) & 0x1FF;
            // Variable-size rectangles have 10-bit Xsiz and 9-bit Ysiz.
            // Bits outside those fields are ignored by the GPU; treating
            // them as part of the size can reject otherwise valid sprites
            // and make an entire UI/scene layer disappear. Unlike VRAM copy
            // commands, zero does not mean the maximum for render rectangles.
            w = data[3] & 0x3FF;
            h = (data[3] >> 16) & 0x1FF;
        } else {
            w = data[2] & 0x3FF;
            h = (data[2] >> 16) & 0x1FF;
        }
    } else if (textured) {
        u = data[2] & 0xFF; v = (data[2] >> 8) & 0xFF;
        clut_x = (data[2] >> 16) & 0x3F; clut_y = (data[2] >> 22) & 0x1FF;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 512) {
        /* log silenced */
        return;
    }
    static int s_rect_logs = 0;
    if (s_rect_logs < 0) {
        Log::info("GPU rect tex=%d %dx%d @(%d,%d) uv=(%d,%d) clut=(%d,%d) color=%06X",
            textured?1:0, w, h, x, y, u, v, clut_x, clut_y, color);
    }
    u16 solid = rgb888_to_555(color);
    int drawn = 0;
    int skipped = 0;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            u16 c = solid;
            bool pixel_semi = semi;
            if (textured) {
                const TextureSample sample = sample_texture(
                    (m_texture_rect_x_flip ? (u + w - 1 - px) : (u + px)),
                    (m_texture_rect_y_flip ? (v + h - 1 - py) : (v + py)),
                    clut_x * 16, clut_y);
                // Indexed texture transparency is based on source index 0.
                if (sample.transparent) { skipped++; ++m_debug_textured_pixels_skipped; continue; }
                c = sample.color;
                pixel_semi = semi && ((c & 0x8000u) != 0);
                if (!raw) {
                    auto modulate5 = [](int tex5, int color8) {
                        const int tex8 = (tex5 * 255 + 15) / 31;
                        const int out8 = (tex8 * color8 + 64) >> 7;
                        return std::clamp((out8 * 31 + 127) / 255, 0, 31);
                    };
                    int tr = c & 0x1F, tg = (c >> 5) & 0x1F, tb = (c >> 10) & 0x1F;
                    int vr = modulate5(tr, color & 0xFF);
                    int vg = modulate5(tg, (color >> 8) & 0xFF);
                    int vb = modulate5(tb, (color >> 16) & 0xFF);
                    c = static_cast<u16>(vr | (vg << 5) | (vb << 10) | (c & 0x8000));
                }
            }
            put_pixel(x + px, y + py, c, pixel_semi);
            drawn++;
        }
    }
    if (s_rect_logs < 0) {
        Log::info("GPU rect done drawn=%d skipped=%d", drawn, skipped);
    }
    s_rect_logs++;
}

void GPU::draw_line(bool, bool semi, const u32* data) {
    u32 color = data[0] & 0xFFFFFF;
    int x0 = static_cast<s16>(data[1] & 0xFFFF);
    int y0 = static_cast<s16>((data[1] >> 16) & 0xFFFF);
    int x1 = static_cast<s16>(data[2] & 0xFFFF);
    int y1 = static_cast<s16>((data[2] >> 16) & 0xFFFF);
    u16 c = rgb888_to_555(color);
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        put_pixel(x0, y0, c, semi);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

GPU::TextureSample GPU::sample_texture(int u, int v, u16 clut_x, u16 clut_y) const {
    const int tp_x = (m_texture_page_x & 0xF) * 64;
    const int tp_y = (m_texture_page_y & 1) * 256;

    const int mask_x = (m_tex_window_mask_x & 0x1F) * 8;
    const int mask_y = (m_tex_window_mask_y & 0x1F) * 8;
    const int off_x  = (m_tex_window_offset_x & 0x1F) * 8;
    const int off_y  = (m_tex_window_offset_y & 0x1F) * 8;
    u = (u & ~mask_x) | (off_x & mask_x);
    v = (v & ~mask_y) | (off_y & mask_y);
    u &= 255;
    v &= 255;

    const int depth = m_texture_depth & 3;
    u16 color = 0;
    if (depth == 0) { // 4-bit CLUT
        const int tx = (tp_x + (u >> 2)) & 1023;
        const int ty = (tp_y + v) & 511;
        const u16 texel = read_vram(tx, ty);
        const int index = (texel >> ((u & 3) * 4)) & 0x0F;
        color = read_vram((clut_x + index) & 1023, clut_y & 511);
    } else if (depth == 1) { // 8-bit CLUT
        const int tx = (tp_x + (u >> 1)) & 1023;
        const int ty = (tp_y + v) & 511;
        const u16 texel = read_vram(tx, ty);
        const int index = (u & 1) ? ((texel >> 8) & 0xFF) : (texel & 0xFF);
        color = read_vram((clut_x + index) & 1023, clut_y & 511);
    } else {
        color = read_vram((tp_x + u) & 1023, (tp_y + v) & 511);
    }

    // Transparency is determined by the resulting 15-bit texel, not by the
    // palette index. DuckStation's software renderer uses the same rule:
    // after resolving a 4/8-bit CLUT entry, only colour 0000h is transparent;
    // a non-zero CLUT entry at index 0 is a valid visible texel. The old S96
    // implementation tested index==0 instead, which discarded legitimate
    // pixels and simultaneously drew CLUT entries whose actual colour was 0.
    return {color, color == 0};
}

u16 GPU::get_texture_color(int u, int v, u16 clut_x, u16 clut_y) const {
    return sample_texture(u, v, clut_x, clut_y).color;
}

int GPU::display_width() const {
    // The active display width is determined by GP1(06), not merely the
    // resolution selector. The horizontal range is expressed in dot-clock
    // units; using the hardware divider here keeps 256/320/512/640/368
    // modes correct while also handling BIOS/game custom ranges.
    static constexpr int dividers[] = {10, 8, 5, 4, 7, 7};
    const int mode = std::clamp(m_hres, 0, 5);
    const int divider = dividers[mode];
    const int cycles_per_line = (m_video_mode != 0) ? 3406 : 3413;
    int x1 = std::clamp(m_display_x1, 0, cycles_per_line);
    int x2 = std::clamp(m_display_x2, 0, cycles_per_line);
    if (x2 <= x1) return mode == 4 ? 368 : (mode == 3 ? 640 : mode == 2 ? 512 : mode == 1 ? 320 : 256);
    const int cycles = x2 - x1;
    const int width = ((cycles / divider) + 2) & ~3;
    return std::clamp(width, 4, 1024);
}

int GPU::display_height() const {
    // GP1(07) selects the vertical display range. In interlaced mode the
    // displayed image contains both fields, so the reported image height is
    // doubled, matching the observable framebuffer dimensions.
    const int total_scanlines = (m_video_mode != 0) ? 314 : 263;
    const int y1 = std::clamp(m_display_y1, 0, total_scanlines);
    const int y2 = std::clamp(m_display_y2, 0, total_scanlines);
    int height = (y2 > y1) ? (y2 - y1) : (m_vres ? 480 : 240);
    if (m_vertical_interlace) height *= 2;
    return std::clamp(height, 1, 576);
}

void GPU::copy_display_to(u8* rgba, int out_w, int out_h) {
    int dw = display_width();
    int dh = display_height();
    if (dw < 1) dw = 320;
    if (dh < 1) dh = 240;

    // GP1(05h) sets the VRAM origin of the displayed image. Trust it exactly
    // as programmed — do not second-guess it with a "does this region look
    // nonzero" heuristic. That heuristic (previously here) would silently
    // substitute a guessed VRAM address whenever the real display buffer
    // happened to sample as mostly black (e.g. during a fade-in, or a
    // buffer that just hasn't been fully painted yet this frame), showing
    // whatever unrelated leftover VRAM content it guessed instead — a
    // classic source of "wrong colors and placement" / garbled strips.
    int src_x = m_display_x & 1023;
    int src_y = m_display_y & 511;

    // 24-bit direct color mode (GP1(08h) bit4): VRAM stores 3 bytes (R,G,B)
    // per displayed pixel rather than one 16-bit 555 halfword — 2 pixels
    // pack into 3 consecutive halfwords. Byte address of displayed pixel N
    // in a row is (src_x*2 + N*3). Not decoding this and reading VRAM as
    // plain 555 (previous behaviour) reinterprets 24-bit RGB byte triples
    // as 16-bit 555 pixels: colors come out wrong AND the image effectively
    // desyncs/stretches horizontally, since 3 source bytes no longer line
    // up with 2 destination pixels — exactly "wrong colors and placement,
    // logo too big".
    auto read_vram_byte = [&](int byte_addr, int y) -> u8 {
        int col = (byte_addr >> 1) & 1023;
        u16 hw = read_vram(col, y);
        return (byte_addr & 1) ? static_cast<u8>(hw >> 8) : static_cast<u8>(hw & 0xFF);
    };

    int nonzero = 0;
    for (int y = 0; y < out_h; y++) {
        // For interlaced 480/576 output, the display engine presents the
        // complete woven VRAM rectangle. Do not resample the two fields
        // independently or duplicate a field; each output row maps to the
        // corresponding VRAM row. Non-interlaced modes retain the old scaled
        // mapping for native240p presentation.
        int sy = m_vertical_interlace && dh == out_h
            ? src_y + y
            : src_y + (y * dh) / out_h;
        for (int x = 0; x < out_w; x++) {
            int col = (x * dw) / out_w; // pixel index within the display row
            const int shown_col = m_reverse_flag ? (dw - 1 - col) : col;
            int r, g, b;
            if (m_display_depth_24) {
                int byte_off = src_x * 2 + shown_col * 3;
                r = read_vram_byte(byte_off + 0, sy & 511);
                g = read_vram_byte(byte_off + 1, sy & 511);
                b = read_vram_byte(byte_off + 2, sy & 511);
                if (r | g | b) nonzero++;
            } else {
                int sx = src_x + shown_col;
                u16 pix = read_vram(sx & 1023, sy & 511);
                if (pix & 0x7FFF) nonzero++;
                // PS1 VRAM is BGR555 in the 16-bit word. Expand 5 bits to 8
                // with bit replication (abcde -> abcdeabc), not a plain <<3;
                // the latter makes dark gradients and shadow detail visibly
                // too dark.
                const int cr = pix & 0x1F;
                const int cg = (pix >> 5) & 0x1F;
                const int cb = (pix >> 10) & 0x1F;
                r = (cr << 3) | (cr >> 2);
                g = (cg << 3) | (cg >> 2);
                b = (cb << 3) | (cb >> 2);
            }
            int i = (y * out_w + x) * 4;
            rgba[i + 0] = static_cast<u8>(r);
            rgba[i + 1] = static_cast<u8>(g);
            rgba[i + 2] = static_cast<u8>(b);
            rgba[i + 3] = 255;
        }
    }
    static int s_last_nz = -1;
    if (nonzero != s_last_nz && nonzero > 0) {
        /* log silenced */
        s_last_nz = nonzero;
    }
}

void GPU::tick(int cycles) {
    if (cycles <= 0) return;
    if (m_busy_cycles > 0) {
        m_busy_cycles -= cycles;
        if (m_busy_cycles <= 0) {
            m_busy_cycles = 0;
            m_ready_cmd = m_ready_vram = m_ready_dma = true;
        }
    }

    // The GPU has an independent video clock. Convert CPU time to the number
    // of GPU video clocks elapsed using the console region, then consume the
    // exact horizontal timing. This removes the old hard-coded 2146-CPU-cycle
    // scanline approximation and avoids accumulating drift.
    const bool pal = (m_video_mode != 0);
    // Keep the raster cadence internally consistent with the PS1 HBlank clock.
    // NTSC Timer1 already uses 2146 master cycles/line; using the video-dot
    // oscillator ratio here introduced a ~0.3% line-rate mismatch, so one
    // 60 Hz CPU frame (564480 cycles) only advanced ~262.7 lines. The BIOS
    // consequently saw the wrong scanline/field state and could sit on the
    // SCE logo or animate with visible timing corruption.
    constexpr double ntsc_cpu_cycles_per_line = 564480.0 / 263.0;
    constexpr double pal_cpu_cycles_per_line  = 677376.0 / 314.0;
    const double cpu_cycles_per_line = pal ? pal_cpu_cycles_per_line
                                           : ntsc_cpu_cycles_per_line;

    m_cycle_counter += static_cast<double>(cycles);
    const int lines_per_frame = pal ? 314 : 263;
    // Each NTSC/PAL field has roughly 240/288 visible lines. Interlaced 480/576
    // modes still use two fields; they do not turn the single-field raster into
    // 480/576 scanlines.
    const int active_lines = pal ? 288 : 240;
    while (m_cycle_counter >= cpu_cycles_per_line) {
        m_cycle_counter -= cpu_cycles_per_line;
        const int prev = m_scanline;
        const bool was_vblank = m_in_vblank;
        m_scanline = (m_scanline + 1) % lines_per_frame;
        m_in_vblank = (m_scanline >= active_lines);

        // On entry to vertical blank, the CRTC switches the displayed field
        // to the opposite of the field currently being drawn in 480i. At the
        // frame wrap the renderer field toggles for the next field. This is the
        // same separation used by DuckStation's CRTC state.
        if (!was_vblank && m_in_vblank && m_vertical_interlace && m_vres)
            m_interlaced_display_field = !m_interlace_field;

        if (m_scanline < prev && m_vertical_interlace)
            m_interlace_field = !m_interlace_field;

        if (m_vertical_interlace && m_vres)
            m_active_line_lsb = static_cast<u8>((m_display_y + (m_interlaced_display_field ? 1 : 0)) & 1);
        else
            m_active_line_lsb = 0;
    }
}



void GPU::force_vblank(bool in_blank) {
    // Compatibility helper for the frontend. Timing itself is owned by tick();
    // do not rewrite the raster position at host-frame boundaries.
    m_in_vblank = in_blank;
    if (in_blank) {
        // Each NTSC/PAL field has roughly 240/288 visible lines. Interlaced 480/576
    // modes still use two fields; they do not turn the single-field raster into
    // 480/576 scanlines.
        const int active_lines = (m_video_mode != 0) ? 288 : 240;
        m_scanline = active_lines;
    }
}

void GPU::dump_debug_stats() const {
    Log::info("GPU SUMMARY: commands=%u draws=%llu textured=%llu transparent_skips=%llu unknown=%llu last=%02X",
              m_debug_cmd_count,
              static_cast<unsigned long long>(m_debug_draw_primitives),
              static_cast<unsigned long long>(m_debug_textured_primitives),
              static_cast<unsigned long long>(m_debug_textured_pixels_skipped),
              static_cast<unsigned long long>(m_debug_unknown_commands),
              m_debug_last_cmd);
    if (Log::level() <= LogLevel::Debug) {
        for (unsigned i = 0; i < 256; ++i) {
            if (m_debug_cmd_hist[i])
                Log::debug("GPU CMD %02X count=%llu", i, static_cast<unsigned long long>(m_debug_cmd_hist[i]));
        }
    }
}


} // namespace ps96

