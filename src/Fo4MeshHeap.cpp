#include "Fo4MeshHeap.h"
#include <utility>

namespace Fo4Mesh {
bool CompletelyEmpty(const NativeHeap& heap) noexcept {
    const auto* block=heap.firstAddress;
    return heap.backing && heap.blockPool && heap.freeBytes==NativeHeapBytes && !heap.usedBytes &&
        block && heap.addressRoot==block && !block->left && !block->right &&
        !(block->parentAndColour&~std::uintptr_t{1}) && !block->offset &&
        !(block->capacityAndFlags&0x80000000u) &&
        (block->capacityAndFlags&0x1fffffffu)==NativeHeapBytes;
}

bool HeapReleaseQueue::Detach(NativeHeap& heap) noexcept {
    if (!CompletelyEmpty(heap)) return false;
    // Native heaps allocate independent backing resources. A duplicate
    // pointer cannot be justified as a second owned reference by this adapter.
    for (std::size_t i=0;i<size_;++i) if (resources_[i]==heap.backing) return false;
    if (size_==resources_.size()) {++stats_.queueFull;return false;}
    // All potentially failing checks precede the one native mutation. The
    // native allocator already recreates a null backing without changing its
    // block trees; ownership of the previous backing now belongs to this slot.
    resources_[size_++]=std::exchange(heap.backing,nullptr);
    ++stats_.detached;stats_.pendingBytes+=NativeHeapBytes;
    return true;
}

void HeapReleaseQueue::Drain() noexcept {
    while (size_) {
        auto* resource=std::exchange(resources_[--size_],nullptr);
        resource->Release();
        ++stats_.releasedReferences;stats_.releasedReferenceBytes+=NativeHeapBytes;
        stats_.pendingBytes-=NativeHeapBytes;
    }
}
}
