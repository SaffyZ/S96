#pragma once

#include "common/Types.hpp"
#include <array>
#include <functional>

namespace ps96 {

class InterruptController;

class SPU {
public:
    SPU();
    void reset();
    void set_irq(InterruptController* irq) { m_irq = irq; }

    u32 read32(u32 addr);
    void write32(u32 addr, u32 value);
    void dma_write(u32 value);
    u32  dma_read();

    void tick(int cycles);
    void mix(s16* out, int samples);

private:
    enum class AdsrPhase : u8 { Off, Attack, Decay, Sustain, Release };

    struct Voice {
        u16 volume_left = 0;
        u16 volume_right = 0;
        u16 pitch = 0x1000;
        u16 start_addr = 0;
        u32 adsr = 0;
        u16 adsr_vol = 0;
        u16 repeat_addr = 0;
        u32 current_addr = 0;
        bool key_on = false;
        bool key_off = false;
        AdsrPhase phase = AdsrPhase::Off;
        s16 s[2]{};
        u8 header = 0;
        std::array<s16, 28> block{};
        int block_pos = 28;
        f32 phase_frac = 0;
        u32 adsr_cycles = 0;
        u32 samples_played = 0;
    };

    std::array<Voice, 24> m_voices{};
    std::array<u8, SPU_RAM_SIZE> m_ram{};

    u16 m_main_vol_left = 0;
    u16 m_main_vol_right = 0;
    u16 m_reverb_vol_left = 0;
    u16 m_reverb_vol_right = 0;
    u32 m_key_on = 0;
    u32 m_key_off = 0;
    u32 m_endx = 0x00FFFFFF;
    u32 m_pm_on = 0;
    u32 m_noise_on = 0;
    u32 m_reverb_on = 0;
    u16 m_spucnt = 0;
    u16 m_spustat = 0;
    int m_transfer_busy_cycles = 0;
    u16 m_transfer_addr_reg = 0;
    u32 m_transfer_addr = 0;
    u16 m_irq_addr = 0;
    int m_tick_accum = 0;
    static constexpr int kRing = 8192;
    s16 m_audio_ring[kRing]{};
    int m_ring_w = 0;
    int m_ring_r = 0;
    InterruptController* m_irq = nullptr;
    std::array<u16, 0x100> m_regs{};

    void decode_adpcm(Voice& v, int voice_index);
    s16 sample_voice(Voice& v, int voice_index);
    void update_adsr(Voice& v, int voice_index);
    void voice_key_on(int i);
    void voice_key_off(int i);
};

} // namespace ps96
