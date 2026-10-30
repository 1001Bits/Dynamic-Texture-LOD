#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// How much video memory to keep free, from the largest jump actually seen
// (Skyrim Performance Solution's reserve recommendation): the largest rise
// over the lowest reading of the last 3 seconds, outside loading screens and
// this plugin's own refill. Pure and clock-free, for tests.
namespace dtl
{
    class ReserveMeter
    {
    public:
        static constexpr std::int64_t  kBucketMs = 250;
        static constexpr std::size_t   kBuckets = 12;  // 3 s
        static constexpr std::int64_t  kPlayNeededMs = 3 * 60 * 1000;
        static constexpr std::uint64_t kStep = 256ull << 20;
        static constexpr std::uint64_t kLeast = 512ull << 20;
        static constexpr std::uint64_t kMost = 6144ull << 20;

        // A reading during play.
        void Add(std::int64_t a_nowMs, std::uint64_t a_usage)
        {
            // Play time counts between readings, at most a second each, so a
            // pause or a stall adds nothing.
            if (m_lastMs >= 0) m_playMs += std::clamp<std::int64_t>(a_nowMs - m_lastMs, 0, 1000);
            m_lastMs = a_nowMs;
            const auto id = a_nowMs / kBucketMs;
            auto& bucket = m_buckets[static_cast<std::size_t>(id % static_cast<std::int64_t>(kBuckets))];
            if (bucket.id != id) bucket = { id, a_usage };
            else bucket.low = std::min(bucket.low, a_usage);
            std::uint64_t low = a_usage;
            for (const auto& other : m_buckets)
                if (other.id >= 0 && other.id > id - static_cast<std::int64_t>(kBuckets) && other.id <= id)
                    low = std::min(low, other.low);
            m_largest = std::max(m_largest, a_usage - low);
        }

        // A loading screen, a refill or a test's pretend usage: what follows
        // is not compared with what came before. Play time and the largest
        // jump so far are kept.
        void Break()
        {
            m_buckets.fill({});
            m_lastMs = -1;
        }

        std::uint64_t Largest() const { return m_largest; }
        std::int64_t  PlayMs() const { return m_playMs; }
        bool          Ready() const { return m_playMs >= kPlayNeededMs; }

        // The largest jump plus a quarter, in whole 256 MB steps, 512 MB to
        // 6 GB, and never more than a quarter of the OS budget.
        static std::uint64_t KeepFree(std::uint64_t a_largest, std::uint64_t a_osBudget)
        {
            const auto wanted = (a_largest + a_largest / 4 + kStep - 1) / kStep * kStep;
            auto keep = std::clamp(wanted, kLeast, kMost);
            if (a_osBudget) keep = std::min(keep, a_osBudget / 4);
            return keep;
        }

    private:
        struct Bucket
        {
            std::int64_t  id = -1;
            std::uint64_t low = 0;
        };
        std::array<Bucket, kBuckets> m_buckets{};
        std::int64_t  m_lastMs = -1;  // -1: no reading since the last break
        std::int64_t  m_playMs = 0;
        std::uint64_t m_largest = 0;
    };
}
