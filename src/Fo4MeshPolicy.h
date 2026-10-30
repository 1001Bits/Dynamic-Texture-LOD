#pragma once
#include "Fo4MeshData.h"
#include "core/Observation.h"
#include "core/StarfieldPlanner.h"

namespace Fo4Mesh {
    struct PublicationGuards {
        bool healthy{},unique{},pinned{},forceFull{};
    };
    // A known native fallback pass can use an already-full private fixture
    // without pinning its future policy. Reduced pairs still require the
    // pre-Setup full-source scope; this never admits an unknown consumer or
    // bypasses high-bit mode, source identity, or ownership checks.
    constexpr bool KeepFullNativePass(bool privatePilot, bool knownConsumer,
        bool identityAndOwnership, bool highBitMode, std::uint8_t current) noexcept {
        return privatePilot && knownConsumer && identityAndOwnership && !highBitMode && current==0;
    }
    // CPU planner state for one enrolled native shape. The renderer registry
    // owns synchronization and validates native wrapper identity before every
    // call that can accept or publish work. No engine pointers are read here.
    class PolicyState {
    public:
        PolicyState(dtl::TextureIdentity identity, std::uint64_t revision, PrefixLevels levels,
                    std::uint64_t initialAllocatedBytes=0);
        void Observe(std::uint64_t round, float rank);
        dtl::PlanEntry Snapshot(std::uint64_t round, const std::array<float,4>& thresholds, bool pin);
        dtl::RequestReceipt Accept(const dtl::PlanRequest&, dtl::StreamingLedger&, std::uint64_t round,
                                  const std::array<float,4>& thresholds);
        bool Fresh(std::uint64_t round,const std::array<float,4>& thresholds) const;
        bool Publishable(std::uint64_t round,const std::array<float,4>& thresholds,PublicationGuards guards) const;
        bool Complete(std::uint64_t nextRevision,dtl::StreamingLedger&,std::uint64_t actualAllocatedBytes=0);
        bool FullSourcePublished(std::uint64_t nextRevision,dtl::StreamingLedger&,std::uint64_t actualAllocatedBytes=0);
        bool Cancel(dtl::StreamingLedger&);
        dtl::TextureIdentity Identity() const { return identity_; }
        std::uint64_t Revision() const { return revision_; }
        std::uint8_t Current() const { return current_; }
        std::uint8_t Target() const { return target_; }
        bool Pending() const { return ticket_!=0; }
        std::uint64_t AllocatedBytes() const { return allocatedBytes_; }
        const PrefixLevels& Levels() const { return levels_; }
    private:
        std::uint8_t Wanted(const std::array<float,4>& thresholds) const;
        dtl::TextureIdentity identity_;
        std::uint64_t revision_;
        PrefixLevels levels_;
        // A native pooled block can be larger than its requested payload.
        // Requests predict rounded payload sizes; every published current
        // level reports its actual live native allocation capacity. A request
        // receipt is a reservation, never completed or physical-free bytes.
        std::uint64_t allocatedBytes_;
        dtl::Observation observation_;
        dtl::StreamingLedger::Ticket ticket_=0;
        std::uint8_t current_=0,target_=0,snapshotWanted_=0,acceptedWanted_=0;
        bool snapshotSeen_=false,acceptedSeen_=false,pinned_=true;
    };
}
