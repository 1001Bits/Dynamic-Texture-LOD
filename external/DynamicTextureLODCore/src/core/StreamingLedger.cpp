#include "StreamingLedger.h"

#include <limits>

namespace dtl
{
    namespace
    {
        std::uint64_t Add(std::uint64_t a, std::uint64_t b)
        {
            return b > std::numeric_limits<std::uint64_t>::max() - a ?
                std::numeric_limits<std::uint64_t>::max() : a + b;
        }
    }

    StreamingLedger::Ticket StreamingLedger::Request(TextureIdentity identity, std::uint64_t oldBytes,
        std::uint64_t newBytes)
    {
        std::scoped_lock lock(mutex_);
        if (oldBytes == newBytes || next_ == 0) return 0;
        if (!pending_.insert(identity).second) return 0;
        const auto ticket = next_++;
        changes_.emplace(ticket, Change{ identity, oldBytes, newBytes, Clock::now() });
        return ticket;
    }

    bool StreamingLedger::CompleteChange(Ticket ticket, TextureIdentity identity)
    {
        std::scoped_lock lock(mutex_);
        const auto found = changes_.find(ticket);
        if (found == changes_.end() || !(found->second.identity == identity)) return false;
        pending_.erase(identity);
        changes_.erase(found);
        return true;
    }

    bool StreamingLedger::CancelChange(Ticket ticket, TextureIdentity identity)
    {
        return CompleteChange(ticket, identity);
    }

    bool StreamingLedger::Pending(TextureIdentity identity) const
    {
        std::scoped_lock lock(mutex_);
        return pending_.contains(identity);
    }

    StreamingLedger::Ticket StreamingLedger::QueueRelease(TextureIdentity identity, std::uint64_t bytes,
        ReleaseEvidence evidence)
    {
        std::scoped_lock lock(mutex_);
        if (evidence != ReleaseEvidence::EngineDeferredDestroyQueued || bytes == 0 || next_ == 0) return 0;
        const auto ticket = next_++;
        releases_.emplace(ticket, Release{ identity, bytes });
        return ticket;
    }

    bool StreamingLedger::CompleteRelease(Ticket ticket, ReleaseEvidence evidence)
    {
        std::scoped_lock lock(mutex_);
        const auto found = releases_.find(ticket);
        if (found == releases_.end() || evidence != ReleaseEvidence::EngineDestroyObserved) return false;
        destroyed_ = Add(destroyed_, found->second.bytes);
        releases_.erase(found);
        return true;
    }

    bool StreamingLedger::RevokeRelease(Ticket ticket)
    {
        std::scoped_lock lock(mutex_);
        return releases_.erase(ticket) != 0;
    }

    StreamingAccounting StreamingLedger::Snapshot(std::chrono::milliseconds a_creditWindow) const
    {
        const auto now = Clock::now();
        std::scoped_lock lock(mutex_);
        StreamingAccounting result;
        result.pendingOperations = changes_.size();
        result.observedDestroyedBytes = destroyed_;
        for (const auto& [ticket, change] : changes_) {
            if (change.oldBytes > change.newBytes) {
                result.requestedDecreaseBytes = Add(result.requestedDecreaseBytes, change.oldBytes - change.newBytes);
                if (now - change.at <= a_creditWindow)
                    result.recentRequestedDecreaseBytes =
                        Add(result.recentRequestedDecreaseBytes, change.oldBytes - change.newBytes);
            } else
                result.requestedIncreaseBytes = Add(result.requestedIncreaseBytes, change.newBytes - change.oldBytes);
        }
        for (const auto& [ticket, release] : releases_)
            result.releasedAwaitingDestructionBytes = Add(result.releasedAwaitingDestructionBytes, release.bytes);
        return result;
    }
}
