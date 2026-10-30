// Source-only handoff: compile with Fo4MeshLifetime.cpp in a Windows CPU target.
// No device, WARP, driver, game or tracing session is created by these tests.
#include "Fo4MeshLifetime.h"
#include "Fo4MeshLifetimeDiagnostics.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <thread>

using namespace Fo4Mesh::Lifetime;
namespace {
    int checks{},failures{};
    void Check(bool value,const char* message) {
        ++checks;if (!value) {++failures;std::fprintf(stderr,"FAIL: %s\n",message);}
    }
    struct FakeBuffer final : ID3D11Buffer {
        std::atomic<ULONG> references{1};
        IUnknown* data{};
        UINT width{4096},setCalls{},eviction{};
        bool failAttach{},foreignData{},failQuery{};
        explicit FakeBuffer(UINT bytes=4096):width(bytes) {}
        ~FakeBuffer() {Clear();}
        void Clear() noexcept {auto* old=data;data=nullptr;if (old) old->Release();}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
            if (!output) return E_POINTER;
            *output=nullptr;
            if (iid!=__uuidof(IUnknown) && iid!=__uuidof(ID3D11DeviceChild) &&
                iid!=__uuidof(ID3D11Resource) && iid!=__uuidof(ID3D11Buffer)) return E_NOINTERFACE;
            *output=static_cast<ID3D11Buffer*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {return ++references;}
        ULONG STDMETHODCALLTYPE Release() override {return --references;}
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override {if(device)*device=nullptr;}
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,UINT* bytes,void* output) override {
            if (guid!=PrivateGuid) return DXGI_ERROR_NOT_FOUND;
            if (failQuery) return E_FAIL;
            if (!data && !foreignData) return DXGI_ERROR_NOT_FOUND;
            if (!bytes) return E_INVALIDARG;
            if (!output || *bytes<sizeof(IUnknown*)) {*bytes=sizeof(IUnknown*);return DXGI_ERROR_MORE_DATA;}
            if (foreignData) return E_FAIL; // not an interface; observer never reads it
            *static_cast<IUnknown**>(output)=data;data->AddRef();*bytes=sizeof(IUnknown*);return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,UINT bytes,const void*) override {
            if (guid!=PrivateGuid) return E_INVALIDARG;
            Clear();foreignData=bytes!=0;return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,const IUnknown* value) override {
            ++setCalls;
            if (guid!=PrivateGuid) return E_INVALIDARG;
            if (failAttach) return E_OUTOFMEMORY;
            auto* next=const_cast<IUnknown*>(value);
            if (next) next->AddRef();
            Clear();foreignData=false;data=next;return S_OK;
        }
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* type) override {if(type)*type=D3D11_RESOURCE_DIMENSION_BUFFER;}
        void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override {eviction=priority;}
        UINT STDMETHODCALLTYPE GetEvictionPriority() override {return eviction;}
        void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* desc) override {
            if(desc) {*desc={};desc->ByteWidth=width;desc->Usage=D3D11_USAGE_DEFAULT;desc->BindFlags=D3D11_BIND_VERTEX_BUFFER;}
        }
    };
    std::unique_ptr<Snapshot> SnapshotNow() {
        auto snapshot=std::make_unique<Snapshot>();Check(Drain(*snapshot),"drain succeeds");return snapshot;
    }
    unsigned Count(const Snapshot& snapshot,EventKind kind) {
        unsigned count{};for(std::size_t i=0;i<snapshot.count;++i) if(snapshot.events[i].kind==kind) ++count;return count;
    }
}

