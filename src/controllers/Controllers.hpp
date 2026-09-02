#pragma once

#include "common/Types.hpp"
#include <array>
#include <deque>
#include <string>

namespace ps96 {

class InterruptController;
class MemoryCard;

struct PadState {
    bool up = false, down = false, left = false, right = false;
    bool select = false, start = false;
    bool l3 = false, r3 = false;
    bool triangle = false, circle = false, cross = false, square = false;
    bool l1 = false, l2 = false, r1 = false, r2 = false;
    s16 stick_lx = 0, stick_ly = 0, stick_rx = 0, stick_ry = 0;
    bool analog = false;
};

class Controllers {
public:
    Controllers();
    void reset();
    void set_irq(InterruptController* irq) { m_irq = irq; }
    void set_memcard(int slot, MemoryCard* card) { if (slot >= 0 && slot < 2) m_cards[slot] = card; }

    u8  read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    void set_pad_state(int slot, const PadState& state);
    void tick(int cycles);
    PadState& pad_state(int slot) { return m_pads[slot]; }

    // Xbox 360 / SDL mapping applied in frontend Input

private:
    InterruptController* m_irq = nullptr;
    MemoryCard* m_cards[2]{nullptr, nullptr};
    PadState m_pads[2]{};
    PadState m_poll_snapshot[2]{};

    u16 m_joy_stat = 0;
    u16 m_joy_mode = 0;
    u16 m_joy_ctrl = 0;
    u16 m_joy_baud = 0x0088;
    std::deque<u8> m_rx_fifo;
    u8  m_rx_last = 0xFF;
    bool m_ack_pulse = false;
    bool m_ack_wait = false;
    int m_ack_cycles = 0;
    int m_ack_delay = 0;
    bool m_transfer_active = false;
    bool m_tx_busy = false;
    bool m_tx_pending = false;
    u8  m_tx_pending_byte = 0;
    u8  m_tx_shift_byte = 0;
    int m_tx_cycles = 0;
    bool m_analog_mode[2] = {false, false};
    bool m_config_mode[2] = {false, false};
    u8 m_command = 0;
    bool m_txen_latched = false;
    int  m_selected_latched = -1;

    int m_selected = -1; // 0 or 1
    int m_bitbang_pos = 0;
    std::array<u8, 16> m_tx_buf{};
    int m_tx_len = 0;
    int m_tx_pos = 0;
    std::array<u8, 32> m_rx_buf{};
    int m_rx_len = 0;
    int m_rx_pos = 0;

    void start_transfer(u8 value);
    void complete_transfer(u8 value);
    int baud_cycles_per_byte() const;
    void enqueue_rx(u8 value);
    u8 dequeue_rx();
};

} // namespace ps96
