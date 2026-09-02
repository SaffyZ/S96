#include "controllers/Controllers.hpp"
#include "emulator/InterruptController.hpp"
#include "memorycard/MemoryCard.hpp"
#include "common/Log.hpp"
#include <algorithm>

namespace ps96 {

namespace {
constexpr u16 STAT_TX_READY = 1u << 0;
constexpr u16 STAT_RX_READY = 1u << 1;
constexpr u16 STAT_TX_IDLE  = 1u << 2;
constexpr u16 STAT_ACK      = 1u << 7;   // SIO_STAT.7 = DSR, /ACK on SIO0
constexpr u16 STAT_IRQ      = 1u << 9;   // SIO_STAT.9, sticky local IRQ request
constexpr u16 CTRL_TXEN     = 1u << 0;
constexpr u16 CTRL_DTR      = 1u << 1;
constexpr u16 CTRL_RXEN     = 1u << 2;
constexpr u16 CTRL_ACK      = 1u << 4;
constexpr u16 CTRL_RESET    = 1u << 6;
constexpr u16 CTRL_DSRE     = 1u << 12;
constexpr u16 CTRL_PORT2    = 1u << 13;
constexpr int ACK_LOW_CYCLES = 100; // documented minimum-scale pulse, ~2.95 us
}

Controllers::Controllers() { reset(); }

void Controllers::reset() {
    m_joy_stat = STAT_TX_READY | STAT_TX_IDLE;
    m_joy_mode = 0;
    m_joy_ctrl = 0;
    m_joy_baud = 0x0088;
    m_selected = -1;
    m_selected_latched = -1;
    m_txen_latched = false;
    m_transfer_active = false;
    m_tx_busy = false;
    m_tx_pending = false;
    m_tx_pending_byte = 0;
    m_tx_shift_byte = 0;
    m_tx_cycles = 0;
    m_analog_mode[0] = false;
    m_analog_mode[1] = false;
    m_config_mode[0] = false;
    m_config_mode[1] = false;
    m_command = 0;
    m_tx_len = 0;
    m_tx_pos = 0;
    m_rx_fifo.clear();
    m_rx_last = 0xFF;
    m_ack_pulse = false;
    m_ack_wait = false;
    m_ack_cycles = 0;
    m_ack_delay = 0;
    m_pads[0] = {};
    m_pads[1] = {};
    m_poll_snapshot[0] = {};
    m_poll_snapshot[1] = {};
}

void Controllers::set_pad_state(int slot, const PadState& state) {
    if (slot >= 0 && slot < 2) m_pads[slot] = state;
}

int Controllers::baud_cycles_per_byte() const {
    // SIO0_BAUD uses the master clock divider; the retail/default value 0x0088
    // produces the standard ~250 kHz controller clock. Each byte transfers
    // eight clocked bits, so 0x88 corresponds to roughly 1088 master cycles.
    const u32 mode_factor = [&]() -> u32 {
        switch (m_joy_mode & 3u) {
            case 1: return 1;
            case 2: return 16;
            case 3: return 64;
            default: return 1;
        }
    }();
    const u32 bit_cycles = std::max<u32>(1u, u32(m_joy_baud & 0xFFFEu)) * mode_factor;
    return static_cast<int>(std::max<u32>(8u, bit_cycles * 8u));
}

void Controllers::enqueue_rx(u8 value) {
    m_rx_last = value;
    if (m_rx_fifo.size() >= 8) {
        // Hardware overwrites the newest/last FIFO slot when full. SIO0 does
        // not expose the overrun flag, so retain the documented visible bytes.
        m_rx_fifo.back() = value;
    } else {
        m_rx_fifo.push_back(value);
    }
    if (!m_rx_fifo.empty()) m_joy_stat |= STAT_RX_READY;
}

u8 Controllers::dequeue_rx() {
    if (m_rx_fifo.empty()) return m_rx_last;
    const u8 value = m_rx_fifo.front();
    m_rx_fifo.pop_front();
    if (m_rx_fifo.empty()) m_joy_stat &= ~STAT_RX_READY;
    else m_joy_stat |= STAT_RX_READY;
    return value;
}

u8 Controllers::read8(u32 addr) {
    switch (addr) {
        case 0x1F801040: {
            const u8 value = dequeue_rx();
            static int s_rx_trace = 0;
            if (s_rx_trace < 80) {
                Log::info("SIO0 RX read=%02X fifo=%u stat=%03X", value,
                          static_cast<unsigned>(m_rx_fifo.size()), m_joy_stat & 0x3FF);
                ++s_rx_trace;
            }
            return value;
        }
        case 0x1F801044:
            return static_cast<u8>(m_joy_stat & 0xFF);
        case 0x1F801048:
            return static_cast<u8>(m_joy_mode & 0xFF);
        case 0x1F801049:
            return static_cast<u8>(m_joy_mode >> 8);
        case 0x1F80104A:
            return static_cast<u8>(m_joy_ctrl & 0xFF);
        case 0x1F80104B:
            return static_cast<u8>(m_joy_ctrl >> 8);
        case 0x1F80104E:
            return static_cast<u8>(m_joy_baud & 0xFF);
        case 0x1F80104F:
            return static_cast<u8>(m_joy_baud >> 8);
        default:
            return 0xFF;
    }
}

u16 Controllers::read16(u32 addr) {
    if (addr == 0x1F801040) {
        // A 16-bit RX read consumes one FIFO entry; the high byte is a preview
        // of the next entry, as documented by the physical SIO FIFO.
        const u8 lo = dequeue_rx();
        const u8 hi = m_rx_fifo.empty() ? m_rx_last : m_rx_fifo.front();
        return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
    }
    if (addr == 0x1F801044) return m_joy_stat;
    if (addr == 0x1F801048) return m_joy_mode;
    if (addr == 0x1F80104A) return m_joy_ctrl;
    if (addr == 0x1F80104E) return m_joy_baud;
    return 0xFFFF;
}

u32 Controllers::read32(u32 addr) {
    if (addr == 0x1F801040) {
        // A word read consumes four FIFO entries. Unavailable preview bytes are
        // repeated as the last received value, matching the hardware's stale
        // FIFO behavior closely enough for software that incorrectly uses Lw.
        u32 value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<u32>(dequeue_rx()) << (i * 8);
        return value;
    }
    switch (addr) {
        case 0x1F801044: return m_joy_stat;
        case 0x1F801048: return m_joy_mode;
        case 0x1F80104A: return m_joy_ctrl;
        case 0x1F80104E: return m_joy_baud;
        default: return 0xFFFFFFFFu;
    }
}

void Controllers::write8(u32 addr, u8 value) {
    switch (addr) {
        case 0x1F801040:
            start_transfer(value);
            return;
        case 0x1F801048:
            m_joy_mode = static_cast<u16>((m_joy_mode & 0xFF00u) | value);
            return;
        case 0x1F801049:
            m_joy_mode = static_cast<u16>((m_joy_mode & 0x00FFu) | (static_cast<u16>(value) << 8));
            return;
        case 0x1F80104A: {
            const u16 merged = static_cast<u16>((m_joy_ctrl & 0xFF00u) | value);
            write16(addr, merged);
            return;
        }
        case 0x1F80104B: {
            const u16 merged = static_cast<u16>((m_joy_ctrl & 0x00FFu) | (static_cast<u16>(value) << 8));
            write16(0x1F80104A, merged);
            return;
        }
        case 0x1F80104E:
            m_joy_baud = static_cast<u16>((m_joy_baud & 0xFF00u) | value);
            return;
        case 0x1F80104F:
            m_joy_baud = static_cast<u16>((m_joy_baud & 0x00FFu) | (static_cast<u16>(value) << 8));
            return;
        default:
            return;
    }
}

void Controllers::write16(u32 addr, u16 value) {
    switch (addr) {
        case 0x1F801048: m_joy_mode = value; break;
        case 0x1F80104A: write32(addr, value); break;
        case 0x1F80104E: m_joy_baud = value; break;
        default: break;
    }
}

void Controllers::write32(u32 addr, u32 value) {
    switch (addr) {
        case 0x1F801040:
            start_transfer(static_cast<u8>(value));
            return;
        case 0x1F801048:
            m_joy_mode = static_cast<u16>(value);
            return;
        case 0x1F80104A: {
            const u16 requested = static_cast<u16>(value);

            if (requested & CTRL_ACK) {
                // Local SIO_STAT.9 cannot be cleared while /ACK/DSR is still
                // asserted. I_STAT.7 is a separate edge-triggered latch and
                // is cleared by the interrupt controller at 1F801070.
                if ((m_joy_stat & STAT_ACK) == 0) m_joy_stat &= ~STAT_IRQ;
            }

            if (requested & CTRL_RESET) {
                const bool keep_irq = (m_joy_stat & STAT_IRQ) != 0;
                m_joy_ctrl = 0;
                m_txen_latched = false;
                m_transfer_active = false;
                m_tx_busy = false;
                m_tx_pending = false;
                m_tx_cycles = 0;
                m_tx_shift_byte = 0;
                m_selected = -1;
                m_selected_latched = -1;
                m_tx_pos = 0;
                m_tx_len = 0;
                m_command = 0;
                m_rx_fifo.clear();
                m_ack_pulse = false;
                m_ack_wait = false;
                m_ack_cycles = 0;
                m_ack_delay = 0;
                m_joy_stat = STAT_TX_READY | STAT_TX_IDLE | (keep_irq ? STAT_IRQ : 0);
                return;
            }

            const int new_selected = (requested & CTRL_DTR)
                ? ((requested & CTRL_PORT2) ? 1 : 0)
                : -1;

            if (new_selected != m_selected) {
                m_selected = new_selected;
                if (!m_tx_busy && !m_transfer_active) {
                    m_selected_latched = -1;
                } else if (m_transfer_active && new_selected != m_selected_latched) {
                    // /CS or port-select changes terminate the active JoyBus
                    // packet. This prevents port 2's address byte being
                    // interpreted as byte 2 of port 1's old packet.
                    m_transfer_active = false;
                    m_txen_latched = false;
                    m_selected_latched = -1;
                    m_tx_busy = false;
                    m_tx_pending = false;
                    m_tx_cycles = 0;
                    m_tx_pos = 0;
                    m_tx_len = 0;
                    m_command = 0;
                    m_rx_fifo.clear();
                    m_joy_stat &= ~STAT_ACK;
                    m_ack_pulse = false;
                    m_ack_wait = false;
                    m_ack_cycles = 0;
                    m_ack_delay = 0;
                    m_joy_stat |= STAT_TX_READY | STAT_TX_IDLE;
                }
            }

            m_joy_ctrl = static_cast<u16>(requested & ~(CTRL_ACK | CTRL_RESET));
            if (new_selected < 0) {
                m_transfer_active = false;
                m_txen_latched = false;
                m_tx_busy = false;
                m_tx_pending = false;
                m_tx_cycles = 0;
                m_tx_shift_byte = 0;
                m_selected_latched = -1;
                m_tx_pos = 0;
                m_tx_len = 0;
                m_command = 0;
                m_rx_fifo.clear();
                m_joy_stat &= ~STAT_ACK;
                m_ack_pulse = false;
                m_ack_wait = false;
                m_ack_cycles = 0;
                m_ack_delay = 0;
                m_joy_stat |= STAT_TX_READY | STAT_TX_IDLE;
            }
            break;
        }
        case 0x1F80104E:
            m_joy_baud = static_cast<u16>(value);
            break;
        default:
            break;
    }
}

void Controllers::start_transfer(u8 value) {
    const bool txen_now = (m_joy_ctrl & CTRL_TXEN) != 0;
    if (!txen_now || m_selected < 0) {
        enqueue_rx(0xFF);
        m_joy_stat |= STAT_TX_READY | STAT_TX_IDLE;
        return;
    }

    // Real SIO has a one-byte transmit register/FIFO in addition to the byte
    // currently shifting. A write while busy replaces the pending byte.
    if (m_tx_busy) {
        m_tx_pending = true;
        m_tx_pending_byte = value;
        m_joy_stat |= STAT_TX_READY;
        return;
    }

    if (!m_transfer_active) {
        m_transfer_active = true;
        m_txen_latched = true; // TXEN is latched with the transfer
        m_selected_latched = m_selected;
        m_poll_snapshot[m_selected_latched] = m_pads[m_selected_latched];
        m_tx_pos = 0;
        m_tx_len = 0;
        m_command = 0;
    }

    m_tx_busy = true;
    m_tx_shift_byte = value;
    m_tx_cycles = baud_cycles_per_byte();
    m_joy_stat &= ~STAT_TX_IDLE;
    // SIO_STAT.0 returns once the current byte has entered the shift register,
    // allowing one pending byte to be queued while STAT.2 remains busy.
    m_joy_stat |= STAT_TX_READY;
}

void Controllers::complete_transfer(u8 value) {
    if (!m_transfer_active || m_selected_latched < 0) {
        enqueue_rx(0xFF);
        return;
    }

    m_tx_pos = std::min(m_tx_pos + 1, static_cast<int>(m_tx_buf.size()));
    m_tx_len = std::max(m_tx_len, m_tx_pos);
    m_tx_buf[m_tx_pos - 1] = value;

    const int port = m_selected_latched;
    const PadState& p = m_poll_snapshot[port];
    bool last_byte = false;
    u8 response = 0xFF;

    auto finish = [&]() {
        m_transfer_active = false;
        m_txen_latched = false;
        m_selected_latched = -1;
        m_tx_pos = 0;
        m_tx_len = 0;
        m_command = 0;
    };

    const u8 first = (m_tx_len > 0) ? m_tx_buf[0] : value;
    if (first == 0x01) {
        if (m_tx_pos == 1) {
            response = 0xFF;
        } else if (m_tx_pos == 2) {
            m_command = value;
            switch (m_command) {
                case 0x42:
                    response = m_analog_mode[port] ? 0x73 : 0x41;
                    break;
                case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x4C:
                    response = m_config_mode[port] ? 0xF3 : (m_analog_mode[port] ? 0x73 : 0x41);
                    break;
                default:
                    response = 0xFF;
                    break;
            }
        } else if (m_tx_pos == 3) {
            response = 0x5A;
        } else {
            switch (m_command) {
                case 0x42:
                    if (m_tx_pos == 4) {
                        response = 0xFF;
                        if (p.select) response &= ~0x01;
                        if (p.l3) response &= ~0x02;
                        if (p.r3) response &= ~0x04;
                        if (p.start) response &= ~0x08;
                        if (p.up) response &= ~0x10;
                        if (p.right) response &= ~0x20;
                        if (p.down) response &= ~0x40;
                        if (p.left) response &= ~0x80;
                    } else if (m_tx_pos == 5) {
                        response = 0xFF;
                        if (p.l2) response &= ~0x01;
                        if (p.r2) response &= ~0x02;
                        if (p.l1) response &= ~0x04;
                        if (p.r1) response &= ~0x08;
                        if (p.triangle) response &= ~0x10;
                        if (p.circle) response &= ~0x20;
                        if (p.cross) response &= ~0x40;
                        if (p.square) response &= ~0x80;
                        last_byte = !m_analog_mode[port];
                    } else if (m_analog_mode[port]) {
                        if (m_tx_pos == 6) response = static_cast<u8>(p.stick_rx);
                        else if (m_tx_pos == 7) response = static_cast<u8>(p.stick_ry);
                        else if (m_tx_pos == 8) response = static_cast<u8>(p.stick_lx);
                        else if (m_tx_pos == 9) { response = static_cast<u8>(p.stick_ly); last_byte = true; }
                    } else {
                        response = 0xFF;
                        last_byte = true;
                    }
                    break;
                case 0x43:
                    if (m_tx_pos == 4) m_config_mode[port] = (value & 1) != 0;
                    response = 0x00;
                    last_byte = m_tx_pos >= 9;
                    break;
                case 0x44:
                    // DuckStation models the 0x44 SetAnalogMode packet as a
                    // configuration command whose mode byte arrives after the
                    // command/header bytes. S96 historically only accepted one
                    // exact tx_pos, so a legal BIOS/game sequence could be
                    // parsed but never actually switch the pad into analog.
                    if ((m_tx_pos == 4 || m_tx_pos == 5) && (value == 0x00 || value == 0x01))
                        m_analog_mode[port] = (value == 0x01);
                    response = 0x00;
                    last_byte = m_tx_pos >= 9;
                    break;
                case 0x45: {
                    static constexpr u8 status[6] = {0x03, 0x02, 0x01, 0x02, 0x01, 0x00};
                    response = (m_tx_pos >= 4 && m_tx_pos <= 9) ? status[m_tx_pos - 4] : 0x00;
                    if (m_tx_pos == 7) response = m_analog_mode[port] ? 0x01 : 0x00;
                    last_byte = m_tx_pos >= 9;
                    break;
                }
                case 0x46: case 0x47: case 0x4C:
                    response = 0x00;
                    last_byte = m_tx_pos >= 9;
                    break;
                default:
                    response = 0xFF;
                    last_byte = true;
                    break;
            }
        }
    } else if (first == 0x81) {
        if (m_tx_pos == 1) response = 0xFF;
        else if (m_tx_pos == 2) response = 0x5A;
        else if (m_tx_pos == 3) { response = 0x5D; last_byte = true; }
        else { response = 0xFF; last_byte = true; }
    } else {
        response = 0xFF;
        last_byte = true;
    }

    enqueue_rx(response);
    m_joy_stat |= STAT_TX_READY | STAT_TX_IDLE;

    // SIO0 receives an /ACK pulse after every controller byte, including the
    // final byte of the transaction. Several BIOS/game input loops synchronize
    // on that pulse and on the corresponding controller IRQ; dropping the final
    // pulse can leave the transaction logically open on the software side.
    m_ack_wait = true;
    m_ack_delay = ACK_LOW_CYCLES;
    m_ack_pulse = false;
    m_ack_cycles = 0;
    m_joy_stat &= ~STAT_ACK;
    if (last_byte)
        finish();
}

void Controllers::tick(int cycles) {
    if (cycles <= 0) return;

    int remaining = cycles;
    while (remaining > 0) {
        int step = remaining;
        int event = step;
        if (m_tx_busy) event = std::min(event, std::max(1, m_tx_cycles));
        if (m_ack_wait) event = std::min(event, std::max(1, m_ack_delay));
        if (m_ack_pulse) event = std::min(event, std::max(1, m_ack_cycles));

        if (m_tx_busy) m_tx_cycles -= event;
        if (m_ack_wait) m_ack_delay -= event;
        if (m_ack_pulse) m_ack_cycles -= event;
        remaining -= event;

        if (m_tx_busy && m_tx_cycles <= 0) {
            m_tx_busy = false;
            const u8 value = m_tx_shift_byte;
            complete_transfer(value);
            if (m_tx_pending && m_transfer_active) {
                const u8 next = m_tx_pending_byte;
                m_tx_pending = false;
                // The transaction remains active; directly schedule the next
                // physical byte without clearing the just-completed packet.
                m_tx_shift_byte = next;
                m_tx_busy = true;
                m_tx_cycles = baud_cycles_per_byte();
                m_joy_stat &= ~STAT_TX_IDLE;
                m_joy_stat |= STAT_TX_READY;
            } else {
                m_tx_pending = false;
            }
        }

        if (m_ack_wait && m_ack_delay <= 0) {
            m_ack_wait = false;
            m_ack_pulse = true;
            m_ack_cycles = ACK_LOW_CYCLES;
            m_joy_stat |= STAT_ACK;
            if (m_joy_ctrl & CTRL_DSRE) {
                m_joy_stat |= STAT_IRQ;
                if (m_irq) m_irq->raise(InterruptController::CONTROLLER);
            }
        }

        if (m_ack_pulse && m_ack_cycles <= 0) {
            m_ack_pulse = false;
            m_ack_cycles = 0;
            m_joy_stat &= ~STAT_ACK;
        }

        // If a TX byte completed and the transaction ended, the next queued
        // byte (if any) is still valid but is handled by the same bus-selected
        // peripheral. A CS/port change between bytes cancels it in write32.
        if (event <= 0) break;
    }
}

} // namespace ps96
