#pragma once
#include <cstdint>
#include <d3d11.h>

namespace Fo4Mesh::HeapHooks {
    struct Statistics {
        bool installed{};
        std::uint64_t framePumps{},detachedHeaps{},releasedReferences{},releasedReferenceBytes{};
        std::uint64_t eligibleEmptyHeaps{},retiringSkipped{},untrackedSkipped{},unsafePhaseSkipped{};
        // Diagnostic only. A backing is observed under its allocator owner,
        // never by a later worker dereferencing a remembered raw pointer.
        std::uint64_t lifetimeHeapObservations{},lifetimeHeapInterfaceFailures{};
    };
    using FrameCallback=void(*)(void* creator,ID3D11DeviceContext* actualContext);
    // Called only by the final verified mesh installation transaction. The
    // native callsite and all allocator/lifecycle endpoints are validated as
    // one set before the first patch. Exact OG1.10.163 only.
    bool Install(FrameCallback);
    // This is an explicit subfeature gate, separate from hook installation.
    // Frame callback servicing can be installed while heap release stays off.
    void SetReclaimEnabled(bool) noexcept;
    // Called only from the verified native frame callback, while the retiring
    // native pair still owns its wrappers. Marks the heap affected by an
    // actual mesh replacement; unrelated empty native heaps are not trimmed.
    void MarkMeshPoolRelease(ID3D11Buffer* backing);
    Statistics GetStatistics() noexcept;
}
