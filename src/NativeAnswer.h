#pragma once

#include <algorithm>
#include <cstdint>

// The source policy's answer to one native texture visit. Pure, for tests.
namespace NativeAnswer
{
    constexpr std::uint8_t kNotPlanned = 0xFF;

    struct Visit
    {
        std::uint8_t  planned = kNotPlanned;  // the last target this plugin asked for
        bool          held = false;           // planned keeps less detail than the game's own level
        std::uint8_t  current = 0;            // its level now
        std::uint32_t lowest = 3;             // the least detail it may have
        std::uint32_t floor = 0;              // the game's own level for this visit
        bool          followGame = false;     // pressure first, with room to spare
    };

    // Never more detail than the game's own level. With room to spare under
    // pressure first, only what this plugin lowered stays down until refill:
    // everything else takes the game's level, as without the plugin, so a
    // texture the game lowered while loading comes back as the game brings
    // it back. Otherwise an unplanned texture keeps its level until the
    // planner decides.
    constexpr std::uint32_t Level(const Visit& a_visit)
    {
        const std::uint32_t level = a_visit.followGame && !a_visit.held ? a_visit.floor :
            a_visit.planned != kNotPlanned ? a_visit.planned : a_visit.current;
        return std::max(std::min(level, a_visit.lowest), a_visit.floor);
    }

    // Whether an accepted target keeps the texture below the game's level.
    constexpr bool Holds(std::uint32_t a_target, std::uint32_t a_floor) { return a_target > a_floor; }

    // With room to spare under pressure first the visit already takes the
    // game's level, so a planned lowering only repeats one the game made
    // before the snapshot. Once the game has given the texture more detail
    // again, accepting it would hold the texture below the game's level until
    // refill: worse than without the plugin, so the request is stale.
    constexpr bool StaleFollow(bool a_followGame, std::uint32_t a_current, std::uint32_t a_target,
        std::uint32_t a_floor)
    {
        return a_followGame && a_target > a_current && Holds(a_target, a_floor);
    }

    // Whether the native streamer still has a level change to make. A texture
    // that near and far objects share is batched every round: the far visit
    // queues a coarser level, the near one lowers the queued level back to the
    // current one, and the batch changes nothing. Only a queued level other
    // than the current one is work in flight (the legacy rule counted the
    // batch flag alone, so such textures never reached refill).
    constexpr bool NativeBusy(std::uint32_t a_requests, std::uint8_t a_stateFlags, bool a_inBatch,
        std::uint8_t a_queued, std::uint8_t a_current, bool a_legacy)
    {
        return a_requests != 0 || (a_stateFlags & 3u) != 1u ||
            (a_inBatch && (a_legacy || a_queued != a_current));
    }
}
