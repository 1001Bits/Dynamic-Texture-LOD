#pragma once

#include <algorithm>
#include <cstdint>

// [Smoothing] MaxChangesPerSecond: every texture level change is a re-create
// on the game's one creation thread, which the frame waits on. At most that
// many accepted changes a second, at most a tenth of a second's worth at once.
// Pure, for tests; the planner thread is its only user.
class ChangeRate
{
public:
    void Configure(std::uint32_t a_perSecond)
    {
        perSecond = a_perSecond;
        capacity = std::max(1.0, a_perSecond / 10.0);
        tokens = capacity;
        lastMs = 0;
    }

    // Whether one more change may be accepted at a_nowMs (monotonic). 0 per
    // second is off: always.
    bool Take(std::uint64_t a_nowMs)
    {
        if (!perSecond) return true;
        if (lastMs && a_nowMs > lastMs) tokens = std::min(capacity, tokens + (a_nowMs - lastMs) * (perSecond / 1000.0));
        lastMs = std::max(lastMs, a_nowMs);
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }

private:
    std::uint32_t perSecond = 0;
    double capacity = 0.0, tokens = 0.0;
    std::uint64_t lastMs = 0;
};
