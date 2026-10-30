#pragma once
#include <cstdint>

namespace Fo4Mesh::Pilot {
    // One hash-pinned fixture, one native shape, two attributed geometry
    // owners. Addresses are opaque identities: this class never dereferences
    // them or owns native references. The adapter serializes every operation.
    // A sampled count of two is never sufficient to create a certificate.
    class OwnerProvenance {
    public:
        enum class Stage : std::uint8_t { Empty, Copying, Aliased, Validated, Invalid };
        struct State {
            std::uint64_t certificate{}, native{}, source{}, destination{};
            Stage stage{Stage::Empty};
            std::uint32_t matchedAdds{};
        };
        const State& Get() const noexcept { return state_; }
        std::uint64_t Begin(std::uint64_t native,std::uint64_t source,std::uint64_t destination,
                            std::uint32_t references,bool sourceAudited,bool destinationExactType) noexcept {
            if (state_.stage==Stage::Copying) {state_.stage=Stage::Invalid;return 0;}
            if (state_.stage==Stage::Aliased || state_.stage==Stage::Validated) return 0;
            if (!native || !source || !destination || source==destination || references!=1 ||
                !sourceAudited || !destinationExactType || next_==UINT64_MAX) return 0;
            state_={++next_,native,source,destination,Stage::Copying,0};
            return state_.certificate;
        }
        // copyScope is a thread-local ticket set only around the audited outer
        // BSTriShape CopyMembers call. Concurrent/unattributed increments and
        // a second increment invalidate even if the count later returns to2.
        bool AddRef(std::uint64_t native,std::uint32_t before,std::uint64_t copyScope) noexcept {
            if (native!=state_.native) return false;
            if (state_.stage==Stage::Copying && copyScope==state_.certificate && before==1 && !state_.matchedAdds) {
                ++state_.matchedAdds;return true;
            }
            Invalidate(native);return false;
        }
        bool Complete(std::uint64_t certificate,std::uint32_t references,bool aliasesVerified,
                      bool sourceStillAudited,bool destinationRigid) noexcept {
            if (!certificate || certificate!=state_.certificate) return false;
            if (state_.stage!=Stage::Copying || state_.matchedAdds!=1 || references!=2 ||
                !aliasesVerified || !sourceStillAudited || !destinationRigid) {
                state_.stage=Stage::Invalid;return false;
            }
            // Derived BSMesh CopyMembers copies its segment counts after the
            // base call returns. Require a fresh borrowed destination visit.
            state_.stage=Stage::Aliased;return true;
        }
        bool ValidateDestination(std::uint64_t native,std::uint64_t geometry,std::uint32_t references,
                                 bool exactFinalMetadata) noexcept {
            if (native!=state_.native || geometry!=state_.destination || state_.stage!=Stage::Aliased) return false;
            if (references!=2 || !exactFinalMetadata) {state_.stage=Stage::Invalid;return false;}
            state_.stage=Stage::Validated;return true;
        }
        bool Certifies(std::uint64_t native,std::uint32_t references,std::uint64_t certificate) const noexcept {
            return certificate && state_.certificate==certificate && state_.native==native &&
                state_.stage==Stage::Validated && references==2;
        }
        bool Includes(std::uint64_t native,std::uint64_t geometry,std::uint32_t references,
                      std::uint64_t certificate) const noexcept {
            return Certifies(native,references,certificate) &&
                (geometry==state_.source || geometry==state_.destination);
        }
        bool CopyIncludes(std::uint64_t geometry,std::uint64_t copyScope) const noexcept {
            return copyScope && state_.stage==Stage::Copying && state_.certificate==copyScope &&
                (geometry==state_.source || geometry==state_.destination);
        }
        bool Invalidate(std::uint64_t native) noexcept {
            if (native!=state_.native || state_.stage==Stage::Empty || state_.stage==Stage::Invalid) return false;
            state_.stage=Stage::Invalid;return true;
        }
        bool HasAlias(std::uint64_t geometry) const noexcept {
            return geometry && (geometry==state_.source || geometry==state_.destination);
        }
    private:
        State state_;
        std::uint64_t next_{};
    };
}
