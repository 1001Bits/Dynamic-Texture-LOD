#include "Fo4MeshPolicy.h"
#include <cmath>

namespace Fo4Mesh {
    PolicyState::PolicyState(dtl::TextureIdentity identity,std::uint64_t revision,PrefixLevels levels,
                             std::uint64_t initialAllocatedBytes):
        identity_(identity),revision_(revision),levels_(levels),
        allocatedBytes_(initialAllocatedBytes ? initialAllocatedBytes : levels.levels[0].bytes) {}
    void PolicyState::Observe(std::uint64_t round,float rank) {
        if (!round || !std::isfinite(rank) || rank<0) return;
        if (round>=observation_.round) observation_.Visit(round,true,0,rank);
        else if (round+1==observation_.round) {
            // A completed-view callback may arrive after the worker captured
            // the next gather round. Preserve its maximum in the older slot.
            observation_.priority[1]=std::max(observation_.priority[1],rank);
            observation_.lastSeen=std::max(observation_.lastSeen,round);
        }
    }
    std::uint8_t PolicyState::Wanted(const std::array<float,4>& thresholds) const {
        return dtl::StarfieldWantedLevel(observation_.Priority(),levels_.count ? levels_.count-1 : 0,thresholds);
    }
    dtl::PlanEntry PolicyState::Snapshot(std::uint64_t round,const std::array<float,4>& thresholds,bool pin) {
        observation_.Advance(round);
        dtl::PlanEntry result;
        result.key=identity_.key;result.generation=identity_.generation;result.resourceIdentity=revision_;
        result.current=current_;result.previous=target_;result.lowest=levels_.count ? levels_.count-1 : 0;
        result.seen=observation_.Seen(round);result.priority=observation_.Priority();result.wanted=Wanted(thresholds);
        result.pending=Pending();result.noDegrade=pin || levels_.count<2;
        for (unsigned i=0;i<levels_.count;++i) result.bytes[i]=levels_.levels[i].bytes;
        if (current_<levels_.count) result.bytes[current_]=allocatedBytes_;
        snapshotSeen_=result.seen;snapshotWanted_=result.wanted;pinned_=result.noDegrade;
        return result;
    }
    dtl::RequestReceipt PolicyState::Accept(const dtl::PlanRequest& request,dtl::StreamingLedger& ledger,
                                           std::uint64_t round,const std::array<float,4>& thresholds) {
        if (request.reason!=dtl::RequestReason::MeshUpgrade && request.reason!=dtl::RequestReason::MeshDegrade &&
            request.reason!=dtl::RequestReason::MeshEmergency) return {};
        dtl::PlanEntry live;
        live.key=identity_.key;live.generation=identity_.generation;live.resourceIdentity=revision_;
        live.current=current_;live.lowest=levels_.count ? levels_.count-1 : 0;live.pending=Pending();
        if (!dtl::MatchesRequest(live,request) || request.target>=levels_.count || request.target==current_ ||
            (pinned_ && request.target>current_) || observation_.Seen(round)!=snapshotSeen_ || Wanted(thresholds)!=snapshotWanted_)
            return {dtl::RequestStatus::Stale,0};
        const auto before=allocatedBytes_,after=levels_.levels[request.target].bytes;
        const auto ticket=ledger.Request(identity_,before,after);
        if (!ticket) return {};
        ticket_=ticket;target_=request.target;acceptedSeen_=snapshotSeen_;acceptedWanted_=snapshotWanted_;
        return {dtl::RequestStatus::AcceptedPending,before>after ? before-after : after-before};
    }
    bool PolicyState::Fresh(std::uint64_t round,const std::array<float,4>& thresholds) const {
        return Pending() && observation_.Seen(round)==acceptedSeen_ && Wanted(thresholds)==acceptedWanted_;
    }
    bool PolicyState::Publishable(std::uint64_t round,const std::array<float,4>& thresholds,PublicationGuards guards) const {
        return guards.healthy && guards.unique && !guards.forceFull &&
            (!guards.pinned || target_<current_) && Fresh(round,thresholds);
    }
    bool PolicyState::Complete(std::uint64_t nextRevision,dtl::StreamingLedger& ledger,std::uint64_t actualAllocatedBytes) {
        // A native pair publication is required. Event completion by itself
        // must never call this or create destruction credit in the ledger.
        const auto bytes=actualAllocatedBytes ? actualAllocatedBytes : levels_.levels[target_].bytes;
        if (!Pending() || nextRevision<=revision_ || bytes<levels_.levels[target_].bytes ||
            !ledger.CompleteChange(ticket_,identity_)) return false;
        revision_=nextRevision;current_=target_;allocatedBytes_=bytes;ticket_=0;return true;
    }
    bool PolicyState::Cancel(dtl::StreamingLedger& ledger) {
        if (!Pending()) return false;
        const bool result=ledger.CancelChange(ticket_,identity_);
        ticket_=0;target_=current_;return result;
    }
    bool PolicyState::FullSourcePublished(std::uint64_t nextRevision,dtl::StreamingLedger& ledger,std::uint64_t actualAllocatedBytes) {
        const auto bytes=actualAllocatedBytes ? actualAllocatedBytes : levels_.levels[0].bytes;
        if (nextRevision<=revision_ || bytes<levels_.levels[0].bytes) return false;
        Cancel(ledger);revision_=nextRevision;current_=target_=0;allocatedBytes_=bytes;pinned_=true;return true;
    }
}
