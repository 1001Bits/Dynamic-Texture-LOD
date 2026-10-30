// Tests for the game-independent core: the screen-size LOD policy and the
// planner. Returns the number of failed checks.

#include "core/DdsHeader.h"
#include "core/Frustum.h"
#include "core/LodPolicy.h"
#include "core/Observation.h"
#include "core/Planner.h"
#include "core/TextureFormat.h"

#include <array>
#include <span>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace
{
    int g_failures = 0;

#define CHECK(a_condition)                                                        \
    do {                                                                          \
        if (!(a_condition)) {                                                     \
            std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #a_condition); \
            ++g_failures;                                                         \
        }                                                                         \
    } while (false)

    constexpr std::uint64_t MB(std::uint64_t a_megabytes) { return a_megabytes << 20; }

    bool Near(float a_left, float a_right) { return std::fabs(a_left - a_right) < 0.01f; }

    // ── LodPolicy ────────────────────────────────────────────────────────────

    constexpr dtl::View kView{ 1080.0f, 1.0f };

    void ProjectedPixels()
    {
        // Radius 1 whose nearest point is 10 away, 1080 lines over a view
        // that spans 2 units at distance 1: 108 pixels.
        CHECK(Near(dtl::ProjectedPixels(kView, { 4096, 1.0f, 11.0f }), 108.0f));

        // Inside the sphere counts as touching the camera.
        CHECK(Near(dtl::ProjectedPixels(kView, { 4096, 10.0f, 5.0f }), 10800.0f));
    }

    void VisibleLevel()
    {
        // 4096 texels over 108 pixels: 37.9 texels a pixel, five whole levels.
        CHECK(dtl::VisibleLevel(kView, { 4096, 1.0f, 11.0f }) == 5);

        // Bigger on screen than the texture: nothing to drop.
        CHECK(dtl::VisibleLevel(kView, { 4096, 100.0f, 101.0f }) == 0);

        CHECK(dtl::VisibleLevel(kView, { 0, 1.0f, 11.0f }) == 0);

        // Too small to measure: everything but the last level.
        CHECK(dtl::VisibleLevel(kView, { 4096, 0.0f, 50.0f }) == 12);

        // Moving closer never asks for less detail.
        std::uint32_t previous = 99;
        for (float distance = 5000.0f; distance > 1.0f; distance *= 0.9f) {
            const auto level = dtl::VisibleLevel(kView, { 2048, 3.0f, distance });
            CHECK(level <= previous);
            previous = level;
        }

        CHECK(dtl::VisibleLevelFromPixels(4096, 108.0f) == 5);
        CHECK(dtl::VisibleLevelFromPixels(4096, 0.5f) == 12);
        CHECK(dtl::VisibleLevelFromPixels(4096, std::numeric_limits<float>::quiet_NaN()) == 0);
        CHECK(dtl::VisibleLevelFromPixels(4096, std::numeric_limits<float>::infinity()) == 0);
    }

    void UvScale()
    {
        CHECK(Near(dtl::UvScale(2.0f, 4.0f), 2.0f));     // the axis that repeats least
        CHECK(Near(dtl::UvScale(-0.5f, 1.0f), 0.5f));    // mirrored still counts
        CHECK(Near(dtl::UvScale(0.0f, 1.0f), 1.0f));     // no usable scale
        CHECK(Near(dtl::UvScale(1000.0f, 1000.0f), dtl::kMaxUvScale));
        CHECK(Near(dtl::UvScale(std::numeric_limits<float>::quiet_NaN(), 2.0f), 1.0f));
        CHECK(Near(dtl::UvScale(2.0f, std::numeric_limits<float>::quiet_NaN()), 1.0f));
        CHECK(Near(dtl::UvScale(2.0f, std::numeric_limits<float>::infinity()), 1.0f));
        CHECK(Near(dtl::PixelsPerRepeat(256.0f, 4.0f), 64.0f));
    }

    void WantedLevel()
    {
        const dtl::Policy policy{ 1, 3 };

        // Visible level 5, one kept as margin, clamped to what the backend can drop.
        CHECK(dtl::WantedLevel(policy, 4096, 108.0f, false) == 3);
        // Visible level 1 is all margin.
        CHECK(dtl::WantedLevel(policy, 256, 108.0f, false) == 0);
        // Near the camera nothing goes, however small.
        CHECK(dtl::WantedLevel(policy, 4096, 2.0f, true) == 0);

        // A texture repeating 4 times shows a quarter of the pixels per
        // repeat: more can go. One magnified 4 times needs more.
        const dtl::Policy deep{ 1, 8 };
        CHECK(dtl::WantedLevel(deep, 1024, 256.0f, false) == 1);
        CHECK(dtl::WantedLevel(deep, 1024, dtl::PixelsPerRepeat(256.0f, 4.0f), false) == 3);
        CHECK(dtl::WantedLevel(deep, 1024, dtl::PixelsPerRepeat(256.0f, 0.25f), false) == 0);
    }

    void Priority()
    {
        CHECK(Near(dtl::Priority(540.0f, 1080.0f, 5000.0f, 875.0f), 0.5f));
        CHECK(Near(dtl::Priority(5000.0f, 1080.0f, 5000.0f, 875.0f), 1.0f));  // at most the whole screen

        // Inside the near distance: 2 to 3, closer higher, above anything not near.
        CHECK(Near(dtl::Priority(10.0f, 1080.0f, 0.0f, 875.0f), 3.0f));
        CHECK(Near(dtl::Priority(10.0f, 1080.0f, 437.5f, 875.0f), 2.5f));
        CHECK(dtl::Priority(1.0f, 1080.0f, 870.0f, 875.0f) > dtl::Priority(5000.0f, 1080.0f, 900.0f, 875.0f));

        // Turned off.
        CHECK(Near(dtl::Priority(540.0f, 1080.0f, 0.0f, 0.0f), 0.5f));

        CHECK(Near(dtl::NearestDistance(100.0f, 30.0f), 70.0f));
        CHECK(Near(dtl::NearestDistance(10.0f, 30.0f), 0.0f));
    }

    // ── DDS header and mip sizes ─────────────────────────────────────────────

    std::vector<std::byte> MakeDds(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips,
        std::uint32_t a_fourCC, std::uint32_t a_caps2 = 0)
    {
        std::vector<std::byte> file(4 + 124 + 20);
        const auto put = [&](std::size_t a_offset, std::uint32_t a_value) {
            std::memcpy(file.data() + a_offset, &a_value, sizeof(a_value));
        };
        put(0x00, 0x20534444);  // "DDS "
        put(0x04, 124);
        put(0x0C, a_height);
        put(0x10, a_width);
        put(0x1C, a_mips);
        put(0x50, 0x4);  // DDPF_FOURCC
        put(0x54, a_fourCC);
        put(0x70, a_caps2);
        return file;
    }

    void DdsHeader()
    {
        constexpr std::uint32_t kDxt5 = 0x35545844;
        constexpr std::uint32_t kDx10 = 0x30315844;

        const auto plain = dtl::dds::Parse(MakeDds(512, 256, 10, kDxt5));
        CHECK(plain.has_value());
        if (plain) {
            CHECK(plain->width == 512);
            CHECK(plain->height == 256);
            CHECK(plain->mipCount == 10);
            CHECK(plain->dataOffset == 128);
            CHECK(!plain->dx10);
            CHECK(!plain->cubemap);
        }

        const auto extended = dtl::dds::Parse(MakeDds(64, 64, 0, kDx10, 0x200));
        CHECK(extended.has_value());
        if (extended) {
            CHECK(extended->dataOffset == 148);
            CHECK(extended->mipCount == 1);
            CHECK(extended->cubemap);
        }

        auto notDds = MakeDds(64, 64, 1, kDxt5);
        notDds[0] = std::byte{ 'X' };
        CHECK(!dtl::dds::Parse(notDds).has_value());

        const std::array<std::byte, 16> tooShort{};
        CHECK(!dtl::dds::Parse(tooShort).has_value());
    }

    void MipSizes()
    {
        // BC1 1024: half a byte per texel.
        CHECK(TextureFormat::MipBytes(71, 1024, 1024) == 1024 * 1024 / 2);
        // A whole chain is the top level times about 4/3, every level at least one block.
        CHECK(TextureFormat::ChainBytes(77, 4, 4, 0, 3) == 16 * 3);
        // Skipping levels is the same as starting the chain smaller.
        CHECK(TextureFormat::ChainBytes(71, 2048, 2048, 2, 3) == TextureFormat::ChainBytes(71, 512, 512, 0, 3));
        CHECK(!TextureFormat::IsSupported(66));
        CHECK(TextureFormat::IsSupported(87));
    }

    // ── Planner ──────────────────────────────────────────────────────────────

    // An 8 GB card with Starfield's lines: the budget line at 8064 MB, the
    // target at 7552 MB and the upgrade ceiling at about 7884 MB.
    constexpr std::uint64_t kOsBudget = MB(8192);
    const dtl::BudgetLines  kLines{};

    dtl::Budget At(std::uint64_t a_usageMB) { return dtl::Assess(kLines, MB(a_usageMB), kOsBudget); }

    // A BC1 texture, square, with a full chain.
    dtl::PlanEntry Texture(std::uint64_t a_key, float a_priority, std::uint8_t a_current, std::uint8_t a_wanted,
        std::uint32_t a_size = 4096, bool a_seen = true)
    {
        dtl::PlanEntry entry;
        entry.key = a_key;
        entry.priority = a_priority;
        entry.seen = a_seen;
        entry.current = a_current;
        entry.previous = a_current;
        entry.wanted = a_wanted;
        entry.lowest = 3;
        const auto mips = static_cast<std::uint32_t>(std::log2(static_cast<float>(a_size))) + 1;
        dtl::FillBytes(entry, 71, a_size, a_size, mips);
        return entry;
    }

    const dtl::PlanEntry& Find(std::span<const dtl::PlanEntry> a_entries, std::uint64_t a_key)
    {
        for (const auto& entry : a_entries) {
            if (entry.key == a_key) {
                return entry;
            }
        }
        static const dtl::PlanEntry kMissing{};
        CHECK(false);
        return kMissing;
    }

    void Assess()
    {
        const auto room = At(7000);
        CHECK(room.mode == dtl::Mode::Room);
        CHECK(room.line == MB(8064));
        CHECK(room.target == MB(7552));
        CHECK(room.ceiling > MB(7884) && room.ceiling < MB(7885));

        CHECK(At(7552).mode == dtl::Mode::Tight);
        CHECK(At(8064).mode == dtl::Mode::Tight);
        CHECK(At(8065).mode == dtl::Mode::Over);
        CHECK(dtl::Assess(kLines, MB(9000), 0).mode == dtl::Mode::Unlimited);
    }

    void LowersToWanted()
    {
        std::vector entries{ Texture(1, 0.5f, 0, 2) };
        const auto result = dtl::Plan(entries, At(7000), kLines, {});
        CHECK(entries[0].planned == 2);
        CHECK(result.lowered == entries[0].bytes[0] - entries[0].bytes[2]);
        CHECK(result.raised == 0);
    }

    void UpgradesMostImportantFirst()
    {
        // Each 4096 BC1 coming back from level 3 costs about 11 MB; the slice
        // is 34 MB, and the last upgrade may overshoot it.
        std::vector<dtl::PlanEntry> entries;
        for (std::uint64_t key = 1; key <= 6; ++key) {
            entries.push_back(Texture(key, 1.0f - 0.1f * static_cast<float>(key), 3, 0));
        }
        const auto result = dtl::Plan(entries, At(7000), kLines, {});

        for (std::uint64_t key = 1; key <= 4; ++key) {
            CHECK(Find(entries, key).planned == 0);
        }
        CHECK(Find(entries, 5).planned == 3);
        CHECK(Find(entries, 6).planned == 3);
        CHECK(result.upgradesHeld == 2);
        CHECK(entries.front().key == 1);  // sorted, most important first
    }

    void NoLimitsWhenUnlimited()
    {
        std::vector<dtl::PlanEntry> entries;
        for (std::uint64_t key = 1; key <= 20; ++key) {
            entries.push_back(Texture(key, 0.5f, 3, 0));
        }
        const auto result = dtl::Plan(entries, dtl::Budget{}, kLines, {});
        CHECK(result.upgradesHeld == 0);
        for (const auto& entry : entries) {
            CHECK(entry.planned == 0);
        }
    }

    void SwapsAboveTheCeiling()
    {
        // Above the ceiling: no slice. The most important upgrade is paid for
        // by the least important textures dropping one level further.
        std::vector entries{ Texture(1, 0.9f, 2, 0, 1024) };
        for (std::uint64_t key = 10; key < 20; ++key) {
            entries.push_back(Texture(key, 0.01f, 0, 0, 1024));
        }
        const auto result = dtl::Plan(entries, At(7900), kLines, {});

        CHECK(result.swapped);
        CHECK(Find(entries, 1).planned == 0);
        // About 655 KB to pay, 524 KB a texture: the two last in line.
        CHECK(Find(entries, 19).planned == 1);
        CHECK(Find(entries, 18).planned == 1);
        CHECK(Find(entries, 17).planned == 0);
        CHECK(result.traded >= result.raised);
    }

    void SwapOnlyWhenPaidInFull()
    {
        std::vector entries{ Texture(1, 0.9f, 3, 0, 4096), Texture(2, 0.01f, 0, 0, 512) };
        const auto result = dtl::Plan(entries, At(7900), kLines, {});
        CHECK(!result.swapped);
        CHECK(Find(entries, 1).planned == 3);
        CHECK(Find(entries, 2).planned == 0);
        CHECK(result.upgradesHeld == 1);
    }

    void EmergencyFreesTheOverage()
    {
        // 20 MB over the line; each texture frees about 11 MB at its lowest.
        std::vector<dtl::PlanEntry> entries;
        for (std::uint64_t key = 1; key <= 10; ++key) {
            entries.push_back(Texture(key, 1.0f - 0.09f * static_cast<float>(key), 0, 0));
        }
        entries.push_back(Texture(99, 0.95f, 3, 0));  // wants to come back
        const auto result = dtl::Plan(entries, At(8084), kLines, {});

        CHECK(result.mode == dtl::Mode::Over);
        CHECK(result.emergency >= MB(20));
        CHECK(Find(entries, 10).planned == 3);
        CHECK(Find(entries, 9).planned == 3);
        CHECK(Find(entries, 8).planned == 0);
        CHECK(Find(entries, 1).planned == 0);
        CHECK(Find(entries, 99).planned == 3);  // nothing comes back over the line
    }

    void EmergencyCountsWhatIsOnItsWay()
    {
        // 20 MB over, but 22 MB of lowerings asked for earlier aren't done yet.
        std::vector<dtl::PlanEntry> entries;
        for (std::uint64_t key = 1; key <= 4; ++key) {
            auto entry = Texture(key, 0.5f, 0, 0);
            entry.previous = key <= 2 ? 3 : 0;
            entries.push_back(entry);
        }
        const auto result = dtl::Plan(entries, At(8084), kLines, {});
        CHECK(result.pendingFree >= MB(20));
        CHECK(result.emergency == 0);
        CHECK(Find(entries, 1).planned == 3);  // still asked for
        CHECK(Find(entries, 3).planned == 0);
    }

    void UnseenTextures()
    {
        const auto plan = [](dtl::Unseen a_unseen, std::uint64_t a_usageMB) {
            std::vector entries{ Texture(1, 0.0f, 0, 0, 4096, false) };
            dtl::Plan(entries, At(a_usageMB), kLines, { a_unseen });
            return entries[0].planned;
        };
        CHECK(plan(dtl::Unseen::Keep, 7900) == 0);
        CHECK(plan(dtl::Unseen::UnderPressure, 7000) == 0);
        CHECK(plan(dtl::Unseen::UnderPressure, 7600) == 3);
        CHECK(plan(dtl::Unseen::Lowest, 7000) == 3);

        // Unseen ones are last in line, behind even the smallest seen one.
        std::vector entries{ Texture(1, 0.0f, 0, 0, 4096, false), Texture(2, 0.001f, 0, 0) };
        dtl::Plan(entries, At(7000), kLines, {});
        CHECK(entries.back().key == 1);
    }

    void EarlierUpgradesStand()
    {
        // Granted last cycle, not loaded yet: it stays granted without being
        // paid for twice, unless the card goes over the line.
        auto entry = Texture(1, 0.5f, 3, 0);
        entry.previous = 0;

        std::vector entries{ entry };
        auto result = dtl::Plan(entries, At(7000), kLines, {});
        CHECK(entries[0].planned == 0);
        CHECK(result.raised == 0);
        CHECK(result.pendingGrow > 0);

        entries = { entry };
        dtl::Plan(entries, At(8100), kLines, {});
        CHECK(entries[0].planned == 3);
    }

    void SlowTextureRounds()
    {
        dtl::Observation observation;
        observation.Visit(10, true, 1024.0f, 2.5f);
        // Any number of planner ticks within a long native traversal must
        // retain its size and visibility, independent of elapsed seconds.
        for (int tick = 0; tick < 300; ++tick) {
            observation.Advance(10);
            CHECK(observation.Seen(10));
            CHECK(observation.Pixels() == 1024.0f);
        }
        observation.Visit(10, true, 32.0f, 0.01f);
        CHECK(observation.Pixels() == 1024.0f);  // shared texture: largest user
        observation.Advance(11);
        observation.Visit(11, false, 0.0f, 0.0f);
        CHECK(observation.Seen(11));
        CHECK(observation.Priority() == 2.5f);
        observation.Advance(12);
        CHECK(!observation.Seen(12));  // a complete round without a visible use
        CHECK(observation.Pixels() == 0.0f);
        observation.Visit(12, true, 512.0f, 1.0f);
        CHECK(observation.Seen(12));
        CHECK(observation.Pixels() == 512.0f);
    }

    void CameraFrustum()
    {
        CHECK(dtl::SphereInFrustum(10, 0, 0, 1, -1, 1, -1, 1));
        CHECK(!dtl::SphereInFrustum(-10, 0, 0, 1, -1, 1, -1, 1));
        CHECK(!dtl::SphereInFrustum(10, 12, 0, 1, -1, 1, -1, 1));
        CHECK(dtl::SphereInFrustum(10, 11, 0, 1, -1, 1, -1, 1));  // intersects side
        CHECK(!dtl::SphereInFrustum(10, 0, -12, 1, -1, 1, -1, 1));
        CHECK(dtl::SphereInFrustum(0, 0, 0, 1, -1, 1, -1, 1));
        CHECK(dtl::SphereInFrustum(10, 9, 0, 0.5f, -0.5f, 1.5f, -1, 1));
        CHECK(!dtl::SphereInFrustum(10, -9, 0, 0.5f, -0.5f, 1.5f, -1, 1));
        CHECK(dtl::SphereInFrustum(-10, 0, 0, 1, -1, 1, -1, 1, true));
    }

    void FailedRestoreCancelsPendingLowering()
    {
        auto entry = Texture(1, 0.5f, 1, 0);
        entry.previous = 3;
        entry.lowest = 1;  // file verification failed: may not lower further
        std::vector entries{entry};
        const auto result = dtl::Plan(entries, At(8100), kLines, {});
        CHECK(entries[0].planned == 1);
        CHECK(result.pendingFree == 0);
        CHECK(result.emergency == 0);

        // A backend that cannot reload also pins its wanted level. Neither
        // a pending lowering nor an old upgrade may survive that pin.
        entry.wanted = entry.current;
        for (const auto previous : { std::uint8_t{0}, std::uint8_t{3} }) {
            entry.previous = previous;
            entries = { entry };
            const auto pinned = dtl::Plan(entries, At(7000), kLines, {});
            CHECK(entries[0].planned == entry.current);
            CHECK(pinned.raised == 0);
            CHECK(pinned.lowered == 0);
        }
    }
}

int main()
{
    ProjectedPixels();
    VisibleLevel();
    UvScale();
    WantedLevel();
    Priority();
    DdsHeader();
    MipSizes();

    Assess();
    LowersToWanted();
    UpgradesMostImportantFirst();
    NoLimitsWhenUnlimited();
    SwapsAboveTheCeiling();
    SwapOnlyWhenPaidInFull();
    EmergencyFreesTheOverage();
    EmergencyCountsWhatIsOnItsWay();
    UnseenTextures();
    EarlierUpgradesStand();
    SlowTextureRounds();
    CameraFrustum();
    FailedRestoreCancelsPendingLowering();

    if (g_failures == 0) {
        std::printf("All core tests passed\n");
    }
    return g_failures;
}
