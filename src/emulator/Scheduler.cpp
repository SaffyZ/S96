#include "emulator/Scheduler.hpp"

namespace ps96 {

Scheduler::Scheduler() { reset(); }

void Scheduler::reset() {
    m_cycles = 0;
    while (!m_events.empty()) m_events.pop();
}

void Scheduler::schedule(s64 cycles_from_now, EventFn fn) {
    m_events.push(Event{m_cycles + cycles_from_now, std::move(fn)});
}

void Scheduler::tick(s64 cycles) {
    m_cycles += cycles;
    while (!m_events.empty() && m_events.top().time <= m_cycles) {
        auto e = m_events.top();
        m_events.pop();
        if (e.fn) e.fn();
    }
}

} // namespace ps96
