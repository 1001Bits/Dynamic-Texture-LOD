#include "../src/HostFloor.h"
#include <cassert>

int main()
{
    constexpr std::uint64_t MB = 1ull << 20;

    // Off, no budget yet, or enough available memory: the budget as read.
    assert(HostFloor::Budget(7180 * MB, 100 * MB, 0) == 7180 * MB);
    assert(HostFloor::Budget(0, 100 * MB, 1536 * MB) == 0);
    assert(HostFloor::Budget(7180 * MB, 1536 * MB, 1536 * MB) == 7180 * MB);
    assert(HostFloor::Budget(7180 * MB, 4000 * MB, 1536 * MB) == 7180 * MB);

    // Below the floor the budget shrinks by the shortfall.
    assert(HostFloor::Budget(7180 * MB, 1000 * MB, 1536 * MB) == (7180 - 536) * MB);

    // Never by more than a quarter, however short memory is.
    assert(HostFloor::Budget(7180 * MB, 0, 4096 * MB) == 7180 * MB - 7180 * MB / 4);
    assert(HostFloor::Budget(8 * MB, 0, 1536 * MB) == 6 * MB);
}
