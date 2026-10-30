#pragma once

#include <algorithm>
#include <cstdint>

// Two of the game's own limits, as this plugin moves them. Pure, for tests.
namespace NativeLimits
{
    // The game's own level for one texture visit, as ProcessObjects::Func1
    // computes it: the object's level, plus the room bias for a degradable
    // texture unless the bias is the 100 sentinel, at most the maximum.
    constexpr std::uint32_t GameLevel(std::uint32_t a_base, std::uint32_t a_bias, bool a_degradable,
        std::uint32_t a_maximum = 3)
    {
        return std::min(std::min(a_base, a_maximum) + (a_degradable && a_bias < 100 ? a_bias : 0), a_maximum);
    }

    // BSGraphics::uiTextureMemoryUpgradeLimit. StreamingTextureUpgrade and
    // LoadUpgradeTextureDataFromStream raise a texture only while the game's
    // texture counter (uiTotalTextureMemory, kept by Register/
    // UnregisterD3DTexture) is under it; a refused texture is batched again
    // next round. The game writes it once, at device creation: video memory
    // minus 640 MB. Tight or over, textures may grow only into the room left
    // under the budget line; otherwise, and never above it, the game's own.
    constexpr std::uint64_t UpgradeLimit(std::uint64_t a_game, std::uint64_t a_textures, std::uint64_t a_line,
        std::uint64_t a_usage, bool a_pressure)
    {
        if (!a_pressure || !a_line || !a_game) return a_game;
        const auto room = a_line > a_usage ? a_line - a_usage : 0;
        return std::min(a_game, a_textures + room);
    }

    // Whether the upgrade limit stays steered: under pressure, and for a calm
    // period after it ([Budget] RefillCalmMs, which also gates refills). A
    // reading that dips under the target for a moment no longer hands the game
    // its own limit back: tight and room can alternate a second apart, and a
    // release mid-turn lets raises pile in.
    // a_lastPressureMs is 0 before any pressure.
    constexpr bool KeepSteering(bool a_pressure, std::uint64_t a_nowMs, std::uint64_t a_lastPressureMs,
        std::uint32_t a_calmMs)
    {
        return a_pressure || (a_lastPressureMs != 0 && a_nowMs - a_lastPressureMs < a_calmMs);
    }
}
