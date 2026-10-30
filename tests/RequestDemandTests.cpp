#include "../src/RequestDemand.h"
#include "core/StarfieldPolicy.h"
#include <cassert>

int main()
{
    using RequestDemand::Snapshot;
    using RequestDemand::Stronger;
    assert(Stronger(Snapshot{false, 3, 0}, Snapshot{true, 3, 0.1f}));
    assert(Stronger(Snapshot{true, 3, 0.1f}, Snapshot{true, 1, 0.1f}));
    assert(Stronger(Snapshot{true, 1, 0.1f}, Snapshot{true, 1, 0.2f}));
    assert(!Stronger(Snapshot{true, 1, 0.2f}, Snapshot{true, 1, 0.2f}));
    assert(!Stronger(Snapshot{true, 1, 0.2f}, Snapshot{false, 3, 0}));
    assert(!Stronger(Snapshot{true, 1, 0.2f}, Snapshot{true, 2, 0.1f}));

    const auto wanted = [](float, float priority) { return dtl::StarfieldWantedLevel(priority, 3); };
    dtl::Observation approach;
    approach.Visit(1, false, 16, 0.01f);
    const auto before = RequestDemand::Capture(approach, 1, true, wanted);
    assert(before.sized && before.wanted == 3);
    approach.Visit(2, false, 512, 0.2f);
    const auto closer = RequestDemand::Capture(approach, 2, true, wanted);
    assert(!approach.Seen(2));
    assert(closer.sized && closer.wanted == 0 && closer.priority == 0.2f);
    assert(Stronger(before, closer));  // Offscreen approaches cancel stale demotions.

    const auto unchanged = RequestDemand::Capture(approach, 2, true, wanted);
    assert(!Stronger(closer, unchanged));
    approach.Visit(2, true, 512, 0.2f);
    const auto turned = RequestDemand::Capture(approach, 2, true, wanted);
    assert(!Stronger(closer, turned));  // A turn alone does not change distance demand.

    dtl::Observation receding;
    receding.Visit(1, false, 512, 0.2f);
    const auto near = RequestDemand::Capture(receding, 1, true, wanted);
    receding.Visit(2, false, 16, 0.01f);
    receding.Visit(3, false, 16, 0.01f);
    const auto far = RequestDemand::Capture(receding, 3, true, wanted);
    assert(far.wanted == 3 && !Stronger(near, far));

    dtl::Observation legacy;
    legacy.Visit(1, false, 16, 0.01f);
    const auto oldUnseen = RequestDemand::Capture(legacy, 1, false, wanted);
    legacy.Visit(2, false, 512, 0.2f);
    const auto stillUnseen = RequestDemand::Capture(legacy, 2, false, wanted);
    assert(!stillUnseen.sized && stillUnseen.priority == 0);
    assert(!Stronger(oldUnseen, stillUnseen));
    legacy.Visit(2, true, 512, 0.2f);
    assert(Stronger(stillUnseen, RequestDemand::Capture(legacy, 2, false, wanted)));

    float capturedPixels = 0;
    RequestDemand::Capture(approach, 2, true, [&capturedPixels](float pixels, float) {
        capturedPixels = pixels;
        return 0;
    });
    assert(capturedPixels == 512);  // Pixel-based protection uses every visit too.
}
