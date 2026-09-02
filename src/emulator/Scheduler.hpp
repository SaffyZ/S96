#pragma once

#include "common/Types.hpp"
#include <queue>
#include <functional>
#include <vector>

namespace ps96 {

class Scheduler {
public:
    using EventFn = std::function<void()>;

    Scheduler();
    void reset();
    void schedule(s64 cycles_from_now, EventFn fn);
    void tick(s64 cycles);
    s64 cycles() const { return m_cycles; }

private:
    struct Event {
        s64 time;
        EventFn fn;
        bool operator>(const Event& o) const { return time > o.time; }
    };
    s64 m_cycles = 0;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> m_events;
};

} // namespace ps96
