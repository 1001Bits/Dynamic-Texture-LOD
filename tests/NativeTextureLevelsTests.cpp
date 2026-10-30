#include "../src/NativeTextureLevels.h"
#include <cassert>
#include <cstdint>

int main()
{
    using NativeTextureLevels::Maximum;
    static_assert(Maximum(512, 4, 10, 3) == 2);
    static_assert(Maximum(4, 512, 10, 4) == 2);
    static_assert(Maximum(512, 1, 10, 4) == 0);
    static_assert(Maximum(15, 512, 10, 4) == 3);
    static_assert(Maximum(16, 512, 10, 4) == 4);
    static_assert(Maximum(16, 16, 4, 4) == 3);
    static_assert(Maximum(16384, 16384, 15, 4) == 4);
    static_assert(Maximum(0, 512, 10, 4) == 0);
    static_assert(Maximum(512, 512, 0, 4) == 0);
    static_assert(Maximum(512, 512, 1, 4) == 0);
    static_assert(Maximum(2, 2, 2, 4) == 1);
    static_assert(Maximum(512, 4, 10, 4, true) == 0);
    static_assert(Maximum(512, 16, 10, 4, true) == 2);
    static_assert(Maximum(512, 64, 10, 4, true) == 4);
    static_assert(Maximum(132, 516, 10, 4, true) == 0);
    static_assert(Maximum(520, 512, 10, 4, true) == 1);
    // Check the actual creator invariants over asymmetric, non-power-of-two
    // dimensions and truncated chains rather than reproducing its formula.
    for (std::uint32_t w = 1; w < 513; ++w)
        for (std::uint32_t h : { 1u, 2u, 3u, 4u, 15u, 16u, 255u, 512u })
            for (std::uint32_t mips = 1; mips < 12; ++mips) {
                const auto level = Maximum(w, h, mips, 4);
                assert(level <= 4 && level < mips);
                assert((w >> level) > 0 && (h >> level) > 0);
                if (level < 4 && level+1 < mips)
                    assert((w >> (level+1)) == 0 || (h >> (level+1)) == 0);
            }
}
