#pragma once

#include <algorithm>
#include <cstdint>

// [Budget] HostRamFloorMB: while the system's available physical memory is
// below the floor, a video memory reading's budget shrinks by the shortfall,
// at most a quarter, so budget mode lowers textures before Windows pages the
// game out. Each GB of texture VRAM lowered freed about a GB of host RAM at 4K
// with a texture pack. Pure, for tests.
namespace HostFloor
{
    // The budget to report: a_budget unchanged with the floor off (0), no
    // budget yet, or enough available memory.
    inline std::uint64_t Budget(std::uint64_t a_budget, std::uint64_t a_available, std::uint64_t a_floor)
    {
        if (!a_floor || !a_budget || a_available >= a_floor) return a_budget;
        return a_budget - (std::min)(a_floor - a_available, a_budget / 4);
    }
}
