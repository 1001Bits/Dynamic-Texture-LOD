#include "Fo4MeshHeap.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks;
void Check(bool condition,const char* message) {
    ++checks;if (!condition) throw std::runtime_error(message);
}
struct Resource final : IUnknown {
    ULONG references=1,adds=0,releases=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override {return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {++adds;return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {++releases;return --references;}
};
struct Heap {
    Resource resource;
    Fo4Mesh::NativeHeapBlock block;
    Fo4Mesh::NativeHeap native;
    Heap() {
        block.capacityAndFlags=Fo4Mesh::NativeHeapBytes;
        native.backing=&resource;native.blockPool=this;
        native.addressRoot=native.firstAddress=&block;
        native.freeBytes=Fo4Mesh::NativeHeapBytes;
    }
};
void Test() {
    using namespace Fo4Mesh;
    Heap heap;HeapReleaseQueue queue;
    Check(CompletelyEmpty(heap.native),"initial coalesced native heap eligible");
    const auto rejected=[&](const char* message) {
        Check(!queue.Detach(heap.native) && heap.native.backing==&heap.resource &&
            !heap.resource.adds && !heap.resource.releases && !queue.Stats().detached,message);
    };
    heap.native.usedBytes=4;rejected("live bytes block detachment");heap.native.usedBytes=0;
    --heap.native.freeBytes;rejected("inconsistent free count blocks detachment");++heap.native.freeBytes;
    heap.block.capacityAndFlags|=0x80000000u;rejected("allocated node blocks detachment");heap.block.capacityAndFlags&=0x7fffffffu;
    heap.block.offset=4;rejected("free window must start zero");heap.block.offset=0;
    heap.block.capacityAndFlags=NativeHeapBytes-4;rejected("partial range blocks detachment");heap.block.capacityAndFlags=NativeHeapBytes;
    heap.block.left=&heap.block;rejected("multiple address nodes block detachment");heap.block.left=nullptr;
    heap.block.right=&heap.block;rejected("second address child blocks detachment");heap.block.right=nullptr;
    heap.block.parentAndColour=0x1000;rejected("external parent blocks detachment");heap.block.parentAndColour=1;
    heap.native.addressRoot=nullptr;rejected("address tree must match first leaf");heap.native.addressRoot=&heap.block;
    heap.native.blockPool=nullptr;rejected("uninitialized heap blocks detachment");heap.native.blockPool=&heap;
    std::array<std::byte,sizeof(NativeHeap)> before;
    std::memcpy(before.data(),&heap.native,before.size());
    Check(queue.Detach(heap.native),"complete heap ownership transferred");
    Check(!heap.native.backing && heap.resource.references==1 && !heap.resource.adds && !heap.resource.releases,
        "detach neither AddRefs nor releases engine reference");
    auto expected=heap.native;expected.backing=&heap.resource;
    Check(std::memcmp(before.data(),&expected,before.size())==0,"all native free metadata is preserved byte for byte");
    Check(!queue.Detach(heap.native) && queue.Stats().detached==1,"cannot detach same reference twice");
    Heap alias;alias.native.backing=&heap.resource;
    Check(!queue.Detach(alias.native) && alias.native.backing==&heap.resource,
        "duplicate heap backing cannot invent a second owned reference");
    Check(queue.Stats().pendingBytes==NativeHeapBytes && !queue.Stats().releasedReferences,"pending COM bytes separate from release");
    Resource replacement;heap.native.backing=&replacement; // native lazy backing creation
    Check(CompletelyEmpty(heap.native),"preserved metadata remains valid for native lazy recreation");
    queue.Drain();
    Check(heap.resource.references==0 && heap.resource.releases==1 && !replacement.releases &&
        heap.native.backing==&replacement,"release old backing exactly once without touching replacement");
    Check(queue.Stats().pendingBytes==0 && queue.Stats().releasedReferenceBytes==NativeHeapBytes,
        "release accounting states only owned-reference bytes");
    queue.Drain();Check(heap.resource.releases==1,"draining twice cannot double release");

    std::array<Heap,HeapReleaseQueue::Capacity+1> heaps;
    for (std::size_t i=0;i<HeapReleaseQueue::Capacity;++i)
        Check(queue.Detach(heaps[i].native),"bounded ownership slot accepts reference");
    Check(!queue.Detach(heaps.back().native) && heaps.back().native.backing==&heaps.back().resource &&
        !heaps.back().resource.releases && queue.Stats().queueFull==1,"full queue leaves native ownership intact");
    queue.Drain();
    for (std::size_t i=0;i<HeapReleaseQueue::Capacity;++i)
        Check(heaps[i].resource.releases==1 && !heaps[i].resource.adds,"each transferred reference released once");
    Check(queue.Detach(heaps.back().native),"queue can be reused after safe phase drain");queue.Drain();

    // An external readback or D3D binding can still own the resource. A native
    // reference release is deliberately not labelled physical memory freeing.
    Heap retained;retained.resource.AddRef();Check(queue.Detach(retained.native),"externally referenced backing detachable");
    queue.Drain();Check(retained.resource.references==1,"external reference survives native heap release");
    retained.resource.Release();
}
}
int main() {
    try {Test();std::cout<<checks<<" native heap ownership checks passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
