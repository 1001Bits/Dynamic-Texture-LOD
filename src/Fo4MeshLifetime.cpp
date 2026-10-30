#include "Fo4MeshLifetime.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

namespace Fo4Mesh::Lifetime {
namespace {
    enum class Phase : std::uint8_t { Empty, Preparing, Attached, Notified, Failed, Drained };
    struct State;
    struct Token final : IUnknown {
        std::atomic<ULONG> references{};
        State* state{};
        std::uint32_t index{};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) noexcept override {
            if (!output) return E_POINTER;
            *output=nullptr;
            if (iid!=__uuidof(IUnknown)) return E_NOINTERFACE;
            *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return references.fetch_add(1,std::memory_order_relaxed)+1;
        }
        ULONG STDMETHODCALLTYPE Release() noexcept override;
    };
    struct Record {
        Token token;
        std::atomic<Phase> phase{Phase::Empty};
        std::uint64_t generation{},identity{},ownerEpoch{},attachmentQpc{},attachmentSequence{};
        std::uint64_t rolesQpc{},rolesSequence{};
        std::uint32_t byteWidth{},roles{},reportedRoles{};
        D3D11_BUFFER_DESC descriptor{};
        HRESULT hresult{S_OK};
        bool attachmentReported{},identityUnverified{},reportedIdentityUnverified{};
        std::atomic<std::uint64_t> notificationQpc{},notificationSequence{};
        std::atomic<bool> notificationDuringTeardown{};
    };
    struct State {
        std::mutex writer;
        std::atomic<bool> enabled{},teardown{};
        std::uint32_t capacity{};
        HMODULE pinnedModule{};
        std::uint64_t generation{},qpcFrequency{};
        std::atomic<std::uint64_t> sequence{},attempts{},attachments{},deduplicated{},failures{},
            capacityFailures{},identityConflicts{},internalFailures{},addedBytes{},notifications{},
            notifiedBytes{},teardownNotifications{},teardownBytes{},unnotifiedCount{},unnotifiedBytes{},protocolErrors{},identityUnverified{},unrecordedFailures{};
        std::array<std::atomic<std::uint64_t>,RoleCount> roleAttempts{},roleFailures{};
        std::array<Record,MaximumResources> records;
    };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<ULONG>::is_always_lock_free);
    static_assert(std::atomic<Phase>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(sizeof(void*)==sizeof(std::uint64_t));
    // Intentionally process-lifetime storage: late device-child callbacks must
    // not touch a C++ static whose destructor has already run. Bounded once.
    std::atomic<State*> state{};
    std::mutex initialization;
    std::uint64_t Qpc() noexcept {
        LARGE_INTEGER value{};return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
    }
    ULONG Token::Release() noexcept {
        const auto prior=references.fetch_sub(1,std::memory_order_acq_rel);
        if (prior!=1) {
            if (!prior) {
                // A broken COM caller must not silently wrap the count and
                // make a reusable slot look permanently referenced.
                references.fetch_add(1,std::memory_order_relaxed);
                if (state) state->protocolErrors.fetch_add(1,std::memory_order_relaxed);
            }
            return prior ? prior-1 : 0;
        }
        auto& record=state->records[index];
        if (record.phase.load(std::memory_order_acquire)!=Phase::Attached) return 0;
        const bool teardown=state->teardown.load(std::memory_order_acquire);
        record.notificationQpc.store(Qpc(),std::memory_order_relaxed);
        record.notificationSequence.store(state->sequence.fetch_add(1,std::memory_order_relaxed)+1,std::memory_order_relaxed);
        record.notificationDuringTeardown.store(teardown,std::memory_order_relaxed);
        state->notifications.fetch_add(1,std::memory_order_relaxed);
        state->notifiedBytes.fetch_add(record.byteWidth,std::memory_order_relaxed);
        state->unnotifiedCount.fetch_sub(1,std::memory_order_relaxed);
        state->unnotifiedBytes.fetch_sub(record.byteWidth,std::memory_order_relaxed);
        if (teardown) {
            state->teardownNotifications.fetch_add(1,std::memory_order_relaxed);
            state->teardownBytes.fetch_add(record.byteWidth,std::memory_order_relaxed);
        }
        // Published last: Drain/reuse cannot race unfinished callback writes.
        record.phase.store(Phase::Notified,std::memory_order_release);
        return 0;
    }
    void CountRoles(std::array<std::atomic<std::uint64_t>,RoleCount>& counters,std::uint32_t roles) noexcept {
        for (std::size_t i=0;i<RoleCount;++i) if (roles&(1u<<i)) ++counters[i];
    }
    Attachment Failure(State& s,std::uint32_t roles,Result result,HRESULT hr,std::uint64_t identity=0,std::uint32_t bytes=0) noexcept {
        ++s.failures;
        ++s.unrecordedFailures;CountRoles(s.roleFailures,roles);
        if (result==Result::Capacity) ++s.capacityFailures;
        if (result==Result::IdentityConflict) ++s.identityConflicts;
        if (result==Result::InternalFailure) ++s.internalFailures;
        return {result,hr,0,identity,bytes};
    }
    Event MakeEvent(const Record& r,EventKind kind) noexcept {
        Event event;
        event.kind=kind;event.generation=r.generation;event.resourceIdentity=r.identity;
        event.ownerEpoch=r.ownerEpoch;event.byteWidth=r.byteWidth;event.roles=r.roles;event.hresult=r.hresult;
        event.descriptor=r.descriptor;
        event.identityMembershipUnverified=r.identityUnverified;
        event.sequence=r.attachmentSequence;event.qpc=r.attachmentQpc;
        if (kind==EventKind::PrivateDataLifetimeNotification) {
            event.sequence=r.notificationSequence.load(std::memory_order_relaxed);
            event.qpc=r.notificationQpc.load(std::memory_order_relaxed);
            event.duringProcessTeardown=r.notificationDuringTeardown.load(std::memory_order_relaxed);
        } else if (kind==EventKind::ObservationUpdated) {
            event.sequence=r.rolesSequence;event.qpc=r.rolesQpc;
        }
        return event;
    }
}

