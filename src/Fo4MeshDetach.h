#pragma once
#include "Fo4MeshOwners.h"

namespace Fo4Mesh::Pilot {
    // Bookkeeping for one real source AddRef and one freshly created full
    // native shape. The adapter performs the actual native calls. Identities
    // are opaque; no borrowed geometry is owned/dereferenced by this class.
    class DetachOwnership {
    public:
        enum class Stage : std::uint8_t { Empty, SourceHeld, SourceVerified, CandidateOwned, Ready, Published, Cancelled };
        bool Begin(const OwnerProvenance::State& proof,std::uint64_t geometry,bool exclusiveCacheWindow) noexcept {
            if (stage_!=Stage::Empty || proof.stage!=OwnerProvenance::Stage::Validated || !proof.certificate ||
                geometry!=proof.destination || !exclusiveCacheWindow) return false;
            proof_=proof;stage_=Stage::SourceHeld;sourceReferences_=1;return true;
        }
        bool SourceVerified(bool gpuHashMatches) noexcept {
            if (stage_!=Stage::SourceHeld || !gpuHashMatches) {Cancel();return false;}
            stage_=Stage::SourceVerified;return true;
        }
        bool OwnCandidate(std::uint64_t native,std::uint32_t references) noexcept {
            if (stage_!=Stage::SourceVerified || !native || native==proof_.native || references!=1) return false;
            candidate_=native;stage_=Stage::CandidateOwned;return true;
        }
        bool CandidateVerified(bool fullPairReady,bool gpuHashMatches) noexcept {
            if (stage_!=Stage::CandidateOwned || !fullPairReady || !gpuHashMatches) {Cancel();return false;}
            stage_=Stage::Ready;return true;
        }
        bool CanPublish(std::uint64_t geometry,std::uint64_t oldNative,std::uint64_t newNative,
                        bool exactBorrowedIdentity) const noexcept {
            return stage_==Stage::Ready && sourceReferences_==1 && geometry==proof_.destination &&
                oldNative==proof_.native && newNative==candidate_ && exactBorrowedIdentity;
        }
        // Reserve immediately before the verified non-callback leaf setter,
        // under the adapter lock. Its refs1 ownership moves to geometry;
        // geometry's former old reference moves to us in the same interval.
        bool Published() noexcept {
            if (stage_!=Stage::Ready || !candidate_ || sourceReferences_!=1) return false;
            stage_=Stage::Published;sourceReferences_=2;candidate_=0;return true;
        }
        bool Cancel() noexcept {
            if (stage_==Stage::Empty || stage_==Stage::Published || stage_==Stage::Cancelled) return false;
            stage_=Stage::Cancelled;return true;
        }
        bool Pending() const noexcept { return stage_!=Stage::Empty && stage_!=Stage::Published && stage_!=Stage::Cancelled; }
        bool Finished() const noexcept { return stage_==Stage::Published || stage_==Stage::Cancelled; }
        Stage GetStage() const noexcept { return stage_; }
        const OwnerProvenance::State& Proof() const noexcept { return proof_; }
        std::uint8_t SourceReferences() const noexcept { return sourceReferences_; }
        std::uint8_t TakeSourceReferences() noexcept {
            if (!Finished()) return 0;
            const auto result=sourceReferences_;sourceReferences_=0;return result;
        }
        std::uint64_t TakeCandidate() noexcept {
            if (stage_!=Stage::Cancelled) return 0;
            const auto result=candidate_;candidate_=0;return result;
        }
        std::uint64_t Candidate() const noexcept { return candidate_; }
    private:
        OwnerProvenance::State proof_;
        Stage stage_{Stage::Empty};
        std::uint64_t candidate_{};
        std::uint8_t sourceReferences_{};
    };
}
