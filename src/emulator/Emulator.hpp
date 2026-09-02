#pragma once

#include "common/Types.hpp"
#include "cpu/R3000A.hpp"
#include "memory/Bus.hpp"
#include "gpu/GPU.hpp"
#include "gte/GTE.hpp"
#include "dma/DMA.hpp"
#include "cdrom/CDROM.hpp"
#include "cdrom/Disc.hpp"
#include "spu/SPU.hpp"
#include "mdec/MDEC.hpp"
#include "timers/Timers.hpp"
#include "controllers/Controllers.hpp"
#include "memorycard/MemoryCard.hpp"
#include "bios/BIOS.hpp"
#include "emulator/InterruptController.hpp"
#include "emulator/Scheduler.hpp"
#include <string>
#include <atomic>
#include <functional>

namespace ps96 {

class Emulator {
public:
    Emulator();
    ~Emulator() = default;

    bool load_bios(const std::string& path);
    bool load_disc(const std::string& path);
    bool load_memory_card(int slot, const std::string& path);

    bool bios_loaded() const { return m_bios.is_loaded(); }
    bool disc_loaded() const { return m_disc.is_loaded(); }
    const BIOS& bios() const { return m_bios; }

    void reset();
    // Load PS-EXE from disc via SYSTEM.CNF and jump to it (skips BIOS intro).
    bool fast_boot_disc();
    // Advance the emulated machine by an exact number of PS1 CPU cycles.
    void run_cycles(s64 cycles);
    void run_frame();
    void step_instruction();

    void set_paused(bool p) { m_paused = p; }
    bool paused() const { return m_paused; }

    R3000A& cpu() { return m_cpu; }
    GPU& gpu() { return m_gpu; }
    SPU& spu() { return m_spu; }
    Controllers& controllers() { return m_controllers; }
    Bus& bus() { return m_bus; }
    Disc& disc() { return m_disc; }
    DMA& dma() { return m_dma; }
    Timers& timers() { return m_timers; }
    CDROM& cdrom() { return m_cdrom; }

    u64 frame_count() const { return m_frame_count; }

    // Debug-only: fired with (calling PC, cmd byte) whenever the GPU dispatches
    // a GP0 draw/upload command, so a trace tool can correlate BIOS code with
    // GPU activity without duplicating run_frame's timing loop.
    void set_gp0_trace(std::function<void(u32, u32)> cb) { m_gp0_trace = std::move(cb); }

private:
    R3000A m_cpu;
    Bus m_bus;
    GPU m_gpu;
    GTE m_gte;
    DMA m_dma;
    CDROM m_cdrom;
    Disc m_disc;
    SPU m_spu;
    MDEC m_mdec;
    Timers m_timers;
    Controllers m_controllers;
    MemoryCard m_memcards[2];
    BIOS m_bios;
    InterruptController m_irq;
    Scheduler m_scheduler;

    bool m_paused = false;
    u64 m_frame_count = 0;
    bool m_prev_vblank = false;
    std::function<void(u32, u32)> m_gp0_trace;

    void connect();
};

} // namespace ps96
