#include "controllers/Controllers.hpp"
#include "emulator/InterruptController.hpp"
#include "dma/DMA.hpp"
#include "memory/Bus.hpp"
#include "cdrom/CDROM.hpp"
#include "cdrom/Disc.hpp"
#include "gpu/GPU.hpp"
#include "gte/GTE.hpp"
#include <cassert>
#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>

using namespace ps96;

static u8 sio_byte(Controllers& c, u8 v) {
    c.write32(0x1F801040, v);
    return static_cast<u8>(c.read32(0x1F801040));
}

int main() {
    // SIO0: a complete digital poll, including delayed /ACK edges and
    // independent port selection.
    InterruptController irq;
    Controllers sio;
    sio.set_irq(&irq);
    irq.set_mask(1u << InterruptController::CONTROLLER);
    PadState pad{};
    pad.start = true;
    pad.cross = true;
    sio.set_pad_state(0, pad);

    auto transfer_byte = [&](u8 tx, bool expect_ack) -> u8 {
        sio.write32(0x1F801040, tx);
        // SIO0 does not expose RX data until the serial byte has completed.
        sio.tick(1088);
        const u8 rx = static_cast<u8>(sio.read8(0x1F801040));
        if (expect_ack) {
            sio.tick(100);
            assert((sio.read8(0x1F801044) & 0x80u) != 0);
            assert((irq.status() & (1u << InterruptController::CONTROLLER)) != 0);
            sio.tick(100);
            assert((sio.read8(0x1F801044) & 0x80u) == 0);
            // Correct acknowledge ordering: clear I_STAT.7 first, then
            // acknowledge the SIO local latch with JOY_CTRL.4.
            irq.acknowledge(~(1u << InterruptController::CONTROLLER));
            sio.write32(0x1F80104A, static_cast<u32>(sio.read16(0x1F80104A)) | 0x0010u);
        }
        return rx;
    };

    sio.write32(0x1F80104A, 0x1003); // TXEN + DTR + DSRE, port 1
    assert(transfer_byte(0x01, true) == 0xFF);
    assert(transfer_byte(0x42, true) == 0x41);
    assert(transfer_byte(0x00, true) == 0x5A);
    const u8 low = transfer_byte(0x00, true);
    const u8 high = transfer_byte(0x00, false);
    assert((low & 0x08) == 0);
    assert((high & 0x40) == 0);

    // DualShock configuration: 0x43 enters configuration mode and 0x44
    // selects analog mode. Games such as Gran Turismo use this negotiation;
    // the mode byte can arrive at either of the two header positions observed
    // by hardware/software implementations, so both are accepted by S96.
    const auto cfg_xfer = [&](u8 tx) -> u8 {
        sio.write32(0x1F801040, tx);
        sio.tick(1088);
        return static_cast<u8>(sio.read8(0x1F801040));
    };
    assert(cfg_xfer(0x01) == 0xFF);
    assert(cfg_xfer(0x43) == 0x41);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x01);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x00);
    assert(cfg_xfer(0x01) == 0xFF);
    assert(cfg_xfer(0x44) == 0xF3);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x01);
    (void)cfg_xfer(0x00);
    (void)cfg_xfer(0x02);
    (void)cfg_xfer(0xFF);
    (void)cfg_xfer(0xFF);
    (void)cfg_xfer(0xFF);
    (void)cfg_xfer(0xFF);
    // Poll in the newly negotiated analog mode.
    assert(cfg_xfer(0x01) == 0xFF);
    assert(cfg_xfer(0x42) == 0x73);

    // Changing the selected port while DTR stays asserted starts a fresh
    // transaction. This is how the BIOS alternates port 1 and port 2.
    sio.write32(0x1F80104A, 0x3003);
    assert(transfer_byte(0x01, true) == 0xFF);
    assert(transfer_byte(0x42, true) == 0x41);

    // BIOSes commonly switch JOY_CTRL.13 between port 1 and port 2 without
    // inserting a second controller command sequence in software. The port
    // switch must terminate the old packet so the next 0x01 is an address byte,
    // not byte 2 of the previous controller's transaction.
    sio.write32(0x1F80104A, 0x1003);
    assert(transfer_byte(0x01, true) == 0xFF);
    sio.write32(0x1F80104A, 0x3003);
    assert(transfer_byte(0x01, true) == 0xFF);
    assert(transfer_byte(0x42, true) == 0x41);

    // DMA DICR: channel 6 OTC completion raises DICR flag and I_STAT bit3.
    Bus bus;
    DMA dma;
    dma.set_bus(&bus);
    dma.set_irq(&irq);
    bus.set_dma(&dma);
    irq.acknowledge(0xFFFFFFFFu);
    dma.write32(0x1F8010F0, 0x0F654321); // enable channel 6
    dma.write32(0x1F8010F4, 0x00C00000); // CH6 IRQ enable + master enable
    dma.write32(0x1F8010E0, 0x00000100);
    dma.write32(0x1F8010E4, 0x00000002);
    dma.write32(0x1F8010E8, 0x01000002);
    const u32 dicr = dma.read32(0x1F8010F4);
    assert((dicr & (1u << 30)) != 0);
    assert((dicr & 0x80000000u) != 0);
    assert((irq.status() & (1u << InterruptController::DMA)) != 0);
    dma.write32(0x1F8010F4, 0x40000000); // W1C CH6 flag
    assert((dma.read32(0x1F8010F4) & (1u << 30)) == 0);
    assert((irq.status() & (1u << InterruptController::DMA)) != 0);
    irq.acknowledge(~(1u << InterruptController::DMA));
    assert((irq.status() & (1u << InterruptController::DMA)) == 0);

    // GPU framebuffer byte order: PS1 VRAM is BGR555 with red in bits 0..4.
    GPU gpu;
    gpu.vram()[0] = 0x001F;
    std::array<u8, 4> rgba{};
    gpu.copy_display_to(rgba.data(), 1, 1);
    assert(rgba[0] >= 248 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);

    // The default GP1 display ranges describe a 320x240 NTSC image. Width and
    // height must come from those programmed ranges rather than being hard-coded
    // to the resolution selector, because BIOS and games change the ranges.
    gpu.reset();
    assert(gpu.display_width() == 320);
    assert(gpu.display_height() == 240);

    // GP0 FillVram uses the same RGB888 -> BGR555 ordering.
    gpu.reset();
    gpu.write_gp0(0x020000FFu); // pure red in RGB888 (R is low byte)
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x00010001u);
    assert((gpu.peek_vram(0, 0) & 0x7FFF) == 0x001F);
    // GPUSTAT.26/28 are interface/FIFO readiness bits, not a blanket
    // raster-engine-idle bit. A completed draw must not force software to
    // stall for a synthetic fixed busy window before issuing the next command.
    assert((gpu.read_gpustat() & (1u << 26)) != 0);
    assert((gpu.read_gpustat() & (1u << 28)) != 0);

    // Semi-transparent GP0 rectangle uses the selected hardware blend mode.
    gpu.reset();
    gpu.vram()[0] = 0x001F; // destination: full red
    gpu.write_gp0(0xE1000000u); // draw mode, semitransparency mode 0
    gpu.write_gp0(0x62FF0000u); // blue, semi-transparent opaque rectangle
    gpu.write_gp0(0x00000000u); // x=0,y=0
    gpu.write_gp0(0x00010001u); // w=1,h=1
    const u16 blended = gpu.peek_vram(0, 0);
    assert((blended & 0x1F) == 15);
    assert(((blended >> 10) & 0x1F) == 15);

    // Validate all four hardware blend modes in BGR555 space.
    const u16 back = static_cast<u16>(31 | (0 << 5) | (0 << 10));
    const u8 blend_cmds[] = {0x62, 0x62, 0x62, 0x62};
    const u16 expected[] = {
        static_cast<u16>(15 | (15 << 10)),
        static_cast<u16>(31 | (31 << 10)),
        static_cast<u16>(31),
        static_cast<u16>(31 | (7 << 10))
    };
    for (int mode = 0; mode < 4; ++mode) {
        gpu.reset();
        gpu.vram()[0] = back;
        gpu.write_gp0(0xE1000000u | (static_cast<u32>(mode) << 5));
        // Low 24 bits are RGB888. Use full blue as the front pixel;
        // command bit1 enables semi-transparency and E1 bits5-6 choose mode.
        gpu.write_gp0((static_cast<u32>(blend_cmds[mode]) << 24) | 0x00FF0000u);
        gpu.write_gp0(0x00000000u);
        gpu.write_gp0(0x00010001u);
        const u16 got = gpu.peek_vram(0, 0);
        assert((got & 0x7FFF) == expected[mode]);
    }

    // Textured modulation: white vertex colour is identity, not saturation.
    gpu.reset();
    gpu.vram()[0] = static_cast<u16>(7 | (9 << 5) | (11 << 10));
    gpu.write_gp0(0xE1000000u | (2u << 7));
    gpu.write_gp0(0x64808080u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x00010001u);
    const u16 modulated = gpu.peek_vram(0, 0);
    assert((modulated & 0x1F) >= 6 && (modulated & 0x1F) <= 8);
    assert(((modulated >> 5) & 0x1F) >= 8 && ((modulated >> 5) & 0x1F) <= 10);
    assert(((modulated >> 10) & 0x1F) >= 10 && ((modulated >> 10) & 0x1F) <= 12);

    // Variable render-rectangle dimensions are hardware-masked to 10-bit
    // width / 9-bit height. Raw 0x401x0x201 must therefore render as 1x1,
    // not be rejected as an oversized primitive.
    gpu.reset();
    gpu.write_gp0(0x600000FFu);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x02010401u); // raw W=0x401 -> 1, raw H=0x201 -> 1
    assert((gpu.peek_vram(0, 0) & 0x7FFFu) == 0x001Fu);

    // CPU->VRAM transfer dimensions use the same 10/9-bit masked COPY rules.
    // A raw height of 0x201 is one row, so the following E6 command must be
    // seen as a new GP0 command rather than swallowed as transfer data.
    gpu.reset();
    gpu.write_gp0(0xA0000000u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x02010001u); // W=1, raw H=0x201 -> effective H=1
    gpu.write_gp0(0x0000001Fu);
    gpu.write_gp0(0xE6000003u);
    assert((gpu.read_gpustat() & (1u << 11)) != 0);
    assert(gpu.peek_vram(0, 0) == 0x001Fu);

    // Indexed texture transparency is based on source palette index 0, even
    // when CLUT[0] contains a non-zero colour. This prevents palette-zero
    // animation/FMV assets from turning into opaque white blocks.
    gpu.reset();
    gpu.vram()[64] = 0x0010u; // 4-bit texels: page x=1, index 0 at U=0, index 1 at U=1
    gpu.vram()[0] = 0x03E0u;  // destination pixel: green
    gpu.vram()[16] = 0x7FFFu; // CLUT[0] deliberately non-zero white
    gpu.vram()[17] = 0x001Fu; // CLUT[1] red
    gpu.write_gp0(0xE1000001u); // 4-bit texture page at (64,0)
    gpu.write_gp0(0x65000000u); // raw textured rectangle
    gpu.write_gp0(0x00000000u); // dst x=0,y=0
    gpu.write_gp0(0x00010000u); // U=0,V=0, CLUT x=1,y=0
    gpu.write_gp0(0x00010001u); // 1x1
    assert(gpu.peek_vram(0, 0) == 0x03E0u); // index 0 skipped
    gpu.write_gp0(0x65000000u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x00010001u); // U=1,V=0, same CLUT x=1
    gpu.write_gp0(0x00010001u);
    assert((gpu.peek_vram(0, 0) & 0x7FFFu) == 0x001Fu); // index 1 drawn

    // GP0(E1).11 is the second bit of texture-page Y addressing and is
    // reflected in GPUSTAT.15. It must never be treated as a texture-disable
    // switch on ordinary retail draw-mode words.
    gpu.reset();
    gpu.write_gp0(0xE1000800u);
    assert((gpu.read_gpustat() & (1u << 15)) != 0);
    assert((gpu.read_gpustat() & (1u << 11)) == 0);
    gpu.write_gp0(0xE1000000u);
    assert((gpu.read_gpustat() & (1u << 15)) == 0);
    gpu.write_gp0(0xE6000003u);
    assert((gpu.read_gpustat() & (1u << 11)) != 0);
    assert((gpu.read_gpustat() & (1u << 12)) != 0);

    // Exact NTSC frame cadence: 564480 master CPU cycles = 263 raster lines.
    gpu.reset();
    gpu.tick(564480);
    assert(gpu.scanline() == 0);
    assert(!gpu.vblank());

    // Interlaced rendering: the PS1 GPU writes only one VRAM parity per field,
    // opposite the currently displayed field unless E1.10 requests drawing to
    // the displayed field. The target parity flips after the next field.
    gpu.reset();
    gpu.write_gp1(0x08000027u); // 640x480 NTSC interlaced, 16bpp
    gpu.write_gp1(0x05000000u); // VRAM display origin (0,0)
    gpu.write_gp0(0x60FF0000u); // red variable rectangle
    gpu.write_gp0(0x00000000u); // x=0,y=0
    gpu.write_gp0(0x00020002u); // 2x2
    assert((gpu.peek_vram(0, 0) & 0x7FFFu) == 0x001Fu); // even row rendered
    assert(gpu.peek_vram(0, 1) == 0); // odd row suppressed
    assert((gpu.read_gpustat() & (1u << 31)) != 0); // displayed field is odd
    gpu.tick(564480); // advance one NTSC field; render parity must flip
    gpu.write_gp0(0x60FF0000u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0(0x00020002u);
    assert((gpu.peek_vram(0, 1) & 0x7FFFu) == 0x001Fu); // odd row now rendered

    // CD-ROM interrupt latch: internal INT flag can be acknowledged independently
    // while system I_STAT remains asserted until explicitly cleared.
    CDROM cd;
    cd.set_irq(&irq);
    irq.acknowledge(0xFFFFFFFFu);
    cd.write8(0x1F801803, 0x1F); // no-op request register on index 0 after reset
    cd.write8(0x1F801800, 1);    // index 1
    cd.write8(0x1F801802, 0x1F); // enable INT1..5
    cd.write8(0x1F801800, 0);    // index 0
    cd.write8(0x1F801801, 0x01); // Getstat
    cd.tick(3000);
    assert((cd.debug_irq_flag() & 7) == 3);
    assert((irq.status() & (1u << InterruptController::CDROM)) != 0);
    cd.write8(0x1F801800, 1);
    cd.write8(0x1F801803, 0x07);
    assert((cd.debug_irq_flag() & 7) == 0);
    assert((irq.status() & (1u << InterruptController::CDROM)) != 0);
    irq.acknowledge(~(1u << InterruptController::CDROM));

    // GTE color pipeline: NCS must perform BOTH light-matrix and color-matrix
    // stages, preserve the RGBC code byte, and write the color FIFO.
    {
        GTE gte;
        const u32 I = 0x00001000u;
        gte.write_control(8, I); gte.write_control(9, 0); gte.write_control(10, I);
        gte.write_control(11, 0); gte.write_control(12, I);
        gte.write_control(16, I); gte.write_control(17, 0); gte.write_control(18, I);
        gte.write_control(19, 0); gte.write_control(20, I);
        gte.write_control(13, 0); gte.write_control(14, 0); gte.write_control(15, 0);
        gte.write_data(0, (2048u << 16) | 2048u); // V0 = (2048, 2048, 2048)
        gte.write_data(1, 2048u);
        gte.write_data(6, 0x7E808080u);    // RGB=128, CODE=0x7E
        gte.execute(0x00080C1Eu);          // NCS, sf=1, lm=1
        assert(static_cast<s16>(gte.read_data(9)) == 2048);
        assert((gte.read_data(22) & 0x00FFFFFFu) == 0x808080u);
        assert(((gte.read_data(22) >> 24) & 0xFFu) == 0x7E);
    }

    // NCCS adds the primary RGBC modulation stage. With IR=4096 and RGBC=255,
    // the output remains at 255 after the <<4/SAR12 sequence.
    {
        GTE gte;
        const u32 I = 0x00001000u;
        gte.write_control(8, I); gte.write_control(9, 0); gte.write_control(10, I);
        gte.write_control(11, 0); gte.write_control(12, I);
        gte.write_control(16, I); gte.write_control(17, 0); gte.write_control(18, I);
        gte.write_control(19, 0); gte.write_control(20, I);
        gte.write_data(0, 0x10001000u); gte.write_data(1, 0x00001000u);
        gte.write_data(6, 0xAFFFFFFFu);
        gte.execute(0x00080C1Bu);          // NCCS
        assert((gte.read_data(22) & 0x00FFFFFFu) == 0xFFFFFFu);
        assert(((gte.read_data(22) >> 24) & 0xFFu) == 0xAF);
    }

    // INTPL uses FC and IR0 with the intermediate difference saturated as if
    // lm=0. This signed path is important for animation/interpolation code.
    {
        GTE gte;
        gte.write_control(21, 8192); gte.write_control(22, 8192); gte.write_control(23, 8192);
        gte.write_data(6, 0x11000000u);
        gte.write_data(8, 2048);             // IR0 = 0.5
        gte.write_data(9, 4096); gte.write_data(10, 4096); gte.write_data(11, 4096);
        gte.execute(0x00080C11u);             // INTPL, sf=1, lm=1
        assert(static_cast<s16>(gte.read_data(9)) == 6144);
        assert((gte.read_data(22) & 0x00FFFFFFu) == 0xFFFFFFu);
        assert(((gte.read_data(22) >> 24) & 0xFFu) == 0x11);
    }

    // RTPS must honor sf and the projection divider. Identity rotation,
    // translated Z, and a simple OFX/OFY setup gives deterministic screen XY.
    {
        GTE gte;
        const u32 I = 0x00001000u;
        gte.write_control(0, I); gte.write_control(1, 0); gte.write_control(2, I);
        gte.write_control(3, 0); gte.write_control(4, I);
        gte.write_control(5, 0); gte.write_control(6, 0); gte.write_control(7, 0);
        gte.write_control(24, 320 << 16); gte.write_control(25, 120 << 16);
        gte.write_control(26, 256); gte.write_control(27, 0); gte.write_control(28, 0);
        gte.write_data(0, (512u << 16) | 1024u); gte.write_data(1, 4096u);
        gte.execute(0x00080C01u); // RTPS, sf=1, lm=1
        const u32 sxy2 = gte.read_data(14);
        assert(static_cast<s16>(sxy2 & 0xFFFFu) == 384);
        assert(static_cast<s16>(sxy2 >> 16) == 152);
        assert((gte.read_data(19) & 0xFFFFu) == 4096u);
    }

    std::puts("PS96 core validation: PASS");
    return 0;
}
