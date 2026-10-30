#include "../src/NativeAnswer.h"
#include <cassert>

int main()
{
    using NativeAnswer::Level;
    using NativeAnswer::Visit;
    constexpr auto none = NativeAnswer::kNotPlanned;

    // Lowered by the game while loading, then the game wants it back: with
    // room to spare it comes back with the game, not after a refill.
    assert(Level(Visit{ none, false, 2, 3, 0, true }) == 0);
    assert(Level(Visit{ none, false, 1, 3, 0, true }) == 0);
    // Tight or over: an unplanned texture keeps its level.
    assert(Level(Visit{ none, false, 2, 3, 0, false }) == 2);

    // Lowered by this plugin under pressure: held until refill restores it,
    // even with room to spare. A partial restore to the pixel cap still holds.
    assert(Level(Visit{ 3, true, 3, 3, 0, true }) == 3);
    assert(Level(Visit{ 1, true, 1, 3, 0, true }) == 1);
    // A completed restore no longer holds: the game decides, so a later,
    // closer view gets the game's new level instead of the old target.
    assert(Level(Visit{ 1, false, 1, 3, 0, true }) == 0);
    // Without follow mode the accepted target stands.
    assert(Level(Visit{ 1, false, 1, 3, 0, false }) == 1);

    // Never more detail than the game's own level, in every mode.
    assert(Level(Visit{ none, false, 0, 3, 2, true }) == 2);
    assert(Level(Visit{ none, false, 0, 3, 2, false }) == 2);
    assert(Level(Visit{ 0, false, 0, 3, 2, false }) == 2);
    assert(Level(Visit{ 1, true, 1, 3, 2, true }) == 2);
    // Never past the least detail the texture can have.
    assert(Level(Visit{ 4, true, 2, 3, 0, false }) == 3);
    assert(Level(Visit{ none, false, 4, 3, 0, false }) == 3);

    assert(NativeAnswer::Holds(2, 0) && !NativeAnswer::Holds(0, 0) && !NativeAnswer::Holds(1, 2));

    // Room to spare: the snapshot saw the game lower a texture to 2, the game
    // has brought it back to 0 since. Accepting the lowering would hold it.
    using NativeAnswer::StaleFollow;
    assert(StaleFollow(true, 0, 2, 0));
    // Still matching the game's level, or no lower than it: not a hold.
    assert(!StaleFollow(true, 0, 2, 2) && !StaleFollow(true, 1, 2, 3));
    // A raise, or tight or over (a real pressure lowering): never stale here.
    assert(!StaleFollow(true, 2, 0, 0) && !StaleFollow(false, 0, 2, 0));
    static_assert(StaleFollow(true, 0, 2, 0));

    // Native work in flight. Ready (state bit 0, no deferred bit), nothing
    // requested and not batched: idle.
    using NativeAnswer::NativeBusy;
    assert(!NativeBusy(0, 0x1, false, 0, 0, false));
    assert(!NativeBusy(0, 0xFD, false, 2, 0, false));  // a stale queued level without the batch flag
    // Requests outstanding, not ready, or a deferred operation: busy.
    assert(NativeBusy(1, 0x1, false, 0, 0, false));
    assert(NativeBusy(0, 0x0, false, 0, 0, false));
    assert(NativeBusy(0, 0x3, false, 0, 0, false));
    // Batched with a real change queued, either way: busy.
    assert(NativeBusy(0, 0x1, true, 0, 1, false));
    assert(NativeBusy(0, 0x1, true, 2, 1, false));
    // Batched, but the round's visits brought the queued level back to the
    // current one: the batch changes nothing, so refill may plan it.
    assert(!NativeBusy(0, 0x1, true, 1, 1, false));
    // The legacy rule counted the flag alone.
    assert(NativeBusy(0, 0x1, true, 1, 1, true));
    assert(!NativeBusy(0, 0x1, false, 2, 1, true));
    static_assert(!NativeBusy(0, 0x1, true, 0, 0, false));
    static_assert(Level(Visit{ none, false, 2, 3, 0, true }) == 0);
}
