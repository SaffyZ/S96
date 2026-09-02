#include "cdrom/CDROM.hpp"
#include "emulator/InterruptController.hpp"
#include "common/Log.hpp"
#include <vector>
#include <algorithm>
#include <cstring>

namespace ps96 {

CDROM::CDROM() { reset(); }

void CDROM::set_disc(Disc* disc) {
    m_disc = disc;
    update_status_shell();
}

void CDROM::reset() {
    m_index = 0;
    m_status = 0x10; // shell open until disc bound
    m_response.clear();
    m_param.clear();
    m_irq_enable = 0;
    m_irq_flag = 0;
    m_request = 0;
    m_data_ready = false;
    m_sector_pos = 0;
    m_seek_lba = 0;
    m_read_lba = 0;
    m_reading = false;
    m_seeking = false;
    m_read_delay = 0;
    m_seek_delay = 0;
    m_mode = 0x20;
    m_muted = true;
    m_pending_first = false;
    m_first_delay = 0;
    m_first_response.clear();
    m_pending_second = false;
    m_second_delay = 0;
    m_second_response.clear();
    m_second_irq = 0;
    update_status_shell();
}

void CDROM::update_status_shell() {
    if (m_disc && m_disc->is_loaded()) {
        // Disc inserted: shell closed. Keep motor spinning so Getstat after the
        // SCE logo looks like a ready drive (BIOS skips long MotorOn waits).
        m_status &= ~0x10u;
        m_status &= ~0x08u;
        m_status |= 0x02u; // motor on
    } else {
        // No disc: shell open, motor off, not reading/seeking
        m_status = (m_status & ~0x62u) | 0x10u;
    }
}

void CDROM::push_response(u8 v) {
    m_response.push_back(v);
}

void CDROM::raise_irq(u8 which) {
    // Hardware: bits 0-2 of the interrupt flag register hold the INT *number*
    // (1..7), not a bitfield. Bits 3-4 are additional flags. The shell IRQ
    // dispatcher does (flag & 7) and indexes a jump table by that number.
    // Storing a shifted bit (1<<(n-1)) made INT3 appear as value 4 → wrong
    // handler → no DeliverEvent → TestEvent never fires → state machine stuck.
    if (which >= 1 && which <= 7)
        m_irq_flag = u8((m_irq_flag & ~0x07u) | (which & 0x07u));
    {
        static int s_ri = 0;
        if (s_ri < 40) {
            Log::info("CDROM_IRQ_RAISE INT%d flag=%02X en=%02X", which, m_irq_flag, m_irq_enable);
            s_ri++;
        }
    }
    // I_STAT CDROM is asserted only when the specific pending interrupt
    // number is enabled. The previous implementation tested merely whether
    // *any* enable bit was set, which could raise I_STAT for an unrelated INT.
    const u8 inum = m_irq_flag & 0x07;
    const bool enabled = inum >= 1 && inum <= 5 &&
                         (m_irq_enable & u8(1u << (inum - 1))) != 0;
    if (enabled && m_irq)
        m_irq->raise(InterruptController::CDROM);
    // I_STAT bit2 is a system interrupt latch.  Clearing the CD controller's
    // internal INT number must not deassert that system latch; software clears
    // it explicitly through InterruptController::acknowledge().
}

void CDROM::schedule_first(u8 irq_bits, std::initializer_list<u8> bytes, int delay_cycles) {
    m_pending_first = true;
    m_first_delay = delay_cycles;
    m_first_irq = irq_bits;
    m_first_response.assign(bytes);
}

void CDROM::schedule_second(u8 irq_bits, std::initializer_list<u8> bytes, int delay_cycles) {
    m_pending_second = true;
    m_second_delay = delay_cycles;
    m_second_irq = irq_bits;
    m_second_response.assign(bytes);
}

void CDROM::load_sector_data() {
    if (!m_disc || !m_disc->is_loaded()) return;
    m_disc->read_sector(m_read_lba, m_sector.data());
    // Data FIFO start depends on BOTH the drive mode bit and the sector format:
    //   mode.bit5=1 (0x924 size): skip sync only → start at 12
    //   mode.bit5=0 (0x800 size) + MODE1 sector: header ends at 16
    //   mode.bit5=0 (0x800 size) + MODE2 sector: subheader ends at 24
    // FIFA and most PS1 titles are MODE2/2352; starting at 16 was feeding the
    // 8-byte XA subheader into the ISO9660 parser and every subsequent read.
    if (m_mode & 0x20) {
        m_sector_pos = 12;
    } else {
        u8 sec_mode = m_sector[15];
        m_sector_pos = (sec_mode == 2) ? 24 : 16;
    }
    m_data_ready = true;
    m_status = (m_status & ~0x10) | 0x22; // motor + reading
    {
        static int s_ls = 0;
        if (s_ls < 6) {
            Log::info("LOAD_SECTOR lba=%u pos=%d secmode=%u drvmode=%02X data0=%02X%02X%02X%02X",
                m_read_lba, m_sector_pos, m_sector[15], m_mode,
                m_sector[m_sector_pos], m_sector[m_sector_pos+1],
                m_sector[m_sector_pos+2], m_sector[m_sector_pos+3]);
            s_ls++;
        }
    }
}

u8 CDROM::read8(u32 addr) {
    u8 reg = addr & 3;
    switch (reg) {
        case 0: { // status — nocash 1F801800
            // bit0-1 index, 3 param empty, 4 param not full,
            // 5 response not empty, 6 data not empty, 7 busy
            u8 s = m_index & 3;
            if (m_param.empty()) s |= 0x08;
            if (m_param.size() < 16) s |= 0x10;
            if (!m_response.empty()) s |= 0x20;
            if (m_data_ready) s |= 0x40;
            // Busy only while a command is actively working — not after INT3
            // has already been posted with a finished response.
            if (m_seeking || (m_reading && m_read_delay > 0) || m_pending_second)
                s |= 0x80;
            return s;
        }
        case 1: { // response FIFO
            if (m_response.empty()) return 0;
            u8 v = m_response.front();
            m_response.pop_front();
            return v;
        }
        case 2: {
            // data register (CPU path). BFRD is advisory for DMA; CPU reads
            // work whenever a sector is ready.
            if (m_data_ready && m_sector_pos < 2352) {
                static int s_drd = 0;
                u8 v = m_sector[m_sector_pos++];
                if (s_drd < 5) {
                    Log::info("CD_DATA_READ pos=%d v=%02X lba=%d", m_sector_pos-1, v, m_read_lba);
                    s_drd++;
                }
                return v;
            }
            return 0;
        }
        case 3: {
            if (m_index == 0 || m_index == 2) return m_irq_enable | 0xE0;
            if (m_index == 1 || m_index == 3) return m_irq_flag | 0xE0;
            return 0;
        }
    }
    return 0;
}

void CDROM::write8(u32 addr, u8 value) {
    u8 reg = addr & 3;
    switch (reg) {
        case 0:
            m_index = value & 3;
            break;
        case 1:
            if (m_index == 0) {
                exec_command(value);
            }
            break;
        case 2:
            if (m_index == 0) {
                if (m_param.size() < 16)
                    m_param.push_back(value);
            } else if (m_index == 1) {
                m_irq_enable = value & 0x1F;
                // Re-evaluate: flag bits 0-2 = INT number; enable is bitfield
                u8 inum = m_irq_flag & 0x07;
                bool pending = inum >= 1 && inum <= 5 && (m_irq_enable & u8(1u << (inum - 1)));
                if (pending && m_irq)
                    m_irq->raise(InterruptController::CDROM);
            }
            break;
        case 3:
            if (m_index == 0) {
                // Request register (nocash):
                //   bit7 BFRD = Want Data Read. 1 = load/keep data FIFO for
                //   reading; 0 = reset data FIFO. Must NOT clear the parameter
                //   FIFO (that was a prior mix-up with another port).
                u8 prev = m_request;
                m_request = value;
                if ((value & 0x80) == 0) {
                    // BFRD cleared → reset data FIFO position for next sector
                    // (sector buffer itself stays until next load_sector_data)
                    if (prev & 0x80) {
                        // end of this sector's delivery
                    }
                }
            } else if (m_index == 1) {
                // Acknowledge: write-1s clear the corresponding bits.
                // Bits 0-2 hold INT number — writing 0x07 clears any INT1-7.
                m_irq_flag &= u8(~(value & 0x1F));
                // The CD controller's internal interrupt number is cleared by
                // this acknowledge, but I_STAT is a separate system latch and
                // remains asserted until software clears I_STAT bit2.  Do not
                // destroy unread response bytes here; response FIFO lifetime is
                // independent from the interrupt acknowledge.
            }
            break;
    }
}

u32 CDROM::read32(u32 addr) {
    // Only low byte is meaningful for most ports; do a single read8
    return read8(addr);
}

void CDROM::write32(u32 addr, u32 value) {
    // CDROM is an 8-bit peripheral — only the low byte counts
    write8(addr, static_cast<u8>(value));
}

u32 CDROM::dma_read() {
    if (!m_data_ready) {
        static int s_nr = 0;
        if (s_nr < 3) { Log::info("CD_DMA_READ not ready pos=%d", m_sector_pos); s_nr++; }
        return 0;
    }
    u32 word = 0;
    for (int i = 0; i < 4; i++) {
        if (m_sector_pos < 2352)
            word |= u32(m_sector[m_sector_pos++]) << (8 * i);
    }
    static int s_dr = 0;
    if (s_dr < 5) {
        Log::info("CD_DMA_READ word=%08X pos=%d lba=%d", word, m_sector_pos, m_read_lba);
        s_dr++;
    }
    // End of the 0x800 (or 0x924) delivery window
    int end;
    if (m_mode & 0x20)
        end = 12 + 0x924;
    else
        end = ((m_sector[15] == 2) ? 24 : 16) + 0x800;
    if (m_sector_pos >= end)
        m_data_ready = false;
    return word;
}

int CDROM::sector_period_cycles() const {
    // PS1 CD-ROM transfer timing is expressed in sectors per second. Normal
    // speed is 75 sectors/s; Setmode bit7 selects 2x (150 sectors/s).
    // Convert that interval into PS1 master-clock cycles.
    constexpr int MASTER_CLOCK = 33868800;
    constexpr int NORMAL_SECTORS_PER_SECOND = 75;
    const int sectors_per_second = (m_mode & 0x80)
        ? NORMAL_SECTORS_PER_SECOND * 2
        : NORMAL_SECTORS_PER_SECOND;
    return MASTER_CLOCK / sectors_per_second;
}

void CDROM::tick(int cycles) {
    if (m_pending_first) {
        m_first_delay -= cycles;
        if (m_first_delay <= 0) {
            m_response.clear();
            for (u8 b : m_first_response)
                m_response.push_back(b);
            m_first_response.clear();
            m_pending_first = false;
            raise_irq(m_first_irq);
        }
    }
    if (m_pending_second) {
        m_second_delay -= cycles;
        if (m_second_delay <= 0) {
            m_response.clear();
            for (u8 b : m_second_response)
                m_response.push_back(b);
            m_second_response.clear();
            m_pending_second = false;
            raise_irq(m_second_irq);
        }
    }

    if (m_seeking) {
        m_seek_delay -= cycles;
        if (m_seek_delay <= 0) {
            m_seeking = false;
            m_read_lba = m_seek_lba;
            update_status_shell();
            m_status &= ~0x40;
            m_response.clear();
            push_response(m_status);
            raise_irq(2); // INT2 complete
        }
    }

    if (m_reading) {
        // Hardware does not post the next INT1 until the previous data IRQ
        // has been acknowledged. Without this the BIOS misses sectors and
        // the license / SYSTEM.CNF read fails.
        // Wait only while INT1 (number==1) is still latched unacked.
        // With number encoding, INT3 (flag=3) must NOT block INT1 delivery.
        if ((m_irq_flag & 0x07) == 1) {
            // Still waiting for ack of previous INT1 — do not advance.
        } else if (m_read_delay > 0) {
            m_read_delay -= cycles;
            if (m_read_delay <= 0) {
                load_sector_data();
                m_response.clear();
                push_response(m_status);
                raise_irq(1); // INT1 data ready
                m_read_lba++;
                // ~75 sectors/sec at 1x; slightly faster for emulator throughput
                m_read_delay = sector_period_cycles();
            }
        } else {
            // delay already 0 and no pending INT1 — deliver immediately
            load_sector_data();
            m_response.clear();
            push_response(m_status);
            raise_irq(1);
            m_read_lba++;
            m_read_delay = sector_period_cycles();
        }
    }
}

void CDROM::exec_command(u8 cmd) {
    static int s_cmd_log = 0;
    m_response.clear();
    update_status_shell();
    if (s_cmd_log < 32) {
        Log::info("CDROM cmd=%02X params=%zu disc=%d status=%02X irq_en=%02X",
            cmd, m_param.size(),
            (m_disc && m_disc->is_loaded()) ? 1 : 0,
            m_status, m_irq_enable);
        s_cmd_log++;
    }

    switch (cmd) {
        case 0x01: // Getstat
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x02: { // Setloc
            if (m_param.size() >= 3) {
                auto bcd = [](u8 v) { return (v >> 4) * 10 + (v & 0xF); };
                u8 mm = bcd(m_param[0]), ss = bcd(m_param[1]), ff = bcd(m_param[2]);
                m_seek_lba = (u32(mm) * 60u + ss) * 75u + ff;
                if (m_seek_lba >= 150) m_seek_lba -= 150;
                else m_seek_lba = 0;
            }
            m_param.clear();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;
        }

        case 0x06: // ReadN
        case 0x1B: // ReadS
            m_reading = true;
            m_seeking = false;
            m_read_lba = m_seek_lba;
            // INT3 (command ack) must be delivered and acked before the first
            // INT1 data sector. Start the data timer after the INT3 latency.
            m_read_delay = 2500 + 3000;
            m_status = (m_status & ~0x10u) | 0x22u; // motor + reading
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x07: { // MotorOn — INT3(stat), then INT2(stat) once spindle is up
            update_status_shell();
            // Motor can spin with lid open; shell bit stays accurate
            m_status |= 0x02;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            schedule_second(2, {m_status}, 5000);
            break;
        }

        case 0x08: // Stop
            m_reading = false;
            m_seeking = false;
            m_status &= ~0x62u; // clear motor/read/seek
            update_status_shell();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            schedule_second(2, {m_status}, 5000);
            break;

        case 0x09: // Pause
            m_reading = false;
            m_status &= ~0x20;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            schedule_second(2, {m_status}, 5000);
            break;

        case 0x0A: { // Init — mode reset; INT3 then INT2. Shell follows disc presence.
            m_reading = false;
            m_seeking = false;
            m_mode = 0x20;
            m_param.clear();
            update_status_shell();
            if (m_disc && m_disc->is_loaded())
                m_status = (m_status & ~0x10u) | 0x02u; // closed + motor on
            // no disc: leave shell-open from update_status_shell
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            schedule_second(2, {m_status}, 5000);
            break;
        }

        case 0x0B: // Mute
            m_muted = true;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x0C: // Demute
            m_muted = false;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x0D: // Setfilter
            m_param.clear();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x0E: // Setmode
            if (!m_param.empty()) m_mode = m_param[0];
            m_param.clear();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x0F: // Getparam
            push_response(m_status);
            push_response(m_mode);
            push_response(0);
            push_response(0);
            raise_irq(3);
            break;

        case 0x10: { // GetlocL
            u32 lba = m_read_lba + 150;
            auto to_bcd = [](u32 v) -> u8 { return u8(((v / 10) << 4) | (v % 10)); };
            u8 ff = to_bcd(lba % 75);
            u32 ss = lba / 75;
            u8 s = to_bcd(ss % 60);
            u8 m = to_bcd(ss / 60);
            push_response(m); push_response(s); push_response(ff);
            push_response(0x02); // mode
            push_response(0); push_response(0); push_response(0); push_response(0);
            raise_irq(3);
            break;
        }

        case 0x11: { // GetlocP
            u32 lba = m_read_lba + 150;
            auto to_bcd = [](u32 v) -> u8 { return u8(((v / 10) << 4) | (v % 10)); };
            u8 ff = to_bcd(lba % 75);
            u32 ss = lba / 75;
            u8 s = to_bcd(ss % 60);
            u8 m = to_bcd(ss / 60);
            push_response(0x01); // track
            push_response(0x01); // index
            push_response(m); push_response(s); push_response(ff);
            push_response(m); push_response(s); push_response(ff);
            raise_irq(3);
            break;
        }

        case 0x13: { // GetTN
            u8 st = m_status;
            m_response.clear();
            schedule_first(3, {st, 0x01, 0x01}, 2500);
            break;
        }

        case 0x14: { // GetTD — track start time (BCD MM:SS). Track 0 = lead-out.
            u8 track = m_param.empty() ? 0 : m_param[0];
            m_param.clear();
            auto to_bcd = [](u32 v) -> u8 { return u8(((v / 10) << 4) | (v % 10)); };
            u32 lba = 0;
            if (track == 0) {
                // Lead-out: just past last sector
                u32 sectors = (m_disc && m_disc->is_loaded()) ? m_disc->sector_count() : 0;
                lba = sectors + 150;
            } else {
                // Single data track starts at 00:02:00
                lba = 150;
            }
            u8 ff = to_bcd(lba % 75);
            u32 ss = lba / 75;
            u8 s = to_bcd(ss % 60);
            u8 m = to_bcd(ss / 60);
            (void)ff;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st, m, s}, 2500);
            }
            break;
        }

        case 0x15: // SeekL
        case 0x16: // SeekP
            m_seeking = true;
            m_seek_delay = 2000;
            m_status |= 0x40;
            m_read_lba = m_seek_lba;
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;

        case 0x19: { // Test
            u8 sub = m_param.empty() ? 0 : m_param[0];
            m_param.clear();
            m_response.clear();
            // 19h 20h (and some shells probe 19h 01h): CD-ROM controller
            // version / BIOS date. SCPH-1001 expects four FIFO bytes + INT3.
            // Returning fewer than 4 bytes leaves the shell polling 1F801802.
            if (sub == 0x20 || sub == 0x01) {
                schedule_first(3, {0x94, 0x09, 0x19, 0xC0}, 2500);
                Log::info("CDROM Test(%02X) -> version 94 09 19 C0 (INT3)", sub);
            } else if (sub == 0x60) {
                schedule_first(3, {0x00}, 1500);
            } else if (sub == 0x22) {
                // region / HC05 open - return status
                schedule_first(3, {m_status}, 1500);
            } else {
                schedule_first(3, {m_status}, 1500);
            }
            break;
        }

        case 0x1A: { // GetID
            // After SCE logo the BIOS calls GetID. Licensed disc must return
            // INT2 + SCEA or the shell never reaches the second logo.
            update_status_shell();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            if (!m_disc || !m_disc->is_loaded()) {
                u8 st = u8((m_status & ~0x02u) | 0x10u | 0x01u);
                schedule_second(5, {st, 0x40, 0, 0, 0, 0, 0, 0}, 8000);
                Log::info("CDROM GetID -> no disc (INT5)");
            } else {
                u8 st = u8((m_status & ~0x10u) | 0x02u);
                m_status = st;
                schedule_second(2, {
                    st, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'
                }, 8000);
                Log::info("CDROM GetID -> licensed SCEA (INT2 pending)");
            }
            break;
        }

        case 0x1E: // ReadTOC
            update_status_shell();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            schedule_second(2, {m_status}, 5000);
            break;

        default:
            m_param.clear();
            {
                u8 st = m_status;
                m_response.clear();
                schedule_first(3, {st}, 2500);
            }
            break;
    }
}

} // namespace ps96
