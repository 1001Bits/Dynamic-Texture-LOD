#include "../src/ChangeRate.h"
#include <cassert>

int main()
{
    // Off: every change goes through.
    ChangeRate off;
    off.Configure(0);
    for (int i = 0; i < 1000; ++i) assert(off.Take(1));

    // 100 a second: a burst of a tenth of a second's worth (10), then one
    // every 10 ms, and never more than the burst after a long quiet spell.
    ChangeRate rate;
    rate.Configure(100);
    int taken = 0;
    for (int i = 0; i < 50; ++i) taken += rate.Take(1000);
    assert(taken == 10);
    assert(!rate.Take(1005) && rate.Take(1010) && !rate.Take(1010));
    taken = 0;
    for (int i = 0; i < 50; ++i) taken += rate.Take(60000);
    assert(taken == 10);

    // A clock that steps back never adds changes.
    assert(!rate.Take(59000) && !rate.Take(60000));

    // Under 10 a second the burst is still one change.
    ChangeRate slow;
    slow.Configure(4);
    assert(slow.Take(1000) && !slow.Take(1100) && slow.Take(1300) && !slow.Take(1301));
}
