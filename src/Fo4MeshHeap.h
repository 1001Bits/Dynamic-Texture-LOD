#pragma once

#include <Unknwn.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Fo4Mesh {
    constexpr std::uint32_t NativeHeapBytes=128u<<20;

    // OG1.10.163 StoreBlock address-tree links. The second intrusive tree and
    // free-list fields are deliberately opaque: this adapter never changes
    // allocation metadata, even when it transfers the backing COM reference.
    struct NativeHeapBlock {
        NativeHeapBlock* left{};
        NativeHeapBlock* right{};
        std::uintptr_t opaque10{},parentAndColour{};
        std::array<std::byte,0x30> opaque20{};
        std::uint32_t offset{},capacityAndFlags{};
    };
    struct NativeHeap {
        std::array<std::byte,0x100> freeMetadata{};
        void* blockPool{};
        void* opaque108{};
        NativeHeapBlock* addressRoot{};
        NativeHeapBlock* firstAddress{};
        IUnknown* backing{};
        std::uint32_t freeBytes{},usedBytes{};
    };
    static_assert(sizeof(NativeHeapBlock)==0x58 && offsetof(NativeHeapBlock,offset)==0x50);
    static_assert(sizeof(NativeHeap)==0x130 && offsetof(NativeHeap,backing)==0x120);

    // Requires native allocator lifetime + endpoint exclusion before any
    // dereference. A completely coalesced address-tree leaf is deliberately
    // stricter than just freeBytes==128MiB: cached small free blocks stay native.
    bool CompletelyEmpty(const NativeHeap&) noexcept;

    struct HeapReleaseStats {
        std::uint64_t detached{},releasedReferences{},queueFull{};
        std::uint64_t pendingBytes{},releasedReferenceBytes{};
    };

    // No allocation, AddRef, GPU call or engine call occurs during Detach.
    // The heap's single owned reference transfers into this bounded queue.
    // Drain belongs at a verified native renderer phase after PresentBarrier
    // clears, under the SAME endpoint/lifecycle lock used for Detach. The
    // caller must never hold that lock across a wait for the renderer thread.
    // A released COM reference is not proof of physical GPU memory destruction.
    class HeapReleaseQueue {
    public:
        static constexpr std::size_t Capacity=16;
        HeapReleaseQueue()=default;
        HeapReleaseQueue(const HeapReleaseQueue&)=delete;
        HeapReleaseQueue& operator=(const HeapReleaseQueue&)=delete;
        bool Detach(NativeHeap&) noexcept;
        void Drain() noexcept;
        HeapReleaseStats Stats() const noexcept {return stats_;}
    private:
        // Intentionally explicit Drain, with no GPU work in a static destructor.
        // Abnormal process teardown can retain these references until OS exit.
        std::array<IUnknown*,Capacity> resources_{};
        std::size_t size_{};
        HeapReleaseStats stats_;
    };
}
