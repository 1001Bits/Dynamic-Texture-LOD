#pragma once

#include <cstdint>
#include <span>

// This process's video memory as Windows accounts for it: the "GPU Process
// Memory" performance counters behind Task Manager. DXGI's CurrentUsage
// can read gigabytes low (overlays, other APIs, upscalers; Skyrim Performance
// Solution measured 1.5-2.5 GB). Shared usage is memory the OS has paged out
// of video memory: while it keeps rising, the game is past its budget,
// whatever the reading says.
namespace dtl::gpumem
{
    struct Sample
    {
        std::uint64_t dedicated = 0;
        std::uint64_t shared = 0;
        std::int64_t  atMs = 0;  // steady clock
        bool          valid = false;
    };

    // Starts the once-a-second sampler thread on the first call. Never joined.
    // The counters live on that one thread, as PDH requires.
    void Start();
    // The newest sample; invalid before the first one, or without counters.
    Sample Latest();
    // Whether shared usage rose by at least a_minRise within a_windowMs.
    bool SharedRising(std::uint64_t a_minRise, std::int64_t a_windowMs);
    // How much shared usage rose within a_windowMs; 0 if it did not.
    std::uint64_t SharedRise(std::int64_t a_windowMs);

    // The rule itself, pure for tests: how far the newest valid sample is above
    // the smallest valid sample no older than a_windowMs before it.
    inline std::uint64_t Rise(std::span<const Sample> a_history, std::int64_t a_windowMs)
    {
        const Sample* newest = nullptr;
        for (auto it = a_history.rbegin(); it != a_history.rend(); ++it)
            if (it->valid) { newest = &*it; break; }
        if (!newest) return 0;
        std::uint64_t lowest = newest->shared;
        for (const auto& sample : a_history)
            if (sample.valid && sample.atMs <= newest->atMs && newest->atMs - sample.atMs <= a_windowMs &&
                sample.shared < lowest)
                lowest = sample.shared;
        return newest->shared - lowest;
    }
    inline bool Rising(std::span<const Sample> a_history, std::uint64_t a_minRise, std::int64_t a_windowMs)
    {
        return a_minRise > 0 && Rise(a_history, a_windowMs) >= a_minRise;
    }
}
