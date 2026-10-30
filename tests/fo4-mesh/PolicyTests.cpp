#include "Fo4MeshPolicy.h"
#include <cstdlib>
#include <iostream>

namespace {
    unsigned checks=0;
    void Check(bool value,const char* message) {++checks;if(!value){std::cerr<<message<<'\n';std::exit(1);}}
    Fo4Mesh::PrefixLevels Levels() {
        Fo4Mesh::PrefixLevels levels;
        levels.count=3;
        levels.levels={Fo4Mesh::PrefixLevel{{0,90},30,512},Fo4Mesh::PrefixLevel{{0,60},20,256},Fo4Mesh::PrefixLevel{{0,30},10,128}};
        return levels;
    }
    const auto thresholds=dtl::kStarfieldMeshThresholds;
    dtl::RequestReceipt Sink(void* context,const dtl::PlanRequest& request) {
        auto** arguments=static_cast<void**>(context);
        return static_cast<Fo4Mesh::PolicyState*>(arguments[0])->Accept(request,*static_cast<dtl::StreamingLedger*>(arguments[1]),1,thresholds);
    }
}
int main() {
    Check(Fo4Mesh::KeepFullNativePass(true,true,true,false,0),"known Utility/Effect keeps an already-full fixture unpinned");
    Check(!Fo4Mesh::KeepFullNativePass(false,true,true,false,0),"full native pass exception never opens vanilla admission");
    Check(!Fo4Mesh::KeepFullNativePass(true,false,true,false,0),"unknown native class still pins a full fixture");
    Check(!Fo4Mesh::KeepFullNativePass(true,true,false,false,0),"changed identity or ownership never borrows full-pass exception");
    Check(!Fo4Mesh::KeepFullNativePass(true,true,true,true,0),"high-bit native mode remains excluded");
    Check(!Fo4Mesh::KeepFullNativePass(true,true,true,false,1),"reduced Utility/Effect requires the pre-Setup full-source scope");
    dtl::StreamingLedger ledger;
    Fo4Mesh::PolicyState state({10,20},30,Levels());
    state.Observe(1,0.01f);
    auto snapshot=state.Snapshot(1,thresholds,false);
    Check(snapshot.lowest==2 && snapshot.wanted==2 && snapshot.seen,"low rank snapshot");
    dtl::PlanRequest request{{10,20},30,0,2,384,dtl::RequestReason::MeshDegrade};
    auto wrong=request;wrong.reason=dtl::RequestReason::Degrade;
    Check(state.Accept(wrong,ledger,1,thresholds).status==dtl::RequestStatus::Rejected,"texture sink cannot cross mesh identity");
    state.Observe(1,0.5f);
    Check(state.Accept(request,ledger,1,thresholds).status==dtl::RequestStatus::Stale,"reappearance cancels stale worker snapshot");
    snapshot=state.Snapshot(3,thresholds,false);
    Check(!snapshot.seen && snapshot.priority==0,"whole-cycle observation aging");
    Check(state.Accept(request,ledger,3,thresholds).status==dtl::RequestStatus::AcceptedPending,"pending native request accepted");
    Check(state.Publishable(3,thresholds,{true,true,false,false}),"healthy matching ownership can publish accepted degradation");
    Check(!state.Publishable(3,thresholds,{true,true,true,false}),"a pin acquired after acceptance blocks degradation publication");
    Check(state.Current()==0 && state.Target()==2 && ledger.Snapshot().requestedDecreaseBytes==384,"acceptance never publishes a level");
    Check(!state.Complete(30,ledger),"same resource revision cannot acknowledge publication");
    state.Observe(3,0.5f);
    Check(!state.Fresh(3,thresholds),"changed demand blocks late publication");
    Check(state.Cancel(ledger) && ledger.Snapshot().pendingOperations==0,"cancellation clears request accounting");
    Fo4Mesh::PolicyState stable({11,21},31,Levels());
    stable.Observe(1,0.01f);
    // A known full native pass must leave distance-driven degradation eligible;
    // the prior adapter pinned here before the first Core request could occur.
    snapshot=stable.Snapshot(1,thresholds,!Fo4Mesh::KeepFullNativePass(true,true,true,false,stable.Current()));
    const dtl::Budget budget{dtl::Mode::Room,0,1ull<<30,512ull<<20,700ull<<20,1ull<<30};
    dtl::StarfieldPlanSession session;
    session.Begin({},budget,{}, {},{&snapshot,1});
    void* arguments[]{&stable,&ledger};
    while(!session.Done()) session.Step(nullptr,nullptr,100,Sink,arguments);
    Check(stable.Pending() && stable.Target()==2,"real Core mesh stage invokes independent sink");
    Check(stable.Complete(32,ledger) && stable.Current()==2 && ledger.Snapshot().pendingOperations==0,"native pair publication acknowledged");
    Check(ledger.Snapshot().releasedAwaitingDestructionBytes==0 && ledger.Snapshot().observedDestroyedBytes==0,"logical publication creates no physical-free claim");
    request.identity={11,21};request.resourceIdentity=31;request.current=2;request.target=0;request.reason=dtl::RequestReason::MeshUpgrade;
    stable.Observe(1,0.5f);stable.Snapshot(1,thresholds,false);
    Check(stable.Accept(request,ledger,1,thresholds).status==dtl::RequestStatus::Stale,"retired native pair revision rejected");
    request.resourceIdentity=32;
    Check(stable.Accept(request,ledger,1,thresholds).status==dtl::RequestStatus::AcceptedPending,"full source restoration request accepted");
    Check(stable.Cancel(ledger),"failed restoration preserves current resident level");
    Check(stable.Current()==2 && stable.Target()==2,"OOM cancellation retains resident prefix");
    stable.Snapshot(1,thresholds,true);
    Check(stable.Accept(request,ledger,1,thresholds).status==dtl::RequestStatus::AcceptedPending,"pinned record still accepts full-source upgrade");
    Check(stable.Publishable(1,thresholds,{true,true,true,false}),"render scheduler permits validated upgrade on a pinned record");
    Check(!stable.Publishable(1,thresholds,{false,true,true,false}),"health loss blocks publication of already accepted work");
    Check(!stable.Publishable(1,thresholds,{true,false,true,false}),"ownership change blocks pending publication");
    Check(!stable.Publishable(1,thresholds,{true,true,true,true}),"forced full fallback supersedes ordinary pending publication");
    Check(!stable.FullSourcePublished(32,ledger) && stable.Pending(),"manual restoration requires an actual new pair revision");
    Check(stable.FullSourcePublished(33,ledger) && stable.Current()==0 && !stable.Pending(),"manual full source publication cancels outstanding logical request");
    Check(ledger.Snapshot().pendingOperations==0 && ledger.Snapshot().releasedAwaitingDestructionBytes==0,"manual restoration creates no destruction credit");
    stable.Snapshot(3,thresholds,true);
    request.resourceIdentity=33;request.current=0;request.target=2;request.reason=dtl::RequestReason::MeshDegrade;
    Check(stable.Accept(request,ledger,3,thresholds).status==dtl::RequestStatus::Stale,"pinned full record rejects further degradation");
    Fo4Mesh::PolicyState pooled({12,22},1,Levels(),640);
    pooled.Observe(1,0.01f);snapshot=pooled.Snapshot(1,thresholds,false);
    Check(snapshot.bytes[0]==640 && snapshot.bytes[2]==128,"initial pool block capacity distinct from requested payload");
    request={{12,22},1,0,2,512,dtl::RequestReason::MeshDegrade};
    const auto receipt=pooled.Accept(request,ledger,1,thresholds);
    Check(receipt.status==dtl::RequestStatus::AcceptedPending && receipt.bytes==512 &&
        ledger.Snapshot().requestedDecreaseBytes==512,"logical allocation decrease charges actual old pool capacity");
    Check(pooled.Complete(2,ledger) && pooled.AllocatedBytes()==128,"dedicated candidate publishes exact rounded allocation");
    pooled.Observe(2,0.5f);snapshot=pooled.Snapshot(2,thresholds,false);
    Check(snapshot.bytes[0]==512 && snapshot.bytes[2]==128,"restoration predicts a new dedicated full payload, not retired pool slack");
    request={{12,22},2,2,0,384,dtl::RequestReason::MeshUpgrade};
    Check(pooled.Accept(request,ledger,2,thresholds).bytes==384 && pooled.Complete(3,ledger) &&
        pooled.AllocatedBytes()==512,"restored dedicated allocation accounting stays exact");
    Check(ledger.Snapshot().releasedAwaitingDestructionBytes==0 && ledger.Snapshot().observedDestroyedBytes==0,
        "logical pool capacity reduction never becomes physical destruction credit");
    Fo4Mesh::PolicyState pooledCandidate({13,23},1,Levels(),640);
    pooledCandidate.Observe(1,0.01f);pooledCandidate.Snapshot(1,thresholds,false);
    request={{13,23},1,0,2,512,dtl::RequestReason::MeshDegrade};
    Check(pooledCandidate.Accept(request,ledger,1,thresholds).bytes==512,"pooled request reserves requested payload delta");
    Check(!pooledCandidate.Complete(2,ledger,64) && pooledCandidate.Pending(),"invalid candidate capacity cannot acknowledge publication");
    Check(pooledCandidate.Complete(2,ledger,192),"native pool candidate can publish observed block capacity");
    snapshot=pooledCandidate.Snapshot(2,thresholds,false);
    Check(snapshot.bytes[2]==192 && snapshot.bytes[0]==512,"future snapshots distinguish actual current pool capacity from target request");
    pooledCandidate.Observe(2,0.5f);pooledCandidate.Snapshot(2,thresholds,false);
    request={{13,23},2,2,0,320,dtl::RequestReason::MeshUpgrade};
    Check(pooledCandidate.Accept(request,ledger,2,thresholds).bytes==320 && pooledCandidate.Complete(3,ledger,640),
        "restoration records native block slack without inventing physical free credit");
    Check(pooledCandidate.AllocatedBytes()==640 && !ledger.Snapshot().pendingOperations &&
        !ledger.Snapshot().observedDestroyedBytes,"published capacity and logical request completion remain separate");
    std::cout<<checks<<" FO4 mesh policy checks passed\n";
}
