#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace NativeTextureLevels
{
    // FO4's deferred creator shifts BOTH dimensions without clamping to one.
    // Mip count alone cannot admit skinny textures: 512x4 at level3 is invalid.
    constexpr std::uint32_t Maximum(std::uint32_t width, std::uint32_t height,
        std::uint32_t mipCount, std::uint32_t policyMaximum, bool blockCompressed = false) noexcept
    {
        if (!width || !height || mipCount <= 1) return 0;
        const auto dimensionMaximum = static_cast<std::uint32_t>(std::bit_width(std::min(width, height)) - 1);
        const auto maximum = std::min({ dimensionMaximum, mipCount - 1, policyMaximum });
        if (!blockCompressed) return maximum;
        // A mip that becomes a new BC resource's TOP level must be 4-aligned
        // in both dimensions. Lower mips inside a chain may be smaller. Keep
        // every intermediate target legal: 520->260->130->65->32 cannot admit
        // level4 when the native scheduler may request level2 or level3.
        for (std::uint32_t level = 1; level <= maximum; ++level)
            if (((width >> level) & 3u) || ((height >> level) & 3u)) return level - 1;
        return maximum;
    }
}