bool Enable(std::uint32_t capacity) noexcept {
    if (!capacity || capacity>MaximumResources) return false;
    try {
        std::scoped_lock lock(initialization);
        auto* s=state.load(std::memory_order_acquire);
        if (!s) {
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(&Enable),&module)) return false;
            s=new(std::nothrow) State;
            if (!s) return false;
            s->capacity=capacity;s->pinnedModule=module;
            LARGE_INTEGER frequency{};
            s->qpcFrequency=QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : 0;
            for (std::uint32_t i=0;i<capacity;++i) {s->records[i].token.state=s;s->records[i].token.index=i;}
            state.store(s,std::memory_order_release);
        }
        if (s->capacity!=capacity || s->teardown.load(std::memory_order_acquire)) return false;
        s->enabled.store(true,std::memory_order_release);return true;
    } catch (...) {return false;}
}
void Disable() noexcept {
    if (auto* s=state.load(std::memory_order_acquire)) s->enabled.store(false,std::memory_order_release);
}
bool Enabled() noexcept {
    const auto* s=state.load(std::memory_order_acquire);
    return s && s->enabled.load(std::memory_order_acquire) && !s->teardown.load(std::memory_order_acquire);
}
void BeginProcessTeardown() noexcept {
    if (auto* s=state.load(std::memory_order_acquire)) {
        s->teardown.store(true,std::memory_order_release);s->enabled.store(false,std::memory_order_release);
    }
}

