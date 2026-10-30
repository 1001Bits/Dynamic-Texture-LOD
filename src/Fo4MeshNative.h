#pragma once
#include "Fo4MeshData.h"
#include <d3d11.h>
#include <atomic>
#include <cstddef>

namespace Fo4Mesh {
    // Verified OG1.10.163 CreateTriShape1D0BC70, AddBufferToCreate1D49010,
    // CreateBufferRequested1D4A4F0 and DecRef1D0C5D0. These are native wrapper
    // objects, not Skyrim's direct COM-buffer layout. No allocator ownership
    // is implied by declaring the structs here.
    struct NativeBuffer {
        ID3D11Buffer* buffer{};
        void* data{};
        NativeBuffer* next{};
        ID3D11ShaderResourceView* shaderResource{};
        ID3D11UnorderedAccessView* unorderedAccess{};
        void* requestEvent{};
        std::uint32_t capacity{},size{},references{},shaderAcquires{},unorderedAcquires{},pendingRequests{},offset{};
        bool invalidCpuData{},suballocated{},pendingCopy{};
    };
    struct NativeShape {
        std::uint64_t descriptor{};
        NativeBuffer* vertices{};
        NativeBuffer* indices{};
        std::uint32_t references{};
    };
    static_assert(sizeof(NativeBuffer)==0x50 && offsetof(NativeBuffer,references)==0x38 &&
        offsetof(NativeBuffer,pendingRequests)==0x44 && offsetof(NativeBuffer,offset)==0x48 &&
        offsetof(NativeBuffer,suballocated)==0x4d && offsetof(NativeBuffer,pendingCopy)==0x4e);
    static_assert(sizeof(NativeShape)==0x20 && offsetof(NativeShape,vertices)==8 &&
        offsetof(NativeShape,indices)==0x10 && offsetof(NativeShape,references)==0x18);

    // Binding identity only, never an ownership claim. Callers keep the native
    // pair alive; a readback additionally retains the corresponding COM refs.
    struct PairIdentity {
        struct Window {
            NativeBuffer* wrapper{};ID3D11Buffer* resource{};
            std::uint32_t offset{},capacity{},size{};bool pooled{};
            static Window Capture(NativeBuffer* b) noexcept {
                return b ? Window{b,b->buffer,b->offset,b->capacity,b->size,b->suballocated} : Window{};
            }
            bool Matches(NativeBuffer* b) const noexcept {
                return b && b==wrapper && b->buffer==resource && b->offset==offset && b->capacity==capacity &&
                    b->size==size && b->suballocated==pooled;
            }
        };
        std::uint64_t descriptor{};Window vertices,indices;
        static PairIdentity Capture(const NativeShape& shape) noexcept {
            return {shape.descriptor,Window::Capture(shape.vertices),Window::Capture(shape.indices)};
        }
        bool Matches(const NativeShape& shape) const noexcept {
            return descriptor==shape.descriptor && vertices.Matches(shape.vertices) && indices.Matches(shape.indices);
        }
    };

    inline std::uint32_t AtomicValue(std::uint32_t& field) {
        return std::atomic_ref<std::uint32_t>(field).load(std::memory_order_acquire);
    }
    // Caller already holds native lifetime and the registry/cache exclusion
    // lock. A ready wrapper must be independently owning; a shared subrange
    // cannot be accounted or replaced as one standalone allocation.
    inline bool OwningReady(NativeBuffer* buffer,std::uint64_t logicalBytes) {
        return buffer && logicalBytes && logicalBytes<=UINT32_MAX-3 &&
            AtomicValue(buffer->pendingRequests)==0 && !buffer->pendingCopy &&
            AtomicValue(buffer->references)==1 && buffer->buffer && !buffer->suballocated && !buffer->offset &&
            AtomicValue(buffer->shaderAcquires)==0 && AtomicValue(buffer->unorderedAcquires)==0 &&
            buffer->capacity==AllocationBytes(logicalBytes) && buffer->size==buffer->capacity;
    }
    // Exclusive native wrapper ownership is distinct from heap ownership. A
    // pooled wrapper borrows the heap resource; replacing it frees one block,
    // and only a separately verified empty-heap release can drop the backing.
    // Resource bounds/device/overlap validation occurs in BeginRegions.
    inline bool ExclusiveWindowReady(NativeBuffer* buffer,std::uint64_t logicalBytes) {
        if (!buffer || !logicalBytes || logicalBytes>UINT32_MAX-3 ||
            AtomicValue(buffer->pendingRequests)!=0 || buffer->pendingCopy ||
            AtomicValue(buffer->references)!=1 || !buffer->buffer || AtomicValue(buffer->shaderAcquires)!=0 ||
            AtomicValue(buffer->unorderedAcquires)!=0 || buffer->size!=AllocationBytes(logicalBytes)) return false;
        if (!buffer->suballocated) return !buffer->offset && buffer->capacity==buffer->size;
        return !(buffer->offset&3) && buffer->capacity>=buffer->size &&
            std::uint64_t(buffer->offset)+buffer->capacity<=128ull*1024*1024;
    }
}
