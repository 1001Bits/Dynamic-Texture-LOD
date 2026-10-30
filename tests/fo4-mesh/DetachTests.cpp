#include "Fo4MeshDetach.h"
#include <cstdlib>
#include <iostream>
namespace {
    unsigned checks{};
    void Check(bool value,const char* why) {++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
    using Owners=Fo4Mesh::Pilot::OwnerProvenance;
    using Detach=Fo4Mesh::Pilot::DetachOwnership;
    Owners::State Proof() {return {7,100,10,20,Owners::Stage::Validated,1};}
    void Ready(Detach& state) {
        Check(state.Begin(Proof(),20,true),"certified placed clone starts transaction");
        Check(state.SourceVerified(true),"completed source GPU readback matches manifest");
        Check(state.OwnCandidate(200,1),"one created full candidate reference retained");
        Check(state.CandidateVerified(true,true),"ready candidate GPU bytes independently verified");
    }
}
int main() {
    Detach state;
    Check(!state.Published(),"empty transaction cannot manufacture transferred references");
    Check(!state.Begin(Proof(),10,true),"template is not detachable destination");
    Check(!state.Begin(Proof(),20,false),"preexisting cache references prevent capture");
    auto pending=Proof();pending.stage=Owners::Stage::Aliased;
    Check(!state.Begin(pending,20,true),"unvalidated derived metadata cannot detach");
    Ready(state);
    Check(!state.CanPublish(20,100,201,true) && !state.CanPublish(30,100,200,true),"new shape and borrowed geometry identity revalidated");
    Check(!state.CanPublish(20,101,200,true) && !state.CanPublish(20,100,200,false),"old shape change or stale metadata prevents publication");
    Check(state.CanPublish(20,100,200,true),"exact full candidate can transfer");
    Check(state.TakeSourceReferences()==0 && state.TakeCandidate()==0,"owned references cannot be drained during pending work");
    Check(state.Published(),"ready transaction publishes exactly once");
    Check(!state.Published(),"repeated publication cannot duplicate transferred ownership");
    Check(state.Candidate()==0 && state.SourceReferences()==2,"setter transfers candidate to geometry and former clone ref to plugin");
    Check(!state.Cancel(),"later cancellation cannot reclaim geometry-owned new shape");
    Check(state.TakeCandidate()==0,"published full native pair must not be plugin-released");
    Check(state.TakeSourceReferences()==2,"success releases source hold plus former clone exactly once");
    Check(state.TakeSourceReferences()==0,"duplicate cleanup releases no reference");
    Check(!state.Published() && state.TakeSourceReferences()==0,"publication cannot resurrect drained references");
    Check(!state.CanPublish(20,100,200,true),"transaction cannot publish twice");
    Detach before;Check(before.Begin(Proof(),20,true),"capture owns a single explicit source hold");
    Check(before.Cancel() && before.TakeSourceReferences()==1 && before.TakeCandidate()==0,"precreate cancellation releases only hold");
    Detach allocated;Ready(allocated);
    Check(allocated.Cancel(),"foreign ownership event cancels ready unpublished candidate");
    Check(!allocated.CanPublish(20,100,200,true),"count/address ABA cannot resurrect cancelled transaction");
    Check(allocated.TakeCandidate()==200 && allocated.TakeCandidate()==0,"cancel releases only plugin-owned candidate once");
    Check(allocated.TakeSourceReferences()==1,"cancel must not release clone's still-owned old reference");
    Detach mismatch;Check(mismatch.Begin(Proof(),20,true),"source hash failure setup");
    Check(!mismatch.SourceVerified(false) && mismatch.TakeSourceReferences()==1,"bad source cancels without publication");
    Detach copyFailure;Check(copyFailure.Begin(Proof(),20,true) && copyFailure.SourceVerified(true),"copy hash failure setup");
    Check(!copyFailure.OwnCandidate(100,1) && !copyFailure.OwnCandidate(200,2),"source aliases/shared candidates rejected");
    Check(copyFailure.OwnCandidate(200,1),"exclusive candidate accepted after valid creation");
    Check(!copyFailure.CandidateVerified(true,false),"bad candidate GPU bytes cancel");
    Check(copyFailure.TakeCandidate()==200 && copyFailure.TakeSourceReferences()==1,"hash-failed native candidate cleaned exactly once");
    std::cout<<checks<<" detach ownership checks passed\n";
}
