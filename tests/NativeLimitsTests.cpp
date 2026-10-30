#include "../src/NativeLimits.h"
#include "../src/NativeAnswer.h"
#include <cassert>
#include <cstdint>

int main()
{
    using NativeLimits::GameLevel;
    // The object's level plus the room bias for a degradable texture, at most 3.
    assert(GameLevel(0, 0, true) == 0);
    assert(GameLevel(1, 1, true) == 2);
    assert(GameLevel(2, 5, true) == 3);
    // No bias without the allow-degrade flag, and none for the 100 sentinel.
    assert(GameLevel(1, 1, false) == 1);
    assert(GameLevel(1, 100, true) == 1);
    // A this-plugin visitor level of 4 counts as 3.
    assert(GameLevel(4, 0, true) == 3);
    assert(GameLevel(0xFFFFFFFFu, 0, true) == 3);
    static_assert(GameLevel(1, 1, true) == 2);

    using NativeLimits::UpgradeLimit;
    constexpr std::uint64_t MB = 1ull << 20;
    constexpr std::uint64_t game = 7308 * MB;
    // With room to spare, or without a reading, the game's own limit.
    assert(UpgradeLimit(game, 800 * MB, 7052 * MB, 2000 * MB, false) == game);
    assert(UpgradeLimit(game, 800 * MB, 0, 2000 * MB, true) == game);
    // Before the game wrote its limit there is nothing to steer.
    assert(UpgradeLimit(0, 800 * MB, 7052 * MB, 6500 * MB, true) == 0);
    // Tight: textures may grow by what is left under the line.
    assert(UpgradeLimit(game, 800 * MB, 7052 * MB, 6500 * MB, true) == 1352 * MB);
    // Over the line: no growth at all, so every upgrade waits.
    assert(UpgradeLimit(game, 800 * MB, 7052 * MB, 7300 * MB, true) == 800 * MB);
    // Never above the game's own limit.
    assert(UpgradeLimit(1000 * MB, 800 * MB, 7052 * MB, 2000 * MB, true) == 1000 * MB);
    static_assert(UpgradeLimit(game, 1, 3, 2, true) == 2);
    // SteerToTarget passes the target (line minus Target) as the cap, at all
    // times: no growth past the target (rp28: usage climbed 5.8 -> 6.74 GB
    // while turning under the line), the room below it stays the game's.
    assert(UpgradeLimit(game, 5467 * MB, 6156 * MB, 6178 * MB, true) == 5467 * MB);
    assert(UpgradeLimit(game, 5467 * MB, 6156 * MB, 5800 * MB, true) == (5467 + 356) * MB);

    // Steering holds through a calm after pressure (RefillCalmMs, 30 s by
    // default), so a room reading a second after tight keeps the limit.
    using NativeLimits::KeepSteering;
    constexpr std::uint32_t calm = 30000;
    assert(KeepSteering(true, 1000, 1000, calm));
    assert(!KeepSteering(false, 1000, 0, calm));             // never under pressure: the game's own
    assert(KeepSteering(false, 2000, 1000, calm));           // room one second later: still steered
    assert(KeepSteering(false, 30999, 1000, calm));
    assert(!KeepSteering(false, 31000, 1000, calm));         // a full calm: the game's own again
    assert(!KeepSteering(false, 2000, 1000, 0));             // RefillCalmMs=0: the 1.0 behaviour
    static_assert(KeepSteering(false, 5, 1, 10));

    // A [MaxSize] cap acts as a floor: never more detail, in every mode, and
    // never past the least detail the texture can have.
    using NativeAnswer::Level;
    using NativeAnswer::Visit;
    constexpr auto none = NativeAnswer::kNotPlanned;
    assert(Level(Visit{ none, false, 0, 3, 2, true }) == 2);   // room, following the game at 0
    assert(Level(Visit{ 0, false, 0, 3, 2, false }) == 2);     // a planned raise stops at the cap
    assert(Level(Visit{ 3, true, 3, 3, 2, false }) == 3);      // lower than the cap stays lower
}
