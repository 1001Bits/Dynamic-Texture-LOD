#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace dtl
{
    struct TextureIdentity
    {
        std::uint64_t key = 0;
        std::uint64_t generation = 0;
        bool operator==(const TextureIdentity&) const = default;
    };

    struct TextureIdentityHash
    {
        std::size_t operator()(const TextureIdentity& a_identity) const noexcept
        {
            return std::hash<std::uint64_t>{}(a_identity.key * 0x9E3779B97F4A7C15ull ^ a_identity.generation);
        }
    };

    enum class ReleaseEvidence : std::uint8_t
    {
        Unknown,                   // no source-equivalent release evidence
        GpuWorkComplete,           // a fence/event alone does not prove release
        EngineDeferredDestroyQueued,
        EngineDestroyObserved
    };

    struct StreamingAccounting
    {
        std::uint64_t requestedDecreaseBytes = 0;
        std::uint64_t requestedIncreaseBytes = 0;
        std::uint64_t releasedAwaitingDestructionBytes = 0;
        std::uint64_t observedDestroyedBytes = 0;
        std::uint64_t pendingOperations = 0;
        // Lowerings requested within the snapshot's credit window: still on
        // their way, so not yet in a video memory reading.
        std::uint64_t recentRequestedDecreaseBytes = 0;
    };

    // Logical level changes and actual engine destruction are separate
    // channels. CompleteChange/GPU event completion never create free credit.
    // Byte sizes remain resource accounting, not measured physical VRAM.
    class StreamingLedger
    {
    public:
        using Ticket = std::uint64_t;
        using Clock = std::chrono::steady_clock;
        static constexpr std::chrono::milliseconds kCreditWindow{ 2000 };

        Ticket Request(TextureIdentity identity, std::uint64_t oldBytes, std::uint64_t newBytes);
        bool CompleteChange(Ticket ticket, TextureIdentity identity);
        bool CancelChange(Ticket ticket, TextureIdentity identity);
        bool Pending(TextureIdentity identity) const;
        Ticket QueueRelease(TextureIdentity identity, std::uint64_t bytes, ReleaseEvidence evidence);
        bool CompleteRelease(Ticket ticket, ReleaseEvidence evidence);
        bool RevokeRelease(Ticket ticket);
        StreamingAccounting Snapshot(std::chrono::milliseconds a_creditWindow = kCreditWindow) const;
    private:
        struct Change { TextureIdentity identity; std::uint64_t oldBytes, newBytes; Clock::time_point at; };
        struct Release { TextureIdentity identity; std::uint64_t bytes; };
        mutable std::mutex mutex_;
        std::unordered_map<Ticket, Change> changes_;
        // One pending change per texture: checked on every request.
        std::unordered_set<TextureIdentity, TextureIdentityHash> pending_;
        std::unordered_map<Ticket, Release> releases_;
        Ticket next_ = 1;
        std::uint64_t destroyed_ = 0;
    };
}
