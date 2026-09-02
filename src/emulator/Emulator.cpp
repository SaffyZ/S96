#include "emulator/Emulator.hpp"
#include "common/Log.hpp"
#include <cstring>
#include <cctype>
#include <vector>

namespace ps96 {

Emulator::Emulator() {
    connect();
    reset();
}

void Emulator::connect() {
    m_cpu.set_bus(&m_bus);
    m_cpu.set_gte(&m_gte);
    m_bus.set_gpu(&m_gpu);
    m_bus.set_dma(&m_dma);
    m_bus.set_cdrom(&m_cdrom);
    m_bus.set_spu(&m_spu);
    m_bus.set_mdec(&m_mdec);
    m_bus.set_timers(&m_timers);
    m_bus.set_controllers(&m_controllers);
    m_bus.set_irq(&m_irq);
    m_dma.set_bus(&m_bus);
    m_dma.set_gpu(&m_gpu);
    m_dma.set_cdrom(&m_cdrom);
    m_dma.set_spu(&m_spu);
    m_dma.set_mdec(&m_mdec);
    m_dma.set_irq(&m_irq);
    m_cdrom.set_irq(&m_irq);
    m_cdrom.set_disc(&m_disc);
    m_spu.set_irq(&m_irq);
    m_timers.set_irq(&m_irq);
    m_controllers.set_irq(&m_irq);
    m_controllers.set_memcard(0, &m_memcards[0]);
    m_controllers.set_memcard(1, &m_memcards[1]);
    m_irq.set_cpu(&m_cpu);
    m_irq.set_bus(&m_bus);
    // GPU IRQ1 → I_STAT bit1
    m_gpu.set_irq_callback([this]() {
        m_irq.raise(InterruptController::GPU);
    });
    // GP1(02) acknowledges the GPU's internal IRQ request.  I_STAT bit1 is a
    // separate system latch and is cleared only by the interrupt controller.
    m_gpu.set_irq_ack_callback([]() {});
}

bool Emulator::load_bios(const std::string& path) {
    if (!m_bios.load(path)) return false;
    m_bus.load_bios(m_bios.data(), m_bios.size());
    return true;
}

bool Emulator::load_disc(const std::string& path) {
    bool ok = false;
    // Extension check must be fully case-insensitive — Windows filenames can
    // legally be "Game.Cue", "GAME.CUE", etc. The previous check only
    // matched the two exact-case spellings ".cue"/".CUE" and silently fell
    // through to load_bin() (treating the CUE text sheet as a raw binary
    // image) for any other casing, which would fail to boot the disc at
    // all with no obvious error.
    std::string lower_path = path;
    for (char& c : lower_path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower_path.size() > 4 && lower_path.compare(lower_path.size() - 4, 4, ".cue") == 0) {
        ok = m_disc.load_cue(path);
    } else {
        ok = m_disc.load_bin(path);
    }
    // Keep CD-ROM drive status in sync (shell closed / disc present) immediately
    // after a successful load — BIOS Getstat must not still see shell-open.
    m_cdrom.set_disc(&m_disc);
    m_cdrom.notify_disc_changed();
    return ok;
}

bool Emulator::load_memory_card(int slot, const std::string& path) {
    if (slot < 0 || slot > 1) return false;
    return m_memcards[slot].load(path);
}

void Emulator::reset() {
    m_cpu.reset();
    m_bus.reset();
    m_gpu.reset();
    m_gte.reset();
    m_dma.reset();
    m_cdrom.reset();
    m_spu.reset();
    m_mdec.reset();
    m_timers.reset();
    m_controllers.reset();
    m_irq.reset();
    m_scheduler.reset();
    m_frame_count = 0;
    m_prev_vblank = false;
    if (m_bios.is_loaded()) {
        m_bus.load_bios(m_bios.data(), m_bios.size());
    }
    m_cdrom.set_disc(&m_disc);
    m_cpu.set_pc(0xBFC00000);
    Log::info("Emulator reset - PC=%08X disc=%s", m_cpu.pc(),
              m_disc.is_loaded() ? m_disc.path().c_str() : "(none)");
}

void Emulator::step_instruction() {
    m_cpu.step();
}

void Emulator::run_cycles(s64 cycles) {
    if (cycles <= 0 || m_paused) return;

    // All emulated devices consume the same master-clock time as the CPU.
    // Keep the interpreter chunk bounded for safety. The cycle-driven devices
    // are serviced in a four-cycle master-clock quantum below; this is far
    // finer than the controller /ACK pulse and video timing scale, but avoids
    // paying a peripheral function-call/loop cost after every R3000 instruction.
    constexpr s64 kMaxCatchupCycles = 2 * 33'868'800;
    cycles = std::min(cycles, kMaxCatchupCycles);

    s64 executed = 0;
    s64 device_cycles = 0;
    // Device timing is still expressed in exact master CPU cycles. The devices
    // themselves preserve sub-quantum event timing internally, so servicing them
    // every 32 CPU cycles avoids a huge amount of call overhead without making
    // controller ACKs, timers, CD-ROM events, or GPU raster timing host-frame
    // dependent. This materially improves animation throughput on the software
    // renderer while preserving the same guest clock.
    constexpr s64 kDeviceQuantum = 8;

    auto service_devices = [&](s64 elapsed) {
        if (elapsed <= 0) return;
        const int c = static_cast<int>(elapsed);
        m_gpu.tick(c);
        m_timers.tick(c);
        m_cdrom.tick(c);
        m_spu.tick(c);
        m_controllers.tick(c);
        m_dma.tick();

        const bool vblank_now = m_gpu.vblank();
        if (vblank_now && !m_prev_vblank) {
            m_irq.raise(InterruptController::VBLANK);
            static int s_vb = 0;
            if (m_frame_count >= 350 && s_vb < 15) {
                Log::info("VBLANK_RAISE frame=%llu scanline=%d I_STAT=%03X I_MASK=%03X",
                    (unsigned long long)m_frame_count, m_gpu.scanline(),
                    m_irq.status() & 0x7FF, m_irq.mask() & 0x7FF);
                ++s_vb;
            }
        }
        if (!m_prev_vblank && vblank_now)
            ++m_frame_count;
        m_prev_vblank = vblank_now;
    };

    while (executed < cycles) {
        int c = m_cpu.step();
        if (c < 1) c = 1;
        executed += c;
        device_cycles += c;
        if (device_cycles >= kDeviceQuantum) {
            service_devices(device_cycles);
            device_cycles = 0;
        }
    }
    service_devices(device_cycles);
}

void Emulator::run_frame() {
    // Advance until the next actual emulated VBlank edge. This keeps frame
    // presentation coupled to the GPU raster state instead of assuming the
    // caller always enters at scanline zero. NTSC/PAL cadence remains owned by
    // GPU::tick(), so the same API works for both regions.
    const u64 target = m_frame_count + 1;
    while (m_frame_count < target && !m_paused) {
        // A small slice keeps interrupt/raster/device scheduling responsive.
        run_cycles(4096);
    }
}



bool Emulator::fast_boot_disc() {
    if (!m_disc.is_loaded()) {
        Log::error("fast_boot: no disc loaded");
        return false;
    }

    // Approach: run real BIOS until A0/B0/C0 + RAM exception vector are
    // installed, then overlay the PS-EXE and jump. Do NOT replace A0 with
    // fake trampolines (that stuck PC at 0x500).

    m_cpu.reset();
    m_bus.reset();
    m_gpu.reset();
    m_dma.reset();
    m_cdrom.reset();
    m_spu.reset();
    m_timers.reset();
    m_irq.reset();
    if (m_bios.is_loaded())
        m_bus.load_bios(m_bios.data(), m_bios.size());
    m_cdrom.set_disc(&m_disc);
    m_cdrom.notify_disc_changed();
    m_cpu.set_pc(0xBFC00000);

    auto tick_chunk = [&](int tt) {
        const bool was_blank = m_gpu.vblank();
        m_gpu.tick(tt);
        m_timers.tick(tt);
        m_cdrom.tick(tt);
        m_spu.tick(tt);
        m_dma.tick();
        if (m_gpu.vblank() && !was_blank)
            m_irq.raise(InterruptController::VBLANK);
    };

    auto kernel_ready = [&]() -> bool {
        auto is_kernel_vector = [&](u32 base) {
            u32 w0 = m_bus.read32(base);
            u32 w2 = m_bus.read32(base + 8);
            if ((w0 & 0xFFFF0000u) != 0x3C080000u) return false;
            if (w2 != 0x01000008u) return false;
            return true;
        };
        if (!is_kernel_vector(0xA0) || !is_kernel_vector(0xB0) || !is_kernel_vector(0xC0))
            return false;
        u32 v0 = m_bus.read32(0x80000080);
        if (v0 == 0 || v0 == 0xFFFFFFFFu) return false;
        // Table pointer from A0 stub addiu should be non-trivial
        u32 a4 = m_bus.read32(0xA4); // addiu t0, t0, imm
        u16 imm = u16(a4 & 0xFFFF);
        if (imm == 0) return false;
        return true;
    };

    bool ready = false;
    int accum = 0;
    constexpr int kMaxSteps = 15'000'000;
    for (int i = 0; i < kMaxSteps; i++) {
        int c = m_cpu.step();
        if (c < 1) c = 1;
        accum += c;
        if (accum >= 64) {
            tick_chunk(accum);
            accum = 0;
        }
        if ((i & 0x7FFF) == 0 && kernel_ready()) {
            ready = true;
            for (int j = 0; j < 200'000; j++) {
                c = m_cpu.step();
                if (c < 1) c = 1;
                accum += c;
                if (accum >= 64) { tick_chunk(accum); accum = 0; }
            }
            break;
        }
    }
    if (accum > 0) tick_chunk(accum);

    if (!ready) {
        Log::warn("fast_boot: kernel not fully ready (A0=%08X vec=%08X) — still loading EXE",
                  m_bus.read32(0xA0), m_bus.read32(0x80000080));
    } else {
        Log::info("fast_boot: kernel ready A0=%08X B0=%08X vec80=%08X",
                  m_bus.read32(0xA0), m_bus.read32(0xB0), m_bus.read32(0x80000080));
    }

    std::string boot;
    if (!m_disc.read_system_cnf_boot(boot)) {
        Log::error("fast_boot: no SYSTEM.CNF BOOT");
        return false;
    }
    DiscFile exe;
    if (!m_disc.find_file(boot, exe)) {
        Log::error("fast_boot: EXE not found: %s", boot.c_str());
        return false;
    }
    std::vector<u8> data;
    if (!m_disc.read_file(exe, data) || data.size() < 0x800) {
        Log::error("fast_boot: EXE read failed (%zu)", data.size());
        return false;
    }
    if (std::memcmp(data.data(), "PS-X EXE", 8) != 0) {
        Log::error("fast_boot: bad PS-X EXE magic");
        return false;
    }
    auto le32 = [&](u32 off) -> u32 {
        return u32(data[off]) | (u32(data[off+1]) << 8) |
               (u32(data[off+2]) << 16) | (u32(data[off+3]) << 24);
    };
    u32 pc   = le32(0x10);
    u32 gp   = le32(0x14);
    u32 dest = le32(0x18);
    u32 size = le32(0x1C);
    u32 sp   = le32(0x30);
    if (size == 0 || size > data.size() - 0x800)
        size = u32(data.size() - 0x800);
    if (sp == 0) sp = 0x801FFF00;

    // Overlay EXE — do not wipe kernel / A0 tables
    for (u32 i = 0; i < size; i++)
        m_bus.write8(dest + i, data[0x800 + i]);

    // Drop any pending IRQs from the bootstrap (especially timers)
    m_timers.reset();
    m_irq.reset();
    m_cpu.cop0().clear_cause_ip(0xFF);

    // Seed GPRs like LoadExe — keep r0=0, set gp/sp
    for (int i = 1; i < 32; i++)
        m_cpu.reg(i) = 0;
    m_cpu.reg(28) = gp;
    m_cpu.reg(29) = sp;
    m_cpu.reg(30) = sp;
    m_cpu.reg(31) = 0;

    // BEV=0, IEc=1, IM2 enabled for hardware IRQ line
    m_cpu.cop0().write(12, 0x00000401u);
    m_cpu.set_pc(pc);
    m_frame_count = 0;
    m_cdrom.set_disc(&m_disc);
    m_cdrom.notify_disc_changed();

    Log::info("fast_boot(v3): %s → PC=%08X GP=%08X SP=%08X DEST=%08X SIZE=%08X",
              boot.c_str(), pc, gp, sp, dest, size);
    return true;
}


} // namespace ps96
