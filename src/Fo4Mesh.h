#pragma once
#include "GpuGather.h"
#include "core/StarfieldPlanner.h"
#include <vector>

namespace Fo4Mesh {
    struct Stats {
        bool installed{},healthy{};
        std::uint64_t type5Draws{},owningRejected{},cacheBypasses{},enrolled{},verified{},sourceRejected{};
        std::uint64_t accepted{},completed{},cancelled{},compactions{},restores{},compactedDraws{},fallbackDraws{};
        std::uint64_t queryFailures{},nativePairReleases{},sourceBytes{},stagingBytes{},retainedBytes{},pendingBufferBytes{},pending{};
        std::uint64_t reducedAllocationBytes{},completedRetirementBytes{};
        std::uint64_t candidateSubmitted{},candidateTimeouts{},creationThreadRejected{};
        std::uint64_t diagnosticDraws{},diagnosticPooled{},diagnosticSharedShapes{},diagnosticOwningReady{},diagnosticEffectData{},diagnosticExclusiveReady{};
        std::uint64_t nativeFramePumps{},initialPoolSources{},logicalPoolBytesReleased{};
        std::uint64_t trackedAddRefs{};
        std::uint64_t allocationRejected{};
        std::uint64_t preSetupCalls{},preSetupFallbacks{},unsupportedConsumers{},fallbackFailures{};
        bool passObservationInstalled{};
        std::uint64_t diagnosticPasses{},diagnosticUnknownPasses{};
        std::uint64_t diagnosticCacheBypasses{};
        bool pilot{};
        std::uint64_t pilotInactiveCapacity{},pilotFullFallbackCapacity{},pilotActiveBindingReduction{};
        std::uint64_t pilotFullRestores{},pilotReducedReuses{};
        std::uint64_t pilotDrawReceipts{};
        std::uint64_t pilotDetachFullCapacity{},pilotDetachSourceReferences{},pilotDetachTickets{};
        std::uint64_t pilotLiveRecords{};
        std::uint64_t lifetimeUnobservedCandidateDiscards{};
        std::array<std::uint64_t,4> diagnosticNativePasses{};
    };
    // Exact OG1.10.163 native ABI, Experimental.MeshStreaming opt-in. Install
    // before resources are created. Unsupported runtimes fail closed. A false
    // return can enable read-only coverage diagnostics; installed stays false.
    bool Install();
    void OnPresent(std::uint64_t frame);
    // Synchronous borrowed draw: restores the immutable full-source AABB in
    // worldBounds before texture GPU feedback consumes a compacted mesh.
    void Observe(GpuGather::Draw& draw,std::uint64_t gatherRound);
    std::vector<dtl::PlanEntry> Gather(std::uint64_t gatherRound);
    dtl::RequestReceipt Accept(void*,const dtl::PlanRequest& request);
    Stats Statistics();
    dtl::StreamingAccounting LedgerSnapshot(); // mesh-only; never texture emergency credit
    void LogSummary();
}
