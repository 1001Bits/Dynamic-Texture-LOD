#pragma once

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace GpuBounds
{
    struct Bounds { std::array<float, 3> center{}, halfExtent{}; };

    // NiFrustum uses normalized perspective spans in all verified FO4 runtimes.
    // Starfield GPU demand consumes half the larger projection diagonal.
    // Near distance belongs to the depth terms and must not scale this value.
    inline std::optional<float> PerspectiveViewScale(float left, float right,
                                                     float top, float bottom) noexcept
    {
        if (!std::isfinite(left) || !std::isfinite(right) ||
            !std::isfinite(top) || !std::isfinite(bottom)) return {};
        const float horizontal = right - left, vertical = top - bottom;
        if (!std::isfinite(horizontal) || !std::isfinite(vertical) ||
            horizontal <= 0 || vertical <= 0) return {};
        const float scale = std::max(1.0f / horizontal, 1.0f / vertical);
        if (!std::isfinite(scale) || scale <= 0) return {};
        return scale;
    }

    inline float Half(std::uint16_t bits) noexcept
    {
        const auto sign = static_cast<std::uint32_t>(bits & 0x8000) << 16;
        auto mantissa = static_cast<std::uint32_t>(bits & 1023);
        auto exponent = static_cast<std::uint32_t>((bits >> 10) & 31);
        if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
        if (!exponent) {
            if (!mantissa) return std::bit_cast<float>(sign);
            return std::copysign(std::ldexp(static_cast<float>(mantissa), -24), sign ? -1.0f : 1.0f);
        }
        return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    }

    // FO4 native CreateInputLayout: POSITION is offset0. VF_VERTEX+FULLPREC
    // selects R32G32B32A32_FLOAT; VF_VERTEX without FULLPREC selects half4.
    inline std::optional<Bounds> Decode(std::span<const std::byte> data, std::uint32_t stride,
                                       std::uint64_t descriptor) noexcept
    {
        constexpr std::uint64_t vertex = 1ull << 44, full = 1ull << 54, skinned = 1ull << 50;
        const auto positionBytes = (descriptor & full) ? 16u : 8u;
        if (!(descriptor & vertex) || (descriptor & skinned) || stride < positionBytes ||
            stride != (descriptor & 15) * 4 || data.empty() || data.size() % stride ||
            data.size() > 64u * 1024u * 1024u) return {};
        std::array<float, 3> low{}, high{};
        for (std::size_t offset = 0; offset < data.size(); offset += stride) {
            for (std::size_t axis = 0; axis != 3; ++axis) {
                float value{};
                if (descriptor & full) std::memcpy(&value, data.data() + offset + axis * 4, 4);
                else {
                    std::uint16_t half{};
                    std::memcpy(&half, data.data() + offset + axis * 2, 2);
                    value = Half(half);
                }
                if (!std::isfinite(value)) return {};
                if (!offset) low[axis] = high[axis] = value;
                else { low[axis] = std::min(low[axis], value); high[axis] = std::max(high[axis], value); }
            }
        }
        Bounds result;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            result.center[axis] = low[axis] * 0.5f + high[axis] * 0.5f;
            result.halfExtent[axis] = high[axis] * 0.5f - low[axis] * 0.5f;
            if (!std::isfinite(result.halfExtent[axis])) return {};
        }
        return result;
    }

    // Native FO4 NiBound::Update (OG1BB19A0) treats the three padded vectors
    // as COLUMNS: worldX = x*M[0] + y*M[4] + z*M[8]. CommonLib's operator*
    // uses the opposite convention, so do not substitute it here.
    inline std::optional<Bounds> Transform(const Bounds& local, const float* paddedMatrix,
                                          const float* translation, float scale) noexcept
    {
        if (!std::isfinite(scale)) return {};
        Bounds result;
        for (std::size_t row = 0; row != 3; ++row) {
            result.center[row] = translation[row];
            for (std::size_t column = 0; column != 3; ++column) {
                const float coefficient = paddedMatrix[column * 4 + row] * scale;
                result.center[row] += coefficient * local.center[column];
                result.halfExtent[row] += std::abs(coefficient) * local.halfExtent[column];
            }
            if (!std::isfinite(result.center[row]) || !std::isfinite(result.halfExtent[row])) return {};
        }
        return result;
    }
}