Attachment Attach(ID3D11Buffer* buffer,Role role,std::uint64_t ownerEpoch) noexcept {
    auto* s=state.load(std::memory_order_acquire);
    if (!s) return {};
    if (s->teardown.load(std::memory_order_acquire)) return {Result::Teardown,S_FALSE};
    if (!s->enabled.load(std::memory_order_acquire)) return {};
    ++s->attempts;
    constexpr auto allRoles=(1u<<6)-1;
    const auto roles=static_cast<std::uint32_t>(role);
    CountRoles(s->roleAttempts,roles);
    if (!buffer || !roles || (roles&~allRoles)) return Failure(*s,roles,Result::InvalidResource,E_INVALIDARG);
    IUnknown* canonical{};
    const auto identityResult=buffer->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&canonical));
    if (FAILED(identityResult) || !canonical) return Failure(*s,roles,Result::IdentityFailure,FAILED(identityResult) ? identityResult : E_NOINTERFACE);
    struct IdentityOwner {IUnknown* value;~IdentityOwner() {value->Release();}} identityOwner{canonical};
    const auto identity=reinterpret_cast<std::uint64_t>(canonical);
    D3D11_BUFFER_DESC description{};buffer->GetDesc(&description);
    if (!description.ByteWidth) return Failure(*s,roles,Result::InvalidResource,E_INVALIDARG,identity);
    try {
        std::scoped_lock lock(s->writer);
        if (s->teardown.load(std::memory_order_acquire)) return {Result::Teardown,S_FALSE,0,identity,description.ByteWidth};
        if (!s->enabled.load(std::memory_order_acquire)) return {Result::Disabled,S_FALSE,0,identity,description.ByteWidth};
        // Check even on the dedup path: a removed token retained by another
        // caller must not masquerade as a current attachment (or a new object
        // reusing the same address). Size-only access never acquires or treats
        // untrusted stored bytes as an IUnknown.
        UINT occupiedBytes{};
        const auto query=buffer->GetPrivateData(PrivateGuid,&occupiedBytes,nullptr);
        const bool occupied=SUCCEEDED(query) || query==DXGI_ERROR_MORE_DATA;
        if (query!=DXGI_ERROR_NOT_FOUND && !occupied)
            return Failure(*s,roles,Result::PrivateDataQueryFailed,query,identity,description.ByteWidth);
        for (std::uint32_t i=0;i<s->capacity;++i) {
            auto& r=s->records[i];
            if (r.phase.load(std::memory_order_acquire)!=Phase::Attached || r.identity!=identity) continue;
            if (!occupied || occupiedBytes!=sizeof(IUnknown*) || !r.token.references.load(std::memory_order_acquire) ||
                r.byteWidth!=description.ByteWidth || (r.ownerEpoch && ownerEpoch && r.ownerEpoch!=ownerEpoch))
                return Failure(*s,roles,Result::IdentityConflict,E_INVALIDARG,identity,description.ByteWidth);
            if ((r.roles|roles)!=r.roles || !r.identityUnverified) {
                r.roles|=roles;r.rolesQpc=Qpc();
                r.rolesSequence=s->sequence.fetch_add(1,std::memory_order_relaxed)+1;
            }
            r.identityUnverified=true;++s->deduplicated;++s->identityUnverified;
            // Do not fetch/call an unknown private-data pointer. The occupied
            // key could now contain same-sized foreign bytes or an interface;
            // retained references to our old token do not prove membership.
            return {Result::AlreadyObservedUnverified,S_FALSE,r.generation,identity,r.byteWidth};
        }
        // The GUID must be absent: never replace another attachment or data.
        if (occupied) return Failure(*s,roles,Result::PrivateDataOccupied,query,identity,description.ByteWidth);
        Record* target{};
        for (std::uint32_t i=0;i<s->capacity;++i) {
            auto& r=s->records[i];const auto phase=r.phase.load(std::memory_order_acquire);
            if ((phase==Phase::Empty || phase==Phase::Drained) && !r.token.references.load(std::memory_order_acquire)) {target=&r;break;}
        }
        if (!target) return Failure(*s,roles,Result::Capacity,E_OUTOFMEMORY,identity,description.ByteWidth);
        auto& r=*target;
        r.phase.store(Phase::Preparing,std::memory_order_release);
        r.generation=++s->generation;r.identity=identity;r.ownerEpoch=ownerEpoch;
        r.byteWidth=description.ByteWidth;r.descriptor=description;
        r.roles=roles;r.reportedRoles=0;r.attachmentReported=false;
        r.identityUnverified=false;r.reportedIdentityUnverified=false;
        r.attachmentQpc=Qpc();r.attachmentSequence=s->sequence.fetch_add(1,std::memory_order_relaxed)+1;
        r.rolesQpc=r.attachmentQpc;r.rolesSequence=r.attachmentSequence;
        r.notificationQpc=0;r.notificationSequence=0;r.notificationDuringTeardown=false;
        r.token.references.store(1,std::memory_order_release); // local reference, never a resource reference
        r.hresult=buffer->SetPrivateDataInterface(PrivateGuid,&r.token);
        if (FAILED(r.hresult)) {
            r.phase.store(Phase::Failed,std::memory_order_release);r.token.Release();
            ++s->failures;CountRoles(s->roleFailures,roles);
            return {Result::AttachFailed,r.hresult,r.generation,identity,r.byteWidth};
        }
        ++s->attachments;s->addedBytes.fetch_add(r.byteWidth,std::memory_order_relaxed);
        ++s->unnotifiedCount;s->unnotifiedBytes.fetch_add(r.byteWidth,std::memory_order_relaxed);
        r.phase.store(Phase::Attached,std::memory_order_release);
        r.token.Release(); // resource now owns the only observer interface reference
        return {Result::Attached,S_OK,r.generation,identity,r.byteWidth};
    } catch (...) {return Failure(*s,roles,Result::InternalFailure,E_FAIL,identity,description.ByteWidth);}
}

