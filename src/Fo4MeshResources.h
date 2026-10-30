#pragma once

#include "Fo4MeshData.h"
#include "Fo4MeshNative.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace Fo4Mesh {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Query> Completion(ID3D11Device* device);
    HRESULT PollCompletion(ID3D11DeviceContext* context, ID3D11Query* query);

    struct BufferRegion {
        ID3D11Buffer* buffer{};
        std::uint32_t offset{},capacity{};
        bool suballocated{};
    };

    // Full immutable restoration source, copied from the actual GPU pair.
    // No mapped pointer escapes Poll; every successful result owns its bytes.
    // Begin/Poll are called exclusively by the owner of the immediate context.
    class Readback {
    public:
        static std::optional<Readback> Begin(ID3D11Device* device, ID3D11DeviceContext* context,
            ID3D11Buffer* vertices, ID3D11Buffer* indices, std::uint32_t stride,
            std::uint32_t vertexCount, std::uint32_t indexCount,
            std::uint64_t maximumBytes = 8ull << 20);
        // Copies only each validated native allocation window. A retained COM
        // pointer protects the pool resource, not the native suballocation;
        // caller must separately verify wrapper generation/offset after Poll.
        static std::optional<Readback> BeginRegions(ID3D11Device* device, ID3D11DeviceContext* context,
            BufferRegion vertices, BufferRegion indices, std::uint32_t stride,
            std::uint32_t vertexCount, std::uint32_t indexCount,
            std::uint64_t maximumBytes = 8ull << 20);
        HRESULT Poll(ID3D11DeviceContext* context, Data& output);
        std::uint64_t Bytes() const { return allocationBytes_; }
        ID3D11Buffer* SourceVertices() const { return originalVertices_.Get(); }
        ID3D11Buffer* SourceIndices() const { return originalIndices_.Get(); }
        std::uint32_t SourceVertexOffset() const { return vertexOffset_; }
        std::uint32_t SourceIndexOffset() const { return indexOffset_; }
    private:
        ComPtr<ID3D11Buffer> originalVertices_, originalIndices_, vertices_, indices_;
        ComPtr<ID3D11Query> completion_;
        std::uint32_t vertexBytes_ = 0, indexBytes_ = 0, stride_ = 0;
        std::uint64_t allocationBytes_ = 0;
        std::uint32_t vertexOffset_ = 0,indexOffset_ = 0;
    };

    // Emergency draw-only reserve. Never publish these plugin-owned native
    // wrappers into an engine shape or pass them to native DecRef. Native
    // DrawTriShape consumes them synchronously; this path additionally needs
    // a verified shader/pass with no separate geometry-buffer SRV binding.
    class DrawReserve {
    public:
        static std::optional<DrawReserve> Create(ID3D11Device*,std::uint32_t vertexBytes,std::uint32_t indexBytes);
        HRESULT Prepare(ID3D11DeviceContext* actualDrawContext,SourceView,std::uint64_t descriptor);
        NativeShape* Shape();
        std::uint64_t Bytes() const { return std::uint64_t(vertices_.capacity)+indices_.capacity; }
    private:
        ComPtr<ID3D11Buffer> vertexBuffer_,indexBuffer_;
        std::vector<std::byte> indexUpload_;
        NativeBuffer vertices_,indices_;
        NativeShape shape_;
    };
}
