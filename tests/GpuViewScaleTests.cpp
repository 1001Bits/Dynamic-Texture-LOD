#include "../src/GpuBounds.h"
#include <cassert>
#include <limits>
#include <numbers>

int main()
{
    using GpuBounds::PerspectiveViewScale;
    const auto close = [](std::optional<float> value, float expected) {
        assert(value && std::abs(*value - expected) <= 2e-6f * expected);
    };
    // Independent projection cases: square 90 degree FOV and a 16:9 view
    // with 90 degree horizontal FOV. The narrower axis controls the scale.
    close(PerspectiveViewScale(-1, 1, 1, -1), .5f);
    close(PerspectiveViewScale(-1, 1, .5625f, -.5625f), 8.0f / 9);
    close(PerspectiveViewScale(-.5625f, .5625f, 1, -1), 8.0f / 9);
    // An asymmetric eye/frustum offset does not change its projection diagonal.
    close(PerspectiveViewScale(-.75f, 1.25f, .6875f, -.4375f), 8.0f / 9);
    for (float fov : { 30.0f, 60.0f, 90.0f, 110.0f }) {
        const float halfHeight = std::tan(fov * std::numbers::pi_v<float> / 360);
        const float expected = .5f / halfHeight;
        close(PerspectiveViewScale(-halfHeight * 2, halfHeight * 2, halfHeight, -halfHeight), expected);
        close(PerspectiveViewScale(-halfHeight, halfHeight, halfHeight * 2, -halfHeight * 2), expected);
    }
    // Invalid eyes must reject a batch, not silently become viewScale=1.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float large = std::numeric_limits<float>::max();
    const float tiny = std::numeric_limits<float>::denorm_min();
    for (float invalid : { nan, inf, -inf }) {
        assert(!PerspectiveViewScale(invalid, 1, 1, -1));
        assert(!PerspectiveViewScale(-1, invalid, 1, -1));
        assert(!PerspectiveViewScale(-1, 1, invalid, -1));
        assert(!PerspectiveViewScale(-1, 1, 1, invalid));
    }
    assert(!PerspectiveViewScale(1, 1, 1, -1));
    assert(!PerspectiveViewScale(1, -1, 1, -1));
    assert(!PerspectiveViewScale(-1, 1, 1, 1));
    assert(!PerspectiveViewScale(-1, 1, -1, 1));
    assert(!PerspectiveViewScale(-large, large, 1, -1));
    assert(!PerspectiveViewScale(-1, 1, large, -large));
    assert(!PerspectiveViewScale(0, tiny, 1, -1));
    assert(!PerspectiveViewScale(-1, 1, tiny, 0));
}
