#pragma once

#include <cstdint>
#include "core/Observation.h"

namespace RequestDemand
{
    struct Snapshot
    {
        bool sized = false;  // visible, or sized offscreen by the distance policy
        std::uint8_t wanted = 0;
        float priority = 0;
    };

    // Pending requests must use the same observation bank as the planner.
    // Otherwise an offscreen approach can leave an obsolete lowering queued,
    // then require an avoidable restore when the player turns toward it.
    template <class WantedLevel>
    inline Snapshot Capture(const dtl::Observation& observation, std::uint64_t round,
        bool byDistance, WantedLevel&& wantedLevel)
    {
        const auto pixels = byDistance ? observation.ReachPixels() : observation.Pixels();
        const auto priority = byDistance ? observation.Reach() : observation.Priority();
        return { observation.Seen(round) || byDistance,
            static_cast<std::uint8_t>(wantedLevel(pixels, priority)), priority };
    }

    // A budget/balance demotion may intentionally exceed wanted. Only a new,
    // stronger observation invalidates that decision while it is queued.
    inline bool Stronger(const Snapshot& before, const Snapshot& now)
    {
        return now.sized && (!before.sized || now.wanted < before.wanted || now.priority > before.priority);
    }
}
