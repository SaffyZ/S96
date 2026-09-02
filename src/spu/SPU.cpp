#include "spu/SPU.hpp"
#include "emulator/InterruptController.hpp"
#include "common/Log.hpp"
#include <cstring>
#include <algorithm>

namespace ps96 {

static const s16 adpcm_filter[5][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
};

SPU::SPU() { reset(); }

void SPU::reset() {
    m_voices.fill(Voice{});
    m_ram.fill(0);
    m_regs.fill(0);
    m_main_vol_left = m_main_vol_right = 0;
    m_reverb_vol_left = m_reverb_vol_right = 0;
    m_key_on = m_key_off = 0;
    m_endx = 0x00FFFFFF;
    m_pm_on = m_noise_on = m_reverb_on = 0;
    m_spucnt = 0;
    // SPUSTAT bit10 is 1 while a sound-RAM transfer is busy, 0 when ready.
    m_spustat = 0;
    m_transfer_busy_cycles = 0;
    m_transfer_addr = 0;
    m_transfer_addr_reg = 0;
    m_irq_addr = 0;
    m_tick_accum = 0;
}

void SPU::voice_key_on(int i) {
    auto& v = m_voices[i];
    v.key_on = true;
    v.key_off = false;
    v.phase = AdsrPhase::Attack;
    v.current_addr = (u32(v.start_addr) * 8) & (SPU_RAM_SIZE - 1);
    v.block_pos = 28;
    v.phase_frac = 0;
    v.s[0] = v.s[1] = 0;
    v.adsr_vol = 0;
    v.adsr_cycles = 0;
    v.samples_played = 0;
    m_endx &= ~(1u << i);
}

void SPU::voice_key_off(int i) {
    auto& v = m_voices[i];
    v.key_off = true;
    if (!v.key_on || v.adsr_vol == 0 || v.phase == AdsrPhase::Off) {
        v.key_on = false;
        v.phase = AdsrPhase::Off;
        v.adsr_vol = 0;
        m_endx |= (1u << i);
    } else {
        v.phase = AdsrPhase::Release;
        v.adsr_cycles = 0;
    }
}

void SPU::update_adsr(Voice& v, int voice_index) {
    if (v.phase == AdsrPhase::Off) return;

    // Simplified envelope rates derived from ADSR word (nocash layout).
    // Fast enough that logo jingles finish; still shaped like real ADSR.
    const u32 adsr = v.adsr;
    auto rate_steps = [](u32 rate) -> u32 {
        // higher rate nibble = faster; map to samples between steps
        rate &= 0x7F;
        return 1u + ((0x7F - rate) * 4u);
    };

    v.adsr_cycles++;
    switch (v.phase) {
        case AdsrPhase::Attack: {
            u32 ar = (adsr >> 8) & 0x7F;
            if (v.adsr_cycles >= rate_steps(ar)) {
                v.adsr_cycles = 0;
                u32 step = (adsr & (1u << 15)) ? 0x10 : 0x40; // exponential vs linear-ish
                u32 next = u32(v.adsr_vol) + step;
                if (next >= 0x7FFF) {
                    v.adsr_vol = 0x7FFF;
                    v.phase = AdsrPhase::Decay;
                } else {
                    v.adsr_vol = static_cast<u16>(next);
                }
            }
            break;
        }
        case AdsrPhase::Decay: {
            u32 dr = (adsr >> 4) & 0xF;
            u32 sl = adsr & 0xF;
            u32 sustain = (sl * 0x800);
            if (v.adsr_cycles >= rate_steps(dr << 3)) {
                v.adsr_cycles = 0;
                if (v.adsr_vol > sustain + 0x80)
                    v.adsr_vol = static_cast<u16>(v.adsr_vol - 0x80);
                else {
                    v.adsr_vol = static_cast<u16>(sustain);
                    v.phase = AdsrPhase::Sustain;
                }
            }
            break;
        }
        case AdsrPhase::Sustain: {
            // Hold; key_off moves to Release
            if (v.key_off)
                v.phase = AdsrPhase::Release;
            break;
        }
        case AdsrPhase::Release: {
            u32 rr = (adsr >> 16) & 0x1F;
            if (v.adsr_cycles >= rate_steps(rr << 2)) {
                v.adsr_cycles = 0;
                if (v.adsr_vol > 0x200)
                    v.adsr_vol = static_cast<u16>(v.adsr_vol - 0x200);
                else {
                    v.adsr_vol = 0;
                    v.key_on = false;
                    v.key_off = false;
                    v.phase = AdsrPhase::Off;
                    if (voice_index >= 0 && voice_index < 24)
                        m_endx |= (1u << voice_index);
                }
            }
            break;
        }
        default:
            break;
    }
}

u32 SPU::read32(u32 addr) {
    u32 off = addr - 0x1F801C00;

    if (off < 0x180) {
        int voice = static_cast<int>(off / 0x10);
        int reg = static_cast<int>((off % 0x10) / 2);
        auto& v = m_voices[voice];
        auto half = [&](int r) -> u16 {
            switch (r) {
                case 0: return v.volume_left;
                case 1: return v.volume_right;
                case 2: return v.pitch;
                case 3: return v.start_addr;
                case 4: return static_cast<u16>(v.adsr);
                case 5: return static_cast<u16>(v.adsr >> 16);
                case 6: return v.adsr_vol;
                case 7: return v.repeat_addr;
                default: return 0;
            }
        };
        if (reg == 4)
            return v.adsr;
        return u32(half(reg)) | (u32(half(reg + 1)) << 16);
    }

    // Accept both 16-bit and 32-bit addresses for global regs
    const u32 a = addr & ~1u;
    switch (a) {
        case 0x1F801D80: return (u32(m_main_vol_right) << 16) | m_main_vol_left;
        case 0x1F801D84: return (u32(m_reverb_vol_right) << 16) | m_reverb_vol_left;
        case 0x1F801D88: return m_key_on;
        case 0x1F801D8C: return m_key_off;
        case 0x1F801D90: return m_pm_on;
        case 0x1F801D94: return m_noise_on;
        case 0x1F801D98: return m_reverb_on;
        case 0x1F801D9C: return m_endx;
        case 0x1F801DA2: return m_irq_addr;
        case 0x1F801DA6: return m_transfer_addr_reg;
        case 0x1F801DAA: return m_spucnt;
        case 0x1F801DAE:
            return static_cast<u16>((m_spucnt & 0x3F) | (m_transfer_busy_cycles > 0 ? 0x0400 : 0));
        default: {
            if (off < m_regs.size())
                return m_regs[off / 2];
            return 0;
        }
    }
}

void SPU::write32(u32 addr, u32 value) {
    u32 off = addr - 0x1F801C00;

    if (off < 0x180) {
        int voice = static_cast<int>(off / 0x10);
        int reg = static_cast<int>((off % 0x10) / 2);
        auto& v = m_voices[voice];
        auto set_half = [&](int r, u16 val) {
            switch (r) {
                case 0: v.volume_left = val; break;
                case 1: v.volume_right = val; break;
                case 2: v.pitch = val; break;
                case 3: v.start_addr = val; break;
                case 4: v.adsr = (v.adsr & 0xFFFF0000u) | val; break;
                case 5: v.adsr = (v.adsr & 0x0000FFFFu) | (u32(val) << 16); break;
                case 6: v.adsr_vol = val; break;
                case 7: v.repeat_addr = val; break;
                default: break;
            }
        };
        // 16-bit bus writes only carry one halfword (high zeros). Do not
        // wipe the neighbouring register on every 16-bit store.
        set_half(reg, static_cast<u16>(value));
        if ((addr & 3) == 0 && (value >> 16) != 0) {
            if (reg == 4)
                v.adsr = value;
            else
                set_half(reg + 1, static_cast<u16>(value >> 16));
        }
        return;
    }

    switch (addr) {
        case 0x1F801D80:
            m_main_vol_left = static_cast<u16>(value);
            m_main_vol_right = static_cast<u16>(value >> 16);
            return;
        case 0x1F801D84:
            m_reverb_vol_left = static_cast<u16>(value);
            m_reverb_vol_right = static_cast<u16>(value >> 16);
            return;
        case 0x1F801D88: {
            // KEY_ON is a pulse: only bits written as 1 this cycle start voices.
            // Re-writing the latch must not restart already-playing voices.
            u32 bits = (value > 0xFFFF) ? value : (value & 0xFFFF);
            m_key_on = (value > 0xFFFF) ? value : ((m_key_on & 0xFFFF0000u) | bits);
            static int s_log = 0;
            if (s_log < 12 && bits) {
                Log::info("SPU KEY_ON pulse=%08X", bits);
                s_log++;
            }
            for (int i = 0; i < 24; i++)
                if (bits & (1u << i))
                    voice_key_on(i);
            return;
        }
        case 0x1F801D8A: {
            u32 bits = u32(value & 0xFFFF) << 16;
            m_key_on = (m_key_on & 0x0000FFFFu) | bits;
            for (int i = 16; i < 24; i++)
                if (bits & (1u << i))
                    voice_key_on(i);
            return;
        }
        case 0x1F801D8C: {
            u32 bits = (value > 0xFFFF) ? value : (value & 0xFFFF);
            m_key_off = (value > 0xFFFF) ? value : ((m_key_off & 0xFFFF0000u) | bits);
            for (int i = 0; i < 24; i++)
                if (bits & (1u << i))
                    voice_key_off(i);
            return;
        }
        case 0x1F801D8E: {
            u32 bits = u32(value & 0xFFFF) << 16;
            m_key_off = (m_key_off & 0x0000FFFFu) | bits;
            for (int i = 16; i < 24; i++)
                if (bits & (1u << i))
                    voice_key_off(i);
            return;
        }
        case 0x1F801D90: m_pm_on = value; return;
        case 0x1F801D94: m_noise_on = value; return;
        case 0x1F801D98: m_reverb_on = value; return;
        case 0x1F801D9C:
            // ENDX is mostly read-only; writes ignored on hardware
            return;
        case 0x1F801DA2:
            m_irq_addr = static_cast<u16>(value);
            return;
        case 0x1F801DA6:
            m_transfer_addr_reg = static_cast<u16>(value);
            m_transfer_addr = (u32(m_transfer_addr_reg) * 8) & (SPU_RAM_SIZE - 1);
            return;
        case 0x1F801DA8:
            if (m_transfer_addr + 1 < SPU_RAM_SIZE) {
                m_ram[m_transfer_addr]     = static_cast<u8>(value & 0xFF);
                m_ram[m_transfer_addr + 1] = static_cast<u8>((value >> 8) & 0xFF);
                m_transfer_addr = (m_transfer_addr + 2) & (SPU_RAM_SIZE - 1);
                m_transfer_busy_cycles = 16;
            }
            return;
        case 0x1F801DAA:
            m_spucnt = static_cast<u16>(value);
            // Changing transfer mode does not itself mean a transfer is busy.
            // Bit10 only asserts while data is actually moving through the
            // sound-RAM transfer port/DMA.
            if ((value & 0x30) == 0)
                m_transfer_busy_cycles = 0;
            return;
        default:
            if (off < m_regs.size())
                m_regs[off / 2] = static_cast<u16>(value);
            return;
    }
}

void SPU::dma_write(u32 value) {
    write32(0x1F801DA8, value & 0xFFFF);
    write32(0x1F801DA8, value >> 16);
}

u32 SPU::dma_read() {
    if (m_transfer_addr + 1 >= SPU_RAM_SIZE) return 0;
    u32 lo = m_ram[m_transfer_addr];
    u32 hi = m_ram[m_transfer_addr + 1];
    m_transfer_addr = (m_transfer_addr + 2) & (SPU_RAM_SIZE - 1);
    return lo | (hi << 8);
}

void SPU::decode_adpcm(Voice& v, int voice_index) {
    if (v.current_addr + 15 >= SPU_RAM_SIZE) {
        v.key_on = false;
        v.phase = AdsrPhase::Off;
        if (voice_index >= 0 && voice_index < 24)
            m_endx |= (1u << voice_index);
        return;
    }

    u8* src = &m_ram[v.current_addr];
    v.header = src[0];
    int shift = v.header & 0xF;
    int filter = (v.header >> 4) & 7;
    if (filter > 4) filter = 0;
    s16 f0 = adpcm_filter[filter][0];
    s16 f1 = adpcm_filter[filter][1];

    for (int i = 0; i < 28; i++) {
        u8 byte = src[2 + i / 2];
        int nibble = (i & 1) ? (byte >> 4) : (byte & 0xF);
        s32 t = static_cast<s32>(static_cast<s8>(nibble << 4) >> 4);
        t <<= (12 - shift);
        t += (static_cast<s32>(v.s[0]) * f0 + static_cast<s32>(v.s[1]) * f1) / 64;
        t = std::clamp(t, s32(-32768), s32(32767));
        v.block[i] = static_cast<s16>(t);
        v.s[1] = v.s[0];
        v.s[0] = static_cast<s16>(t);
    }
    v.block_pos = 0;

    u8 flags = src[1];
    if (flags & 1) {
        if (flags & 2) {
            v.current_addr = (u32(v.repeat_addr) * 8) & (SPU_RAM_SIZE - 1);
        } else {
            // End of sound data — stop and signal ENDX (BIOS logo waits on this)
            v.key_on = false;
            v.phase = AdsrPhase::Off;
            v.adsr_vol = 0;
            if (voice_index >= 0 && voice_index < 24)
                m_endx |= (1u << voice_index);
        }
    } else {
        v.current_addr = (v.current_addr + 16) & (SPU_RAM_SIZE - 1);
    }
    if (flags & 4)
        v.current_addr = (u32(v.repeat_addr) * 8) & (SPU_RAM_SIZE - 1);
}

s16 SPU::sample_voice(Voice& v, int voice_index) {
    if (!v.key_on && v.phase == AdsrPhase::Off)
        return 0;

    update_adsr(v, voice_index);

    if (!v.key_on && v.phase == AdsrPhase::Off)
        return 0;

    if (v.block_pos >= 28)
        decode_adpcm(v, voice_index);
    if ((!v.key_on && v.phase == AdsrPhase::Off) || v.block_pos >= 28)
        return 0;

    s16 s = v.block[v.block_pos];
    // Apply ADSR volume
    s = static_cast<s16>((s32(s) * s32(v.adsr_vol)) >> 15);

    // Safety: if sample stream never sets the ADPCM end flag (bad dump / silence),
    // still raise ENDX after ~1 second so BIOS logo waits cannot softlock.
    v.samples_played++;
    if (v.samples_played > 11025) {
        v.key_on = false;
        v.phase = AdsrPhase::Off;
        v.adsr_vol = 0;
        if (voice_index >= 0 && voice_index < 24)
            m_endx |= (1u << voice_index);
        return 0;
    }

    f32 step = static_cast<f32>(v.pitch ? v.pitch : 0x1000) / 4096.0f;
    v.phase_frac += step;
    while (v.phase_frac >= 1.0f) {
        v.phase_frac -= 1.0f;
        v.block_pos++;
        if (v.block_pos >= 28) {
            decode_adpcm(v, voice_index);
            if (!v.key_on && v.phase == AdsrPhase::Off)
                break;
        }
    }
    return s;
}

void SPU::tick(int cycles) {
    if (cycles <= 0) return;
    if (m_transfer_busy_cycles > 0) {
        m_transfer_busy_cycles -= cycles;
        if (m_transfer_busy_cycles < 0) m_transfer_busy_cycles = 0;
    }
    m_tick_accum += cycles;
    // PS1 SPU: 33868800 Hz master / 44100 Hz output = 768 cycles/sample
    constexpr int cpu_per_sample = 768;
    const bool enabled = (m_spucnt & 0x8000) != 0;
    const s32 ml = static_cast<s32>(m_main_vol_left & 0x7FFF);
    const s32 mr = static_cast<s32>(m_main_vol_right & 0x7FFF);
    while (m_tick_accum >= cpu_per_sample) {
        m_tick_accum -= cpu_per_sample;
        s32 left = 0, right = 0;
        if (enabled) {
            for (int vi = 0; vi < 24; vi++) {
                s16 s = sample_voice(m_voices[vi], vi);
                if (s == 0) continue;
                s32 vl = static_cast<s32>(m_voices[vi].volume_left & 0x7FFF);
                s32 vr = static_cast<s32>(m_voices[vi].volume_right & 0x7FFF);
                left  += (s32(s) * vl) >> 15;
                right += (s32(s) * vr) >> 15;
            }
            left  = (left  * ml) >> 15;
            right = (right * mr) >> 15;
        }
        auto clamp16 = [](s32 v) -> s16 {
            return static_cast<s16>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
        };
        // Stereo pair into ring (drop if full)
        int next = (m_ring_w + 2) % kRing;
        if (next != m_ring_r && (next + 1) % kRing != m_ring_r) {
            m_audio_ring[m_ring_w] = clamp16(left);
            m_ring_w = (m_ring_w + 1) % kRing;
            m_audio_ring[m_ring_w] = clamp16(right);
            m_ring_w = (m_ring_w + 1) % kRing;
        }
    }
}

void SPU::mix(s16* out, int samples) {
    // Drain samples produced by tick() — keeps audio locked to CPU cycle time
    for (int i = 0; i < samples; i++) {
        s16 l = 0, r = 0;
        if (m_ring_r != m_ring_w) {
            l = m_audio_ring[m_ring_r];
            m_ring_r = (m_ring_r + 1) % kRing;
            if (m_ring_r != m_ring_w) {
                r = m_audio_ring[m_ring_r];
                m_ring_r = (m_ring_r + 1) % kRing;
            }
        }
        out[i * 2]     = l;
        out[i * 2 + 1] = r;
    }
}

} // namespace ps96
