#pragma once

#include "common/Types.hpp"
#include "cdrom/Disc.hpp"
#include <deque>
#include <array>
#include <initializer_list>

namespace ps96 {

class InterruptController;

class CDROM {
public:
    CDROM();
    void reset();
    void set_irq(InterruptController* irq) { m_irq = irq; }
    void set_disc(Disc* disc);
    // After Disc::load_*, refresh shell/motor bits so Getstat sees a closed lid.
    void notify_disc_changed() { update_status_shell(); }

    u8  read8(u32 addr);
    void write8(u32 addr, u8 value);
    u32 read32(u32 addr);
    void write32(u32 addr, u32 value);

    u32 dma_read();
    void tick(int cycles);

    bool disc_present() const { return m_disc && m_disc->is_loaded(); }
    u32 debug_read_lba() const { return m_read_lba; }
    bool debug_reading() const { return m_reading; }
    u32 debug_response_size() const { return static_cast<u32>(m_response.size()); }
    u8 debug_status() const { return m_status; }
    u8 debug_irq_flag() const { return m_irq_flag; }
    u8 debug_request() const { return m_request; }
    bool debug_data_ready() const { return m_data_ready; }

private:
    Disc* m_disc = nullptr;
    InterruptController* m_irq = nullptr;

    u8 m_index = 0;
    u8 m_status = 0x10;
    std::deque<u8> m_response;
    std::deque<u8> m_param;

    u8 m_irq_enable = 0;
    u8 m_irq_flag = 0;

    u8 m_request = 0;
    bool m_data_ready = false;
    std::array<u8, 2352> m_sector{};
    int m_sector_pos = 0;
    u32 m_seek_lba = 0;
    u32 m_read_lba = 0;
    bool m_reading = false;
    bool m_seeking = false;
    int m_read_delay = 0;
    int sector_period_cycles() const;
    int m_seek_delay = 0;
    u8 m_mode = 0x20;
    bool m_muted = true;

    bool m_pending_first = false;
    int m_first_delay = 0;
    u8 m_first_irq = 3;
    std::vector<u8> m_first_response;
    bool m_pending_second = false;
    int m_second_delay = 0;
    std::deque<u8> m_second_response;
    u8 m_second_irq = 0;

    void exec_command(u8 cmd);
    void push_response(u8 v);
    void raise_irq(u8 which);
    void update_status_shell();
    void schedule_first(u8 irq_bits, std::initializer_list<u8> bytes, int delay_cycles);
    void schedule_second(u8 irq_bits, std::initializer_list<u8> bytes, int delay_cycles);
    void load_sector_data();
};

} // namespace ps96
