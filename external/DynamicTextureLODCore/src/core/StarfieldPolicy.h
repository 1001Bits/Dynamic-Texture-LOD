#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace dtl
{
    enum class DetailPolicy : std::uint8_t { Pixels = 0, Starfield = 1 };
    inline constexpr std::array<float,4> kStarfieldTextureThresholds{0.1f,0.05f,0.025f,0.0125f};
    inline constexpr std::array<float,4> kStarfieldTerrainDistances{12.5f,25.0f,50.0f,100.0f};
    inline constexpr std::array<float,4> kStarfieldMeshScreenSizes{0.2f,0.1f,0.05f,0.025f};
    inline constexpr std::array<float,4> kStarfieldMeshThresholds{0.15f,0.075f,0.0375f,0.01875f};

    inline std::array<float,4> StarfieldMeshThresholds(const std::array<float,4>& sizes=kStarfieldMeshScreenSizes,
        float fade=0.2f,float streamingBias=0.05f)
    {
        auto result=sizes;
        const float scale=1.0f-fade-streamingBias;
        for (auto& threshold : result) threshold*=scale;
        return result;
    }

    inline bool ValidStarfieldThresholds(const std::array<float,4>& thresholds)
    {
        for (std::size_t i=0;i<thresholds.size();++i)
            if (!std::isfinite(thresholds[i]) || thresholds[i] < 0 ||
                (i && thresholds[i] > thresholds[i-1])) return false;
        return true;
    }

    // Starfield 1.16.236 FUN_142a51110. maxHalfExtent must come from the
    // measured AABB; passing a sphere radius is a backend approximation.
    inline float StarfieldAngularSize(float maxHalfExtent, float centreDistance)
    {
        if (!std::isfinite(maxHalfExtent) || !std::isfinite(centreDistance) ||
            maxHalfExtent < 0.0f || centreDistance < 0.0f) return 0.0f;
        return 2.0f * maxHalfExtent / std::max(centreDistance, 1.0f);
    }

    // Starfield 1.16.236 FUN_142a512e0, verified instruction tail
    // 142a51460..142a514a2. Inputs are the source's camera-relative centre
    // and stored extents, after its instance-centre transform. This is NOT
    // nearest-point AABB distance: the sphere early-out is strict, and the
    // components abs(delta)-extent are squared without clamping to zero.
    // Invalid/overflowing inputs are outside the verified finite domain.
    // A backend must establish the source coordinate/extent units before
    // substituting this for its existing sphere-distance approximation.
    inline std::optional<float> StarfieldDistanceTerm(const std::array<float, 3>& delta,
                                                     const std::array<float, 3>& extents)
    {
        float centreSquared = 0.0f, extentSquared = 0.0f, differenceSquared = 0.0f;
        for (std::size_t i = 0; i < delta.size(); ++i) {
            if (!std::isfinite(delta[i]) || !std::isfinite(extents[i]) || extents[i] < 0.0f)
                return std::nullopt;
            centreSquared += delta[i] * delta[i];
            extentSquared += extents[i] * extents[i];
            const float difference = std::abs(delta[i]) - extents[i];
            differenceSquared += difference * difference;
        }
        if (!std::isfinite(centreSquared) || !std::isfinite(extentSquared)) return std::nullopt;
        if (centreSquared < extentSquared) return 0.0f;
        if (!std::isfinite(differenceSquared)) return std::nullopt;
        return std::sqrt(differenceSquared);
    }

    // Material -> texture demand, FUN_142a49490. A negative distance term
    // is the source's special-pool path: no UV division or near boost.
    inline float StarfieldTextureDemand(float angularSize, float u, float v,
                                       float surfaceDistance, float nearDistance)
    {
        if (!std::isfinite(angularSize) || angularSize < 0.0f ||
            !std::isfinite(surfaceDistance)) return 0.0f;
        float result = angularSize;
        if (surfaceDistance >= 0.0f) {
            // No legacy absolute-value/clamping policy here. The source uses
            // min(U,V), falling back to one for zero. Invalid inputs are held
            // conservatively at full demand by the caller/backend.
            const float scale = std::min(u, v);
            if (!std::isfinite(u) || !std::isfinite(v) || scale < 0.0f) return 3.0f;
            result = std::min(angularSize / (scale == 0.0f ? 1.0f : scale), 1.0f);
            if (std::isfinite(nearDistance) && nearDistance > 0.0f && surfaceDistance <= nearDistance)
                result = std::max(result, 1.0f - surfaceDistance / nearDistance) + 2.0f;
        }
        return std::min(result, 3.0f);
    }

    // SortLODs FUN_142a4c430. FUN_1428fc420 initializes request bias to one;
    // per-asset/material overrides remain undecoded. Using one for every
    // registered asset is still a backend fallback, not proven equivalence.
    inline float StarfieldTextureRank(float demand, bool seen, float distanceBias)
    {
        if (!std::isfinite(demand) || !std::isfinite(distanceBias)) return 0.0f;
        if (distanceBias == 0.0f) return demand + 2.0f;
        return seen && distanceBias > 0.0f ? std::max(demand / distanceBias, 0.0f) : 0.0f;
    }

    // UpdateTerrainTextureDetail -> FUN_142a4f2b0. These are source distance
    // units, not Skyrim/FO4 coordinates. The output is angular demand before
    // the ordinary texture UV/near/bias transform and maximum merge; it is
    // never a forced mip level. Backend terrain identity/units remain unproven.
    // Restrict this port to finite ordered settings with nonnegative output;
    // a rejected sample must preserve native detail, not become unseen demand.
    inline std::optional<float> StarfieldTerrainAngularDemand(float distance,
        const std::array<float,4>& bands = kStarfieldTerrainDistances,
        const std::array<float,4>& thresholds = kStarfieldTextureThresholds)
    {
        constexpr float epsilon = 0.005f;
        if (!std::isfinite(distance) || !ValidStarfieldThresholds(thresholds) || thresholds[3] < epsilon)
            return std::nullopt;
        for (std::size_t i = 0; i < bands.size(); ++i)
            if (!std::isfinite(bands[i]) || bands[i] <= 0 || (i && bands[i] < bands[i-1]))
                return std::nullopt;
        if (distance > bands[3]) return thresholds[3] - epsilon;
        if (distance > bands[2]) return thresholds[2] - epsilon;
        if (distance > bands[1]) return thresholds[1] - epsilon;
        if (distance > bands[0]) return thresholds[0] - epsilon;
        if (distance > 0) return thresholds[0] + epsilon;
        return 1.0f;
    }

    inline std::uint8_t StarfieldWantedLevel(float rank, std::uint8_t lowest,
        const std::array<float,4>& thresholds = kStarfieldTextureThresholds)
    {
        const std::uint8_t wanted = rank >= thresholds[0] ? 0 : rank >= thresholds[1] ? 1 :
                                    rank >= thresholds[2] ? 2 : rank >= thresholds[3] ? 3 : lowest;
        return std::min(wanted, lowest);
    }

    // Actual GPUCulling-8 DXIL, Starfield 1.16.236. The scale is 2^30-64,
    // deliberately distinct from the CPU decoder's 2^-30. Invalid input is
    // rejected; the shader's NaN conversion semantics have not been proven.
    inline std::optional<std::uint32_t> StarfieldGpuEncode(float demand)
    {
        if (!std::isfinite(demand)) return std::nullopt;
        return static_cast<std::uint32_t>(std::clamp(demand, 0.0f, 4.0f) * 1073741760.0f);
    }

    inline float StarfieldGpuDecode(std::uint32_t encoded)
    {
        return static_cast<float>(encoded) * 0x1p-30f;
    }

    // ScreenSizeLOD DXIL divides the encoded integer (converted to float),
    // not decoded demand, by max(1e-5,min(halfUVu,halfUVv)). No near boost.
    // Inputs must already be rounded to source half precision by the backend.
    // fptoui outside the uint range is not defined by the available evidence;
    // callers must preserve existing demand rather than invent saturation.
    inline std::optional<std::uint32_t> StarfieldGpuUvReduce(std::uint32_t encoded,
        float halfU, float halfV, bool tiled)
    {
        if (!tiled) return encoded;
        if (!std::isfinite(halfU) || !std::isfinite(halfV)) return std::nullopt;
        const float value = static_cast<float>(encoded) / std::max(0.00001f, std::min(halfU, halfV));
        if (!std::isfinite(value) || value < 0.0f || value >= 4294967296.0f) return std::nullopt;
        return static_cast<std::uint32_t>(value);
    }
}
