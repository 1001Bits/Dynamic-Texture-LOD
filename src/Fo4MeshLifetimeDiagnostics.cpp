#include "Fo4MeshLifetimeDiagnostics.h"
#include "Fo4MeshLifetime.h"
#include "Fo4MeshHeapHooks.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <new>
#include <span>

namespace Fo4Mesh::Lifetime::Diagnostics {
namespace {
    std::atomic<Snapshot*> snapshot{};
    std::mutex initialization;
    std::atomic_flag polling=ATOMIC_FLAG_INIT;
    std::uint64_t nextDrainMs{}; // sole polling-flag owner
    std::atomic<std::uint64_t> busyPolls{},lostSnapshots{},lostEvents{},loggedEvents{};
    constexpr std::array<const char*,RoleCount> roleNames{
        "nativeHeap","dedicatedMesh","candidate","drawReserve","sourceStaging","otherAuxiliary"};
    const char* Kind(EventKind kind) noexcept {
        switch(kind) {
        case EventKind::Attachment:return "attachment";
        case EventKind::ObservationUpdated:return "observationUpdated";
        case EventKind::AttachmentFailure:return "attachmentFailure";
        case EventKind::PrivateDataLifetimeNotification:return "privateDataLifetimeNotification";
        }
        return "unknown";
    }
}

bool InitializeFromEnvironment() noexcept {
    wchar_t value[4]{};
    const auto length=GetEnvironmentVariableW(L"DTL_FO4_MESH_LIFETIME",value,static_cast<DWORD>(std::size(value)));
    if (length!=1 || !IsExplicitOptIn(std::wstring_view(value,length))) return false;
    try {
        std::scoped_lock lock(initialization);
        if (snapshot.load(std::memory_order_acquire)) return Enabled();
        // Like the observer token state, this bounded drain buffer deliberately
        // survives C++ static teardown. It contains no resource references.
        auto* output=new(std::nothrow) Snapshot;
        if (!output) return false;
        if (!Enable()) {delete output;return false;}
        snapshot.store(output,std::memory_order_release);
        logger::info("[fo4-mesh-lifetime] diagnostic=true enabled=true capacity={} modulePinned=true "
            "environment=DTL_FO4_MESH_LIFETIME observationScope=meshBuffersAndRegisteredNativeHeaps "
            "completeNativeBirthCoverage=false resourceByteWidthIsResidency=false "
            "privateDataNotificationsArePhysicalDestruction=false auxiliaryCostsIncluded=true "
            "heapCoverage=backingPointerTransitions candidateCoverage=firstReadyPair "
            "unquantifiedCosts=queries_views_driver_CPU netVramSavingClaim=false performanceEligible=false",MaximumResources);
        return true;
    } catch (...) {
        // A logger/allocation failure must not change native allocation flow.
        Disable();return false;
    }
}

void PollLog() noexcept {
    auto* output=snapshot.load(std::memory_order_acquire);
    if (!output) return;
    if (polling.test_and_set(std::memory_order_acquire)) {++busyPolls;return;}
    struct Clear {~Clear() {polling.clear(std::memory_order_release);}} clear;
    const auto now=GetTickCount64();
    if (now<nextDrainMs) return;
    nextDrainMs=now+1000;
    if (!Drain(*output)) return;
    std::size_t emitted{};
    try {
        for (const auto& event:std::span(output->events.data(),output->count)) {
            logger::info("[fo4-mesh-lifetime-event] kind={} sequence={} generation={} resourceIdentity={:X} "
                "ownerEpoch={} qpc={} byteWidth={} rolesMask={} usage={} bindFlags={} cpuAccess={} miscFlags={} stride={} "
                "identityMembershipUnverified={} hr={} processTeardownKnown={} "
                "privateDataOverwriteRuledOut=false physicalDestructionProven=false",
                Kind(event.kind),event.sequence,event.generation,event.resourceIdentity,event.ownerEpoch,event.qpc,
                event.byteWidth,event.roles,static_cast<std::uint32_t>(event.descriptor.Usage),event.descriptor.BindFlags,
                event.descriptor.CPUAccessFlags,event.descriptor.MiscFlags,event.descriptor.StructureByteStride,
                event.identityMembershipUnverified,static_cast<std::uint32_t>(event.hresult),event.duringProcessTeardown);
            ++emitted;++loggedEvents;
        }
        const auto& stats=output->statistics;
        const auto heaps=HeapHooks::GetStatistics(); // atomic counters, no native dereferences or allocator lock
        logger::info("[fo4-mesh-lifetime] enabled={} modulePinned={} teardownKnown={} attempts={} attachments={} "
            "deduplicated={} attachmentFailures={} capacityFailures={} unrecordedAttachmentFailures={} "
            "identityConflicts={} identityMembershipUnverified={} internalFailures={} notificationProtocolErrors={} "
            "addedByteWidth={} lifetimeNotifications={} notifiedByteWidth={} unnotifiedResources={} unnotifiedByteWidth={} "
            "teardownNotifications={} teardownNotifiedByteWidth={} loggedEvents={} lostSnapshots={} lostEvents={} "
            "busyPolls={} heapHooksInstalled={} heapObservations={} heapInterfaceFailures={} "
            "qpcFrequency={} drainBeginQpc={} drainEndQpc={} sequenceBegin={} sequenceEnd={} "
            "concurrentCounterSnapshot=true completeNativeBirthCoverage=false unnotifiedBytesAreLiveOrResident=false "
            "physicalDestructionProven=false netVramSavingClaim=false sinkPersistenceVerified=false",
            stats.enabled,stats.modulePinned,stats.processTeardown,stats.attempts,stats.attachments,stats.deduplicated,
            stats.attachmentFailures,stats.capacityFailures,stats.unrecordedAttachmentFailures,stats.identityConflicts,
            stats.identityUnverifiedObservations,stats.internalFailures,stats.notificationProtocolErrors,
            stats.addedByteWidth,stats.lifetimeNotifications,stats.notifiedByteWidth,stats.unnotifiedResources,
            stats.unnotifiedByteWidth,stats.teardownNotifications,stats.teardownNotifiedByteWidth,
            loggedEvents.load(),lostSnapshots.load(),lostEvents.load(),busyPolls.load(),heaps.installed,
            heaps.lifetimeHeapObservations,heaps.lifetimeHeapInterfaceFailures,output->qpcFrequency,
            output->drainBeginQpc,output->drainEndQpc,output->sequenceBegin,output->sequenceEnd);
        for (std::size_t i=0;i<RoleCount;++i)
            logger::info("[fo4-mesh-lifetime-role] role={} mask={} attempts={} attachmentFailures={} interfaceFailures={} "
                         "rolesMayOverlap=true sumRoleBytesForNet=false",roleNames[i],1u<<i,
                         stats.roleAttempts[i],stats.roleFailures[i],i==0 ? heaps.lifetimeHeapInterfaceFailures : 0);
    } catch (...) {
        // Drain consumes events. A failed sink must not silently look complete.
        // Counters remain for a later successful log; raw native lifetimes and
        // original control flow are unaffected by the diagnostic sink failure.
        ++lostSnapshots;lostEvents.fetch_add(output->count-emitted,std::memory_order_relaxed);
    }
}
}
