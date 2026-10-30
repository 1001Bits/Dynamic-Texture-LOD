#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Screen-size texture LOD: how many mips a texture can give up at its size on
// screen, and how it ranks against the others when memory is short.
//
// Pure functions with no engine in them, shared by every game backend. A
// level is a count of mips dropped from the top: 0 is full size.
//
// Starfield's ScreenSizeBasedLOD ranks each texture by its object's angular
// size divided by the material's UV tiling, puts anything near the camera
// first, and maps the rank to a level through a fixed table that ignores the
// texture's own size. Here the rank uses projected coverage and the same
// near-object preference, and the level comes from the
// texture's size against the pixels it covers, so a 512 and a 4K texture on
// the same object keep the same visible detail.
namespace dtl
{
    // How the scene is being viewed.
    struct View
    {
        float screenHeight = 1080.0f;  // pixels
        float tanHalfFovY = 0.5774f;   // tan of half the vertical field of view
    };

    // One texture on one object.
    struct Placement
    {
        std::uint32_t textureSize = 0;  // smaller side of the texture's top mip, in texels
        float         radius = 0.0f;    // the object's bounding sphere, world units
        float         distance = 0.0f;  // camera to the centre of that sphere
    };

    struct Policy
    {
        // Levels kept above the one the screen can show. The estimate assumes
        // the texture's UV range is spread evenly over the object, and a mesh
        // that packs more of it into a small area shows finer detail.
        std::uint32_t safetyMargin = 1;

        // The most the backend can drop from this texture.
        std::uint32_t maxLevel = 3;
    };

    // Closer than this, an object counts as touching the camera.
    inline constexpr float kNearDistance = 1.0f;

    // Material UV scales outside this range are treated as the nearest end:
    // 64 repeats or a 16th of the texture across a surface.
    inline constexpr float kMinUvScale = 1.0f / 16.0f;
    inline constexpr float kMaxUvScale = 64.0f;

    // The diameter of the object's bounding sphere on screen, in pixels,
    // measured at its nearest point.
    inline float ProjectedPixels(const View& a_view, const Placement& a_placement)
    {
        const float nearest = std::max(a_placement.distance - a_placement.radius, kNearDistance);
        return a_placement.radius * a_view.screenHeight / (nearest * a_view.tanHalfFovY);
    }

    // The tiling term from a material's UV scale, as Starfield takes it: the
    // smaller of the two axes, since the axis that repeats least shows the
    // most detail. 2 means the texture repeats twice across the surface.
    inline float UvScale(float a_u, float a_v)
    {
        if (!std::isfinite(a_u) || !std::isfinite(a_v)) {
            return 1.0f;
        }
        const float scale = std::min(std::fabs(a_u), std::fabs(a_v));
        if (!(scale > 0.0f)) {  // zero or NaN: no usable scale
            return 1.0f;
        }
        return std::clamp(scale, kMinUvScale, kMaxUvScale);
    }

    // Pixels one repeat of the texture covers.
    inline float PixelsPerRepeat(float a_pixels, float a_uvScale) { return a_pixels / a_uvScale; }

    // Estimate from a sphere's projected diameter and material UV scale.
    // This does not measure per-triangle UV derivatives or shader sampling.
    inline std::uint32_t VisibleLevelFromPixels(std::uint32_t a_textureSize, float a_pixels)
    {
        if (!std::isfinite(a_pixels)) {
            return 0;
        }
        if (a_textureSize == 0 || a_pixels >= static_cast<float>(a_textureSize)) {
            return 0;
        }

        // Too small to measure: no detail needed at all.
        if (a_pixels <= 1.0f) {
            return static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(a_textureSize))));
        }

        return static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(a_textureSize) / a_pixels)));
    }

    inline std::uint32_t VisibleLevel(const View& a_view, const Placement& a_placement)
    {
        return VisibleLevelFromPixels(a_placement.textureSize, ProjectedPixels(a_view, a_placement));
    }

    // The level a texture wants at the largest size it was seen at: what the
    // screen can't show, minus the margin, within what the backend can do.
    // a_pixels is per repeat (PixelsPerRepeat). Near the camera, nothing goes.
    inline std::uint32_t WantedLevel(const Policy& a_policy, std::uint32_t a_textureSize, float a_pixels, bool a_near)
    {
        if (a_near) {
            return 0;
        }
        const auto visible = VisibleLevelFromPixels(a_textureSize, a_pixels);
        const auto hidden = visible > a_policy.safetyMargin ? visible - a_policy.safetyMargin : 0;
        return std::min(hidden, a_policy.maxLevel);
    }

    // A rank inspired by Starfield: share of the screen per repeat, at
    // most 1; within the near distance, 2 to 3, closer ranking higher, so
    // anything near the camera comes before anything that isn't. Higher is
    // more important: upgraded first, degraded last. 0 means not seen.
    inline float Priority(float a_pixelsPerRepeat, float a_screenHeight, float a_nearest, float a_nearDistance)
    {
        float rank = a_screenHeight > 0.0f ? std::min(a_pixelsPerRepeat / a_screenHeight, 1.0f) : 0.0f;
        if (a_nearDistance > 0.0f && a_nearest < a_nearDistance) {
            rank = std::min(std::max(rank, 1.0f - a_nearest / a_nearDistance) + 2.0f, 3.0f);
        }
        return rank;
    }

    // Distance from the camera to the nearest point of a bounding sphere.
    inline float NearestDistance(float a_centreDistance, float a_radius)
    {
        return std::max(a_centreDistance - a_radius, 0.0f);
    }
}
