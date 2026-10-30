#include "Fo4MeshResources.h"
#include "Fo4MeshLifetime.h"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace Fo4Mesh {
    ComPtr<ID3D11Query> Completion(ID3D11Device* device) {
        ComPtr<ID3D11Query> result;
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
        if (!device || FAILED(device->CreateQuery(&description, result.GetAddressOf()))) return {};
        return result;
    }
    HRESULT PollCompletion(ID3D11DeviceContext* context, ID3D11Query* query) {
        if (!context || !query) return E_INVALIDARG;
        BOOL complete = FALSE;
        const auto result = context->GetData(query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        return result == S_OK && !complete ? S_FALSE : result;
    }
    namespace {
        bool SameDevice(ID3D11DeviceChild* child, ID3D11Device* expected) {
            if (!child) return false;
            ComPtr<ID3D11Device> actual;
            child->GetDevice(actual.GetAddressOf());
            return actual.Get() == expected;
        }
        ComPtr<ID3D11Buffer> Staging(ID3D11Device* device, const D3D11_BUFFER_DESC& original,UINT bytes) {
            auto desc = original;
            desc.ByteWidth=bytes;
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0; desc.StructureByteStride = 0;
            ComPtr<ID3D11Buffer> result;
            if (FAILED(device->CreateBuffer(&desc, nullptr, result.GetAddressOf()))) return {};
            // This helper owns the newly created interface even if a later
            // allocation/query fails. Observe that partial-attempt cost too.
            (void)Lifetime::Attach(result.Get(),Lifetime::Role::SourceStaging);
            return result;
        }
    }
    std::optional<Readback> Readback::Begin(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Buffer* vertices, ID3D11Buffer* indices, std::uint32_t stride,
        std::uint32_t vertexCount, std::uint32_t indexCount, std::uint64_t maximumBytes) {
        if (!vertices || !indices) return {};
        D3D11_BUFFER_DESC vb{},ib{};vertices->GetDesc(&vb);indices->GetDesc(&ib);
        return BeginRegions(device,context,{vertices,0,vb.ByteWidth,false},{indices,0,ib.ByteWidth,false},
            stride,vertexCount,indexCount,maximumBytes);
    }
    std::optional<Readback> Readback::BeginRegions(ID3D11Device* device, ID3D11DeviceContext* context,
        BufferRegion vertices,BufferRegion indices,std::uint32_t stride,
        std::uint32_t vertexCount,std::uint32_t indexCount,std::uint64_t maximumBytes) {
        if (!device || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !SameDevice(context, device) || !SameDevice(vertices.buffer, device) || !SameDevice(indices.buffer, device) ||
            !stride || stride > 60 || !vertexCount || vertexCount > UINT16_MAX || !indexCount ||
            indexCount % 3 || indexCount > UINT32_MAX / 2) return {};
        D3D11_BUFFER_DESC vb{}, ib{};
        vertices.buffer->GetDesc(&vb); indices.buffer->GetDesc(&ib);
        // Native ordinary owning wrappers use raw SRV/UAV-capable buffers:
        // CreateBufferRequested OG1D4A4F0 sets VB0x89/IB0x8A + Misc0x20.
        // All resource views stay with their native wrapper; this helper only
        // reads the GPU pair. Suballocation/split exclusion belongs to owner.
        const auto valid = [](const D3D11_BUFFER_DESC& desc,BufferRegion region,UINT bind,UINT logicalBytes) {
            if (!region.capacity || region.offset>desc.ByteWidth || region.capacity>desc.ByteWidth-region.offset) return false;
            const bool owning=!region.suballocated && region.offset==0 && region.capacity==desc.ByteWidth;
            const bool plain=owning && desc.BindFlags==bind && !desc.MiscFlags && region.capacity==logicalBytes;
            constexpr UINT views=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            constexpr UINT allowed=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|views;
            const bool extent=region.suballocated ? region.capacity>=AllocationBytes(logicalBytes) : region.capacity==AllocationBytes(logicalBytes);
            const bool native=(desc.BindFlags&(bind|views))==(bind|views) && !(desc.BindFlags&~allowed) &&
                desc.MiscFlags==D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS && extent &&
                !(region.offset&3) && (owning || region.suballocated);
            return desc.ByteWidth && (plain || native) && !desc.CPUAccessFlags && !desc.StructureByteStride &&
                (desc.Usage == D3D11_USAGE_DEFAULT || desc.Usage == D3D11_USAGE_IMMUTABLE);
        };
        const auto vertexBytes = vertexCount * stride, indexBytes = indexCount * 2;
        const auto vertexCopyBytes=vertices.suballocated ? static_cast<UINT>(AllocationBytes(vertexBytes)) : vertices.capacity;
        const auto indexCopyBytes=indices.suballocated ? static_cast<UINT>(AllocationBytes(indexBytes)) : indices.capacity;
        if (!valid(vb,vertices,D3D11_BIND_VERTEX_BUFFER,vertexBytes) || !valid(ib,indices,D3D11_BIND_INDEX_BUFFER,indexBytes) ||
            std::uint64_t(vertexCopyBytes)+indexCopyBytes>maximumBytes) return {};
        if (vertices.buffer==indices.buffer && vertices.offset<std::uint64_t(indices.offset)+indices.capacity &&
            indices.offset<std::uint64_t(vertices.offset)+vertices.capacity) return {};
        Readback capture;
        capture.vertexBytes_ = vertexBytes; capture.indexBytes_ = indexBytes; capture.stride_ = stride;
        capture.allocationBytes_ = std::uint64_t(vertexCopyBytes)+indexCopyBytes;
        capture.vertexOffset_=vertices.offset;capture.indexOffset_=indices.offset;
        capture.originalVertices_=vertices.buffer;capture.originalIndices_=indices.buffer;
        // Native suballocation lifetime remains the caller's responsibility;
        // these COM references now protect the resource during attachment.
        // Shared heap ByteWidth is counted once, not once per mesh window.
        // Repeated membership is explicitly uncertain in Lifetime's receipt.
        (void)Lifetime::Attach(capture.originalVertices_.Get(),vertices.suballocated ?
            Lifetime::Role::NativeHeap : Lifetime::Role::DedicatedMesh);
        (void)Lifetime::Attach(capture.originalIndices_.Get(),indices.suballocated ?
            Lifetime::Role::NativeHeap : Lifetime::Role::DedicatedMesh);
        capture.vertices_=Staging(device,vb,vertexCopyBytes);capture.indices_=Staging(device,ib,indexCopyBytes);
        capture.completion_ = Completion(device);
        if (!capture.vertices_ || !capture.indices_ || !capture.completion_) return {};
        const auto copy=[context](ID3D11Buffer* target,BufferRegion source,UINT bytes) {
            if (!source.suballocated) context->CopyResource(target,source.buffer);
            else {
                const D3D11_BOX box{source.offset,0,0,source.offset+bytes,1,1};
                context->CopySubresourceRegion(target,0,0,0,0,source.buffer,0,&box);
            }
        };
        copy(capture.vertices_.Get(),vertices,vertexCopyBytes);copy(capture.indices_.Get(),indices,indexCopyBytes);
        context->End(capture.completion_.Get());
        return capture;
    }
    HRESULT Readback::Poll(ID3D11DeviceContext* context, Data& output) {
        ComPtr<ID3D11Device> device;
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !completion_) return E_INVALIDARG;
        completion_->GetDevice(device.GetAddressOf());
        if (!SameDevice(context, device.Get())) return E_INVALIDARG;
        const auto ready = PollCompletion(context, completion_.Get());
        if (ready != S_OK) return ready;
        Data source;
        source.stride = stride_;
        try { source.vertices.resize(vertexBytes_); source.indices.resize(indexBytes_ / 2); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto result = context->Map(vertices_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(result)) return result;
        std::memcpy(source.vertices.data(), mapped.pData, vertexBytes_);
        context->Unmap(vertices_.Get(), 0);
        result = context->Map(indices_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(result)) return result;
        std::memcpy(source.indices.data(), mapped.pData, indexBytes_);
        context->Unmap(indices_.Get(), 0);
        const auto count = source.vertices.size() / stride_;
        for (const auto index : source.indices) if (index >= count) return E_INVALIDARG;
        output = std::move(source);
        return S_OK;
    }

    std::optional<DrawReserve> DrawReserve::Create(ID3D11Device* device,std::uint32_t vertexBytes,std::uint32_t indexBytes) {
        if (!device || !vertexBytes || !indexBytes || vertexBytes>UINT32_MAX-3 || indexBytes>UINT32_MAX-3) return {};
        DrawReserve result;
        try { result.indexUpload_.resize(static_cast<std::size_t>(AllocationBytes(indexBytes))); }
        catch(const std::bad_alloc&) { return {}; }
        auto create=[device](UINT bytes,UINT bind,ComPtr<ID3D11Buffer>& output) {
            const D3D11_BUFFER_DESC desc{static_cast<UINT>(AllocationBytes(bytes)),D3D11_USAGE_DEFAULT,
                bind|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
            const auto result=device->CreateBuffer(&desc,nullptr,output.GetAddressOf());
            if (SUCCEEDED(result)) (void)Lifetime::Attach(output.Get(),Lifetime::Role::DrawReserve);
            return result;
        };
        if (FAILED(create(vertexBytes,D3D11_BIND_VERTEX_BUFFER,result.vertexBuffer_)) ||
            FAILED(create(indexBytes,D3D11_BIND_INDEX_BUFFER,result.indexBuffer_))) return {};
        result.vertices_.buffer=result.vertexBuffer_.Get();result.indices_.buffer=result.indexBuffer_.Get();
        result.vertices_.capacity=result.vertices_.size=static_cast<std::uint32_t>(AllocationBytes(vertexBytes));
        result.indices_.capacity=result.indices_.size=static_cast<std::uint32_t>(AllocationBytes(indexBytes));
        result.vertices_.references=result.indices_.references=result.shape_.references=1;
        result.vertices_.invalidCpuData=result.indices_.invalidCpuData=true;
        return result;
    }
    HRESULT DrawReserve::Prepare(ID3D11DeviceContext* context,SourceView source,std::uint64_t descriptor) {
        if (!context || !vertexBuffer_ || !indexBuffer_ || !source.stride || source.stride!=(descriptor&15)*4 ||
            source.vertices.empty() || source.vertices.size()%source.stride || source.indices.empty() || source.indices.size()%3 ||
            source.vertices.size()>vertices_.capacity || source.indices.size()*2>indices_.capacity) return E_INVALIDARG;
        ComPtr<ID3D11Device> device;
        vertexBuffer_->GetDevice(device.GetAddressOf());
        if (!SameDevice(context,device.Get())) return E_INVALIDARG;
        const auto count=source.vertices.size()/source.stride;
        if (count>UINT16_MAX) return E_INVALIDARG;
        for (auto index:source.indices) if(index>=count) return E_INVALIDARG;
        const auto logicalIndexBytes=source.indices.size()*2;
        const auto paddedIndexBytes=static_cast<UINT>(AllocationBytes(logicalIndexBytes));
        std::memcpy(indexUpload_.data(),source.indices.data(),logicalIndexBytes);
        std::memset(indexUpload_.data()+logicalIndexBytes,0,paddedIndexBytes-logicalIndexBytes);
        const D3D11_BOX vertexBox{0,0,0,static_cast<UINT>(source.vertices.size()),1,1};
        const D3D11_BOX indexBox{0,0,0,paddedIndexBytes,1,1};
        // This is the SAME per-thread context the native draw will consume.
        // A deferred context records the copies before its draw; no foreign
        // immediate-context access or context state change occurs here.
        context->UpdateSubresource(vertexBuffer_.Get(),0,&vertexBox,source.vertices.data(),0,0);
        context->UpdateSubresource(indexBuffer_.Get(),0,&indexBox,indexUpload_.data(),0,0);
        shape_.descriptor=descriptor;
        return S_OK;
    }
    NativeShape* DrawReserve::Shape() {
        // Repair self-references after optional/move construction.
        shape_.vertices=&vertices_;shape_.indices=&indices_;return &shape_;
    }
}