bool Drain(Snapshot& output) noexcept {
    auto* s=state.load(std::memory_order_acquire);
    if (!s) return false;
    try {
        std::unique_lock lock(s->writer,std::try_to_lock);
        if (!lock.owns_lock()) return false;
        output.count=0;output.qpcFrequency=s->qpcFrequency;
        output.drainBeginQpc=Qpc();output.sequenceBegin=s->sequence.load(std::memory_order_acquire);
        output.concurrentCounterSnapshot=true;
        for (std::uint32_t i=0;i<s->capacity;++i) {
            auto& r=s->records[i];const auto phase=r.phase.load(std::memory_order_acquire);
            if (phase!=Phase::Attached && phase!=Phase::Notified && phase!=Phase::Failed) continue;
            if (!r.attachmentReported) {
                output.events[output.count++]=MakeEvent(r,phase==Phase::Failed ? EventKind::AttachmentFailure : EventKind::Attachment);
                r.attachmentReported=true;r.reportedRoles=r.roles;r.reportedIdentityUnverified=r.identityUnverified;
            } else if (r.reportedRoles!=r.roles || r.reportedIdentityUnverified!=r.identityUnverified) {
                auto event=MakeEvent(r,EventKind::ObservationUpdated);
                output.events[output.count++]=event;r.reportedRoles=r.roles;r.reportedIdentityUnverified=r.identityUnverified;
            }
            if (phase==Phase::Notified) output.events[output.count++]=MakeEvent(r,EventKind::PrivateDataLifetimeNotification);
            if (phase==Phase::Notified || phase==Phase::Failed) r.phase.store(Phase::Drained,std::memory_order_release);
        }
        std::sort(output.events.begin(),output.events.begin()+output.count,[](const Event& a,const Event& b) noexcept {return a.sequence<b.sequence;});
        auto& result=output.statistics;
        result.enabled=s->enabled.load();result.processTeardown=s->teardown.load();result.capacity=s->capacity;
        result.modulePinned=s->pinnedModule!=nullptr;result.identityUnverifiedObservations=s->identityUnverified.load();
        result.attempts=s->attempts.load();result.attachments=s->attachments.load();result.deduplicated=s->deduplicated.load();
        result.attachmentFailures=s->failures.load();result.capacityFailures=s->capacityFailures.load();
        result.identityConflicts=s->identityConflicts.load();result.internalFailures=s->internalFailures.load();
        result.addedByteWidth=s->addedBytes.load();result.lifetimeNotifications=s->notifications.load();result.notifiedByteWidth=s->notifiedBytes.load();
        result.teardownNotifications=s->teardownNotifications.load();result.teardownNotifiedByteWidth=s->teardownBytes.load();
        result.unnotifiedResources=s->unnotifiedCount.load();result.unnotifiedByteWidth=s->unnotifiedBytes.load();
        result.notificationProtocolErrors=s->protocolErrors.load();
        result.unrecordedAttachmentFailures=s->unrecordedFailures.load();
        for (std::size_t i=0;i<RoleCount;++i) {
            result.roleAttempts[i]=s->roleAttempts[i].load();result.roleFailures[i]=s->roleFailures[i].load();
        }
        output.sequenceEnd=s->sequence.load(std::memory_order_acquire);output.drainEndQpc=Qpc();
        return true;
    } catch (...) {++s->internalFailures;return false;}
}
}
