#pragma once
#include "Fo4MeshNative.h"
#include <utility>

namespace Fo4Mesh {
    // Borrowed full-source binding, never an ownership publication. Caller
    // serializes the entire native SetupGeometry->Draw->RestoreGeometry span,
    // proves those consumers retain no wrapper pointer, and uploads the full
    // reserve using that draw's actual context before constructing this guard.
    class ScopedNativePair {
    public:
        ScopedNativePair(NativeShape& owner,const NativeShape& full) noexcept {
            if (!owner.vertices || !owner.indices || !full.vertices || !full.indices ||
                owner.descriptor!=full.descriptor) return;
            owner_=&owner;vertices_=std::exchange(owner.vertices,full.vertices);
            indices_=std::exchange(owner.indices,full.indices);
        }
        ~ScopedNativePair() {
            if (owner_) {owner_->vertices=vertices_;owner_->indices=indices_;}
        }
        ScopedNativePair(const ScopedNativePair&)=delete;
        ScopedNativePair& operator=(const ScopedNativePair&)=delete;
        explicit operator bool() const noexcept {return owner_!=nullptr;}
    private:
        NativeShape* owner_{};
        NativeBuffer* vertices_{};
        NativeBuffer* indices_{};
    };
}
