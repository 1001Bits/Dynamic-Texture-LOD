#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dtl
{
    // FUN_1429d7de0 changes the BUDGET (not usage) by at most 10 MiB on
    // a distinct frame ID. The first nonzero budget initializes directly;
    // repeated calls in a frame do not add extra steps. Skipped frame IDs
    // add none either, unless a sampler reading every few frames allows up
    // to a_maxSteps of them, so the budget moves at the same rate in time.
    class FrameBudgetSmoother
    {
    public:
        std::uint64_t Observe(std::uint64_t frame, std::uint64_t rawBudget, std::uint64_t a_maxSteps = 1)
        {
            if (initialized_ && frame == frame_) return budget_;
            const std::uint64_t steps = initialized_ && frame > frame_ ?
                std::clamp<std::uint64_t>(frame - frame_, 1, std::max<std::uint64_t>(a_maxSteps, 1)) : 1;
            initialized_ = true;
            frame_ = frame;
            const std::uint64_t step = (10ull << 20) * steps;
            if (budget_ == 0) budget_ = rawBudget;
            else if (rawBudget > budget_) budget_ += std::min(rawBudget - budget_, step);
            else budget_ -= std::min(budget_ - rawBudget, step);
            return budget_;
        }
        void Reset() { initialized_ = false; frame_ = budget_ = 0; }
        std::uint64_t Current() const { return budget_; }
    private:
        bool initialized_ = false;
        std::uint64_t frame_ = 0, budget_ = 0;
    };

    // Correct a cached DXGI reading only when the backend supplies the same
    // allocation counter at query time and now. Missing counters mean no
    // correction; logical mip estimates are not engine allocation counters.
    inline std::uint64_t CorrectCachedUsage(std::uint64_t queriedUsage,
                                          std::uint64_t allocatedAtQuery,
                                          std::uint64_t allocatedNow)
    {
        if (allocatedNow < allocatedAtQuery)
            return queriedUsage - std::min(queriedUsage, allocatedAtQuery - allocatedNow);
        const auto growth = allocatedNow - allocatedAtQuery;
        return growth > std::numeric_limits<std::uint64_t>::max() - queriedUsage ?
            std::numeric_limits<std::uint64_t>::max() : queriedUsage + growth;
    }
}