int main() {
    Check(!Diagnostics::IsExplicitOptIn(L"") && !Diagnostics::IsExplicitOptIn(L"0") &&
          !Diagnostics::IsExplicitOptIn(L"true") && !Diagnostics::IsExplicitOptIn(L"01") &&
          !Diagnostics::IsExplicitOptIn(L"1 "),"diagnostic requires exact explicit opt-in");
    Check(Diagnostics::IsExplicitOptIn(L"1"),"exact opt-in accepted");
    FakeBuffer first(128u<<20);
    Check(!Enabled(),"disabled fast path before observer state exists");
    Check(Attach(&first,Role::NativeHeap).result==Result::Disabled,"disabled by default");
    Check(!Enable(0) && !Enable(MaximumResources+1),"invalid capacities rejected");
    Check(Enable(4),"enable bounded four-slot pool");
    Check(Enabled(),"enabled fast path reflects successful module pin and state");
    Check(!Enable(5),"cannot silently enlarge configured pool");
    const auto attached=Attach(&first,Role::NativeHeap,41);
    Check(attached.result==Result::Attached && attached.generation!=0,"first attachment has generation");
    Check(first.references==1,"observer keeps no resource reference");
    const auto repeated=Attach(&first,Role::Candidate,41);
    Check(repeated.result==Result::AlreadyObservedUnverified && repeated.generation==attached.generation,"repeated address dedup explicitly leaves membership unverified");
    Check(first.setCalls==1,"dedup does not overwrite private data");
    Check(Attach(&first,Role::Candidate,42).result==Result::IdentityConflict,"owner epoch mismatch rejected");
    auto snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::Attachment)==1,"one attachment event");
    Check(snapshot->statistics.addedByteWidth==(128u<<20),"shared heap ByteWidth counted once");
    Check(snapshot->statistics.roleAttempts[0]==1 && snapshot->statistics.roleAttempts[2]==2 &&
          snapshot->statistics.roleFailures[2]==1,"role attribution includes rejected candidate observation");
    Check(snapshot->statistics.unrecordedAttachmentFailures==1,"identity conflict without event still reports lost detail");
    Check(snapshot->statistics.modulePinned && snapshot->statistics.identityUnverifiedObservations==1 &&
          snapshot->events[0].identityMembershipUnverified,"module pin and identity uncertainty are explicit");
    Check(snapshot->events[0].descriptor.ByteWidth==(128u<<20) &&
          snapshot->events[0].descriptor.BindFlags==D3D11_BIND_VERTEX_BUFFER,"native descriptor accompanies nominal byte evidence");
    Check(snapshot->events[0].roles==(static_cast<unsigned>(Role::NativeHeap)|static_cast<unsigned>(Role::Candidate)),"all observed roles retained");
    Check(snapshot->concurrentCounterSnapshot && snapshot->drainEndQpc>=snapshot->drainBeginQpc,"counter snapshot explicitly concurrent and time bounded");
    Check(Attach(&first,Role::DedicatedMesh,41).result==Result::AlreadyObservedUnverified,"later role merges with explicit uncertainty");

    // Clearing private data leaves the resource alive. Notification must not
    // be upgraded to destruction evidence, even when its byte count is known.
    first.Clear();
    snapshot=SnapshotNow();
    Check(first.references==1,"resource still alive after explicit private-data clear");
    Check(Count(*snapshot,EventKind::ObservationUpdated)==1,"later role union is emitted");
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==1,"clear reports lifetime notification");
    Check(snapshot->count==2 && snapshot->events[0].kind==EventKind::ObservationUpdated &&
          snapshot->events[1].kind==EventKind::PrivateDataLifetimeNotification &&
          snapshot->events[0].sequence<snapshot->events[1].sequence,"role addition keeps original order before notification");
    Check(!snapshot->events[1].physicalDestructionProven && !snapshot->events[1].privateDataOverwriteRuledOut,"clear never claims destruction");
    Check(snapshot->statistics.unnotifiedByteWidth==0,"notification removes unnotified accounting only");
    const auto reattached=Attach(&first,Role::NativeHeap,41);
    Check(reattached.result==Result::Attached && reattached.generation>attached.generation,"same address gets fresh generation after notification drain");
    first.Clear();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::Attachment)==1 && Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==1,"fast attach and notification both preserved");

    // GetPrivateData hands an interface reference to its caller. Keeping that
    // reference delays notification even after the resource drops its copy;
    // consequently notification time cannot be labelled destruction time.
    Check(Attach(&first,Role::NativeHeap,41).result==Result::Attached,"external token retention example attached");
    IUnknown* retained{};UINT tokenSize=sizeof(retained);
    Check(SUCCEEDED(first.GetPrivateData(PrivateGuid,&tokenSize,&retained)) && retained,"private interface can have an external owner");
    first.Clear();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==0,"external token reference delays notification");
    Check(Attach(&first,Role::Candidate,41).result==Result::IdentityConflict,"absent private key with retained old token is not silently deduplicated");
    if (retained) retained->Release();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==1 && !snapshot->events[0].physicalDestructionProven,"late token notification remains ambiguous");

    Check(Attach(&first,Role::NativeHeap,41).result==Result::Attached,"foreign replacement scenario attached");
    tokenSize=sizeof(retained);retained=nullptr;
    Check(SUCCEEDED(first.GetPrivateData(PrivateGuid,&tokenSize,&retained)) && retained,"old token externally retained before foreign replacement");
    first.SetPrivateData(PrivateGuid,sizeof(IUnknown*),nullptr);
    const auto foreignRepeated=Attach(&first,Role::NativeHeap,41);
    Check(foreignRepeated.result==Result::AlreadyObservedUnverified && foreignRepeated.hresult==S_FALSE,"same-sized foreign replacement never passes as verified attachment");
    snapshot=SnapshotNow();
    Check(snapshot->count==1 && snapshot->events[0].identityMembershipUnverified,"foreign replacement records persistent coverage uncertainty");
    if (retained) retained->Release();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==1 && snapshot->events[0].identityMembershipUnverified,"uncertainty survives delayed final release");
    first.foreignData=false;

    alignas(FakeBuffer) std::array<std::byte,sizeof(FakeBuffer)> reusedStorage{};
    auto* oldAddress=new(reusedStorage.data()) FakeBuffer(2048);
    Check(Attach(oldAddress,Role::DedicatedMesh).result==Result::Attached,"address reuse scenario attached");
    tokenSize=sizeof(retained);retained=nullptr;
    Check(SUCCEEDED(oldAddress->GetPrivateData(PrivateGuid,&tokenSize,&retained)) && retained,"address reuse keeps old token externally owned");
    oldAddress->~FakeBuffer();
    auto* newAddress=new(reusedStorage.data()) FakeBuffer(2048);newAddress->foreignData=true;
    Check(Attach(newAddress,Role::Candidate).result==Result::AlreadyObservedUnverified,"same-sized foreign data at reused resource address is unverified");
    snapshot=SnapshotNow();
    Check(snapshot->count==1 && snapshot->events[0].identityMembershipUnverified,"reused address uncertainty preserved");
    if (retained) retained->Release();newAddress->~FakeBuffer();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==1 && !snapshot->events[0].physicalDestructionProven,"old token notification does not identify replacement destruction");

    FakeBuffer occupied;occupied.foreignData=true;
    Check(Attach(&occupied,Role::OtherAuxiliary).result==Result::PrivateDataOccupied,"foreign data rejected");
    Check(occupied.setCalls==0,"foreign private data never replaced");
    FakeBuffer queryFailed;queryFailed.failQuery=true;
    Check(Attach(&queryFailed,Role::OtherAuxiliary).result==Result::PrivateDataQueryFailed,"query failure is unavailable evidence");
    FakeBuffer failed;failed.failAttach=true;
    Check(Attach(&failed,Role::SourceStaging).result==Result::AttachFailed,"failed SetPrivateDataInterface explicit");
    snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::AttachmentFailure)==1,"attachment failure event preserved");
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==0,"failed attachment creates no lifetime credit");

    FakeBuffer a(1024),b(2048),c(4096),d(8192),overflow(16384);
    const auto prior=snapshot->statistics.addedByteWidth;
    const auto priorUnrecorded=snapshot->statistics.unrecordedAttachmentFailures;
    const auto priorCandidateFailures=snapshot->statistics.roleFailures[2];
    Check(Attach(&a,Role::DedicatedMesh).result==Result::Attached,"dedicated resource tracked");
    Check(Attach(&b,Role::DrawReserve).result==Result::Attached,"reserve cost tracked");
    Check(Attach(&c,Role::SourceStaging).result==Result::Attached,"staging cost tracked");
    Check(Attach(&d,Role::OtherAuxiliary).result==Result::Attached,"other auxiliary cost tracked");
    Check(Attach(&overflow,Role::Candidate).result==Result::Capacity,"bounded pool refuses excess attachment");
    Check(overflow.setCalls==0,"capacity failure leaves resource untouched");
    snapshot=SnapshotNow();
    Check(snapshot->statistics.addedByteWidth-prior==15360,"all auxiliary ByteWidths included exactly once");
    Check(snapshot->statistics.unrecordedAttachmentFailures==priorUnrecorded+1 &&
          snapshot->statistics.roleFailures[2]==priorCandidateFailures+1,"capacity loss is attributed and cannot look like full coverage");
    Disable();Check(Attach(&overflow,Role::Candidate).result==Result::Disabled,"disable stops new instrumentation");
    Check(!Enabled(),"disabled fast path after disable");
    std::thread t1([&]{a.Clear();b.Clear();});
    std::thread t2([&]{c.Clear();d.Clear();});
    t1.join();t2.join();snapshot=SnapshotNow();
    Check(Count(*snapshot,EventKind::PrivateDataLifetimeNotification)==4,"concurrent final releases preserved while disabled");
    Check(snapshot->statistics.unnotifiedResources==0 && snapshot->statistics.notificationProtocolErrors==0,"drained resource counters balanced");

    Check(Enable(4),"re-enable same capacity");
    FakeBuffer late;
    Check(Attach(&late,Role::OtherAuxiliary).result==Result::Attached,"teardown example attached");
    BeginProcessTeardown();late.Clear();snapshot=SnapshotNow();
    Check(!Enabled(),"teardown cannot leave enabled fast path true");
    bool teardownEvent=false;
    for(std::size_t i=0;i<snapshot->count;++i) if(snapshot->events[i].kind==EventKind::PrivateDataLifetimeNotification)
        teardownEvent=snapshot->events[i].duringProcessTeardown && !snapshot->events[i].physicalDestructionProven;
    Check(teardownEvent,"teardown notification explicitly ambiguous");
    Check(Attach(&overflow,Role::Candidate).result==Result::Teardown && !Enable(4),"teardown prevents restarting observer");
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
