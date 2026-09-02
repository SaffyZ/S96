#pragma once

#include "common/Types.hpp"
#include <array>
#include <vector>
#include <functional>

namespace ps96 {

class Renderer;

class GPU {
public:
    GPU();
    void reset();
    void set_renderer(Renderer* r) { m_renderer = r; }

    u32 read_gpuread();
    u32 read_gpustat();

    void write_gp0(u32 value);
    void write_gp1(u32 value);

    // DMA
    void dma_write(u32 value);
    u32  dma_read();
    bool dma_ready() const;

    // VRAM access
    u16* vram() { return m_vram.data(); }
    const u16* vram() const { return m_vram.data(); }

    // Display
    int display_width() const;
    int display_height() const;
    int display_x() const { return m_display_x; }
    int display_y() const { return m_display_y; }
    bool display_enabled() const { return m_display_enable; }
    bool is_interlaced() const { return m_vertical_interlace; }
    int hres_raw() const { return m_hres; }
    int vres_raw() const { return m_vres; }
    bool is_24bit() const { return m_display_depth_24; }
    bool is_pal() const { return m_video_mode != 0; }

    // Frame buffer for presentation
    void copy_display_to(u8* rgba, int out_w, int out_h);
    u16 peek_vram(int x, int y) const { return read_vram(x, y); }
    // Debug instrumentation: incremented on every GP0 command dispatch, used
    // by the headless trace harness to correlate commands with calling PC.
    u32 debug_cmd_count() const { return m_debug_cmd_count; }
    u32 debug_last_cmd() const { return m_debug_last_cmd; }
    u64 debug_textured_pixels_skipped() const { return m_debug_textured_pixels_skipped; }
    u64 debug_draw_primitives() const { return m_debug_draw_primitives; }
    u64 debug_textured_primitives() const { return m_debug_textured_primitives; }
    u64 debug_unknown_commands() const { return m_debug_unknown_commands; }
    void dump_debug_stats() const;
    void set_debug_trace(bool enabled) { m_debug_trace = enabled; }

    // Timing
    void tick(int cycles);
    void force_vblank(bool in_blank);
    void flip_field() { m_interlace_field = !m_interlace_field; }
    bool vblank() const { return m_in_vblank; }
    int scanline() const { return m_scanline; }
    void set_irq_callback(std::function<void()> cb) { m_irq_cb = std::move(cb); }
    void set_irq_ack_callback(std::function<void()> cb) { m_irq_ack_cb = std::move(cb); }

private:
    std::array<u16, 1024 * 512> m_vram{};
    Renderer* m_renderer = nullptr;
    std::function<void()> m_irq_cb;
    std::function<void()> m_irq_ack_cb;

    // GP0 command FIFO
    std::vector<u32> m_cmd_fifo;
    u32 m_cmd_remaining = 0;
    u32 m_current_cmd = 0;
    // Polyline (GP0 0x48-4F / 0x58-5F): variable length, ends with 0x55555555
    bool m_polyline = false;

    // Drawing state
    s16 m_drawing_x1 = 0, m_drawing_y1 = 0;
    s16 m_drawing_x2 = 1023, m_drawing_y2 = 511;
    s16 m_offset_x = 0, m_offset_y = 0;
    u16 m_texture_page = 0;
    u16 m_texture_window = 0;
    u8  m_tex_window_mask_x = 0, m_tex_window_mask_y = 0;
    u8  m_tex_window_offset_x = 0, m_tex_window_offset_y = 0;
    bool m_dither = false;
    bool m_draw_to_display = false;
    bool m_texture_page_y_base2 = false;
    u8  m_semi_transparency = 0;
    u8  m_texture_page_x = 0, m_texture_page_y = 0;
    u8  m_texture_depth = 0; // 0=4bit,1=8bit,2=15bit
    bool m_texture_rect_x_flip = false, m_texture_rect_y_flip = false;
    bool m_mask_test = false;
    bool m_mask_set = false;

    // Display
    int m_display_x = 0, m_display_y = 0;
    int m_display_x1 = 0, m_display_x2 = 0;
    int m_display_y1 = 0, m_display_y2 = 0;
    bool m_display_enable = false;
    bool m_vertical_interlace = false;
    bool m_display_depth_24 = false;
    bool m_reverse_flag = false;
    int m_hres = 0; // 0=256,1=320,2=512,3=640,4=368
    int m_vres = 0; // 0=240,1=480
    int m_video_mode = 0; // 0=NTSC,1=PAL
    // GPUSTAT bit13: interlace field. In 480-line interlace this toggles once per field.
    // In 240-line interlace the rendered parity follows the live raster parity.
    bool m_interlace_field = false;
    // CRTC field state mirrors the hardware model used by modern PS1 emulators.
    // interlaced_display_field is the field currently being scanned out;
    // active_line_lsb is the VRAM row parity rendered by the drawing engine.
    bool m_interlaced_display_field = false;
    u8 m_active_line_lsb = 0;

    // Transfer
    enum class TransferMode { Idle, CPUToVRAM, VRAMToCPU, VRAMToVRAM };
    TransferMode m_transfer = TransferMode::Idle;
    int m_transfer_x = 0, m_transfer_y = 0;
    int m_transfer_w = 0, m_transfer_h = 0;
    int m_transfer_pos = 0;
    u32 m_gpuread_latch = 0;

    // Status bits
    bool m_ready_cmd = true;
    bool m_ready_vram = true;
    bool m_ready_dma = true;
    // Cycles remaining where GPU appears busy (clears ready bits).
    // Real GPU is async; without a busy window, BIOS "wait until busy"
    // loops spin forever because we finish every command instantly.
    int m_busy_cycles = 0;
    u32 m_dma_direction = false; // 0=off,1=?,2=CPU->GP0,3=GPUREAD->CPU
    bool m_irq = false;
    bool m_in_vblank = false;
    int m_scanline = 0;

    int m_dot_clock_div = 10;
    u32 m_debug_cmd_count = 0;
    u32 m_debug_last_cmd = 0;
    u64 m_debug_textured_pixels_skipped = 0;
    u64 m_debug_draw_primitives = 0;
    u64 m_debug_textured_primitives = 0;
    u64 m_debug_unknown_commands = 0;
    std::array<u64, 256> m_debug_cmd_hist{};
    bool m_debug_trace = false;
    // Fractional CPU->video timing. The PS1 GPU has its own oscillator, so
    // scanline timing is not exactly a fixed number of CPU cycles.
    double m_cycle_counter = 0.0;

    void process_gp0_command();
    void exec_gp0(u32 cmd);
    void exec_gp1(u32 cmd);

    void draw_polygon(bool textured, bool gouraud, bool semi, bool raw, int verts, const u32* data);
    void draw_rect(bool textured, bool semi, bool raw, int size, const u32* data);
    void draw_line(bool gouraud, bool semi, const u32* data);
    void fill_rectangle(u32 color, int x, int y, int w, int h);

    u16 read_vram(int x, int y) const;
    void write_vram(int x, int y, u16 value);
    struct TextureSample {
        u16 color = 0;
        bool transparent = true;
    };
    TextureSample sample_texture(int u, int v, u16 clut_x, u16 clut_y) const;
    u16 get_texture_color(int u, int v, u16 clut_x, u16 clut_y) const;

    void put_pixel(int x, int y, u16 color, bool semi = false);
};

} // namespace ps96
