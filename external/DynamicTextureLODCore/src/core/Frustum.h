#pragma once

#include <cmath>

namespace dtl
{
    // Sphere centre expressed along the camera's forward/right/up vectors.
    // Perspective sides are slopes at unit distance, as in NiFrustumPlanes.
    inline bool SphereInFrustum(float depth, float across, float rise, float radius,
        float left, float right, float bottom, float top, bool ortho = false)
    {
        if (ortho || !std::isfinite(depth) || !std::isfinite(across) || !std::isfinite(rise) ||
            !std::isfinite(radius) || radius < 0.0f || !(left < right) || !(bottom < top)) {
            return true;  // No trustworthy rejection: retain detail.
        }
        if (depth < -radius) {
            return false;
        }
        const auto inside = [&](float position, float slope, bool upper) {
            const float reach = radius * std::sqrt(1.0f + slope * slope);
            return upper ? position - depth * slope <= reach : depth * slope - position <= reach;
        };
        return inside(across, right, true) && inside(across, left, false) &&
               inside(rise, top, true) && inside(rise, bottom, false);
    }
}
