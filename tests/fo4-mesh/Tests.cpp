#include "Fo4MeshData.h"
#include "Fo4MeshResources.h"
#include "Fo4MeshNative.h"
#include "Fo4MeshPass.h"
#include "GpuBounds.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
    unsigned checks = 0;
    void Check(bool result, const char* message) {
        ++checks;
        if (!result) throw std::runtime_error(message);
    }
    Fo4Mesh::Data Source() {
        // Half positions, extra packed attributes, and deliberately unused
        // vertices inside each prefix: IDs must survive without remapping.
        constexpr std::array<std::array<std::uint16_t, 4>, 6> positions{{
            {0x0000,0x0000,0x0000,0x3c00}, {0x3c00,0x4000,0x4200,0x3c00},
            {0xc000,0xbc00,0x4400,0x3c00}, {0x4200,0x4400,0xbc00,0x3c00},
            {0x4400,0x4500,0x4600,0x3c00}, {0xc400,0xc500,0xc600,0x3c00}}};
        Fo4Mesh::Data source;
        source.stride = 12;
        source.vertices.resize(positions.size() * source.stride);
        for (std::size_t i = 0; i != positions.size(); ++i) {
            std::memcpy(source.vertices.data() + i * source.stride, positions[i].data(), 8);
            const std::uint32_t attribute = 0xabc00000u + static_cast<std::uint32_t>(i);
            std::memcpy(source.vertices.data() + i * source.stride + 8, &attribute, 4);
        }
        source.indices = {0,2,0, 2,3,0, 3,5,4};
        return source;
    }
    void DataTests() {
        Fo4Mesh::NativeBuffer smallVB,smallIB,fullVB,fullIB;
        smallVB.references=smallIB.references=fullVB.references=fullIB.references=1;
        Fo4Mesh::NativeShape owner{0x100000000003,&smallVB,&smallIB,1};
        Fo4Mesh::NativeShape full{owner.descriptor,&fullVB,&fullIB,1};
        const auto assertFull=[&](const char* stage) {
            Check(owner.vertices==&fullVB && owner.indices==&fullIB && owner.descriptor==full.descriptor,stage);
        };
        {
            Fo4Mesh::ScopedNativePair pass(owner,full);
            Check(static_cast<bool>(pass),"full source pair admitted before native setup");
            assertFull("native SetupGeometry observes full source");
            assertFull("native Draw observes same full source");
            assertFull("native RestoreGeometry still observes full source");
        }
        Check(owner.vertices==&smallVB && owner.indices==&smallIB,"resident pair restored only after native restore");
        Check(owner.references==1 && smallVB.references==1 && smallIB.references==1 && fullVB.references==1 && fullIB.references==1,
            "temporary binding transfers no native ownership references");
        try {
            Fo4Mesh::ScopedNativePair pass(owner,full);
            throw std::runtime_error("native C++ unwind fixture");
        } catch(const std::runtime_error&) {}
        Check(owner.vertices==&smallVB && owner.indices==&smallIB,"C++ unwind restores original pair without swallowing exception");
        ++full.descriptor;
        {Fo4Mesh::ScopedNativePair pass(owner,full);Check(!pass && owner.vertices==&smallVB,"descriptor mismatch cannot replace live pair");}
        Fo4Mesh::NativeBuffer wrapper;
        wrapper.buffer=reinterpret_cast<ID3D11Buffer*>(1);wrapper.capacity=20;wrapper.size=20;wrapper.references=1;
        Check(Fo4Mesh::OwningReady(&wrapper,18),"native padded owning wrapper ready");
        wrapper.references=2;Check(!Fo4Mesh::OwningReady(&wrapper,18),"command-owned wrapper rejected");wrapper.references=1;
        wrapper.suballocated=true;Check(!Fo4Mesh::OwningReady(&wrapper,18),"allocator subrange rejected");wrapper.suballocated=false;
        wrapper.offset=4;Check(!Fo4Mesh::OwningReady(&wrapper,18),"nonzero native binding offset rejected");wrapper.offset=0;
        wrapper.pendingCopy=true;Check(!Fo4Mesh::OwningReady(&wrapper,18),"initial upload must finish");wrapper.pendingCopy=false;
        wrapper.pendingRequests=1;Check(!Fo4Mesh::OwningReady(&wrapper,18),"creator queue must finish");wrapper.pendingRequests=0;
        wrapper.shaderAcquires=1;Check(!Fo4Mesh::OwningReady(&wrapper,18),"active acquired shader resource rejected");wrapper.shaderAcquires=0;
        Check(!Fo4Mesh::OwningReady(&wrapper,16),"partial capacity ownership rejected");
        wrapper.suballocated=true;wrapper.capacity=32;wrapper.offset=512;
        Check(Fo4Mesh::ExclusiveWindowReady(&wrapper,18),"exclusive padded native pool block is a valid source window");
        wrapper.offset=3;Check(!Fo4Mesh::ExclusiveWindowReady(&wrapper,18),"unaligned pool window rejected");wrapper.offset=512;
        wrapper.references=2;Check(!Fo4Mesh::ExclusiveWindowReady(&wrapper,18),"cached/shared pool wrapper rejected");wrapper.references=1;
        wrapper.capacity=16;Check(!Fo4Mesh::ExclusiveWindowReady(&wrapper,18),"native block cannot be smaller than payload");wrapper.capacity=32;
        wrapper.offset=(128u<<20)-16;Check(!Fo4Mesh::ExclusiveWindowReady(&wrapper,18),"pool window overflow rejected");
        auto source = Source();
        const auto levels = Fo4Mesh::BuildPrefixLevels(source.View(), {1,1,1});
        Check(levels && levels->count == 3, "three distinct prefixes");
        Check(levels->levels[0].bytes == 92 && levels->levels[1].bytes == 60 && levels->levels[2].bytes == 44, "exact byte tables");
        const auto compactedData = Fo4Mesh::CompactPreservingVertexIDs(source.View(), levels->levels[2].range);
        Check(compactedData && compactedData->indices == std::vector<std::uint16_t>({0,2,0}), "indices preserved including hole");
        Check(compactedData->vertices == std::vector<std::byte>(source.vertices.begin(), source.vertices.begin()+36), "all prefix attributes preserved");
        Check(source.vertices.size() == 72 && source.indices.size() == 9, "restoration source unchanged");
        const auto duplicate = Fo4Mesh::BuildPrefixLevels(source.View(), {1,0,2});
        Check(duplicate && duplicate->count == 2, "duplicate native prefix omitted");
        Check(Fo4Mesh::BuildPrefixLevels(source.View(), {0,0,3})->count == 1, "empty native prefixes pinned");
        Check(!Fo4Mesh::BuildPrefixLevels(source.View(), {1,1,2}), "triangle count mismatch rejected");
        Check(!Fo4Mesh::CompactPreservingVertexIDs(source.View(), {1,3}), "unaligned range rejected");
        Check(!Fo4Mesh::CompactPreservingVertexIDs(source.View(), {6,6}), "overrun rejected");
        const auto segment = Fo4Mesh::NativeLODRange({1,1,1},0x82,3);
        Check(segment && segment->first == 6 && segment->count == 3, "native disjoint semantics parsed");
        Check(!Fo4Mesh::NativeLODRange({1,1,1},0x83,3), "invalid high-bit mode rejected");
        // Native FO4 triangle count is u32, even though vertex IDs are u16.
        source.indices.assign(210000, 2);
        Check(Fo4Mesh::BuildPrefixLevels(source.View(), {10000,20000,40000})->count == 3, "more than 65535 triangles supported");
        source.indices.back() = 6;
        Check(!Fo4Mesh::BuildPrefixLevels(source.View(), {10000,20000,40000}), "invalid source index rejected");
        source = Source();
        const auto bounds = GpuBounds::Decode(source.vertices, source.stride, (1ull<<44)|3);
        Check(bounds && bounds->center == std::array<float,3>{0,0,0} && bounds->halfExtent == std::array<float,3>{4,5,6}, "half full-source bounds");
        constexpr float matrix[12]{0,1,0,0, -1,0,0,0, 0,0,1,0};
        constexpr float translation[3]{10,20,30};
        const auto world = GpuBounds::Transform(*bounds,matrix,translation,-2);
        Check(world && world->center == std::array<float,3>{10,20,30} && world->halfExtent == std::array<float,3>{10,8,12}, "native column transform with negative scale");
        const auto smallBounds = GpuBounds::Decode(compactedData->vertices,compactedData->stride,(1ull<<44)|3);
        Check(smallBounds && smallBounds->halfExtent != bounds->halfExtent, "coarse bounds differ; registry must retain full bounds");
    }
    struct Device {
        Fo4Mesh::ComPtr<ID3D11Device> device;
        Fo4Mesh::ComPtr<ID3D11DeviceContext> context;
        Device() {
            Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                device.GetAddressOf(),nullptr,context.GetAddressOf())), "WARP device");
        }
        Fo4Mesh::ComPtr<ID3D11Buffer> Buffer(const void* data, UINT bytes, UINT bind) {
            const D3D11_BUFFER_DESC description{bytes,D3D11_USAGE_IMMUTABLE,bind,0,0,0};
            const D3D11_SUBRESOURCE_DATA contents{data,0,0};
            Fo4Mesh::ComPtr<ID3D11Buffer> result;
            Check(SUCCEEDED(device->CreateBuffer(&description,&contents,result.GetAddressOf())), "buffer creation");
            return result;
        }
        Fo4Mesh::ComPtr<ID3D11Buffer> RawBuffer(const void* data, UINT bytes, UINT bind) {
            std::vector<std::byte> padded(static_cast<std::size_t>(Fo4Mesh::AllocationBytes(bytes)),std::byte{0xff});
            std::memcpy(padded.data(),data,bytes);
            const D3D11_BUFFER_DESC description{static_cast<UINT>(padded.size()),D3D11_USAGE_DEFAULT,
                bind|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
            const D3D11_SUBRESOURCE_DATA contents{padded.data(),0,0};
            Fo4Mesh::ComPtr<ID3D11Buffer> result;
            Check(SUCCEEDED(device->CreateBuffer(&description,&contents,result.GetAddressOf())), "native raw buffer creation");
            return result;
        }
    };
    HRESULT Await(Fo4Mesh::Readback& capture, Device& device, Fo4Mesh::Data& output) {
        device.context->Flush();
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for (;;) {
            const auto result = capture.Poll(device.context.Get(),output);
            if (result != S_FALSE && result != DXGI_ERROR_WAS_STILL_DRAWING) return result;
            if (std::chrono::steady_clock::now() >= deadline) return E_ABORT;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void GpuTests() {
        Device device;
        const auto source = Source();
        auto vb = device.Buffer(source.vertices.data(),static_cast<UINT>(source.vertices.size()),D3D11_BIND_VERTEX_BUFFER);
        auto ib = device.Buffer(source.indices.data(),static_cast<UINT>(source.indices.size()*2),D3D11_BIND_INDEX_BUFFER);
        auto capture = Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,6,9);
        Check(capture.has_value(), "actual pair readback starts");
        Check(capture->Bytes()==source.Bytes() && capture->SourceVertices()==vb.Get() && capture->SourceIndices()==ib.Get(), "bounded readback identity");
        Fo4Mesh::Data output;
        Check(Await(*capture,device,output)==S_OK, "copy completion");
        Check(output.vertices==source.vertices && output.indices==source.indices && output.stride==source.stride, "actual GPU contents verified");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,6,9,source.Bytes()-1), "staging byte cap");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),ib.Get(),vb.Get(),source.stride,6,9), "buffer role mismatch");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),7,6,9), "stride mismatch");
        Device other;
        Check(capture->Poll(other.context.Get(),output)==E_INVALIDARG, "cross-device poll rejected");
        Check(!Fo4Mesh::Readback::Begin(other.device.Get(),other.context.Get(),vb.Get(),ib.Get(),source.stride,6,9), "cross-device source rejected");
        Fo4Mesh::ComPtr<ID3D11DeviceContext> deferred;
        Check(SUCCEEDED(device.device->CreateDeferredContext(0,deferred.GetAddressOf())), "deferred context creation");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),deferred.Get(),vb.Get(),ib.Get(),source.stride,6,9), "deferred readback rejected");
        Check(capture->Poll(deferred.Get(),output)==E_INVALIDARG, "deferred poll rejected");
        auto malformed=source.indices; malformed.back()=6;
        ib=device.Buffer(malformed.data(),static_cast<UINT>(malformed.size()*2),D3D11_BIND_INDEX_BUFFER);
        auto invalid=Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,6,9);
        Check(invalid && Await(*invalid,device,output)==E_INVALIDARG, "GPU out-of-bounds index rejected");
        Check(output.indices==source.indices, "failed verification preserves caller output");
        vb=device.RawBuffer(source.vertices.data(),static_cast<UINT>(source.vertices.size()),D3D11_BIND_VERTEX_BUFFER);
        ib=device.RawBuffer(source.indices.data(),static_cast<UINT>(source.indices.size()*2),D3D11_BIND_INDEX_BUFFER);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format=DXGI_FORMAT_R32_TYPELESS;srvDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        srvDesc.BufferEx.NumElements=5;srvDesc.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        Fo4Mesh::ComPtr<ID3D11ShaderResourceView> srv;
        Check(SUCCEEDED(device.device->CreateShaderResourceView(ib.Get(),&srvDesc,srv.GetAddressOf())), "native raw view alive during readback");
        auto native=Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,6,9);
        Check(native && native->Bytes()==92 && Await(*native,device,output)==S_OK, "native rounded allocation readback");
        Check(output.indices==source.indices && output.vertices==source.vertices, "padding excluded from source and index validation");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,6,9,91), "cap includes native padding");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),vb.Get(),ib.Get(),source.stride,5,9), "reject larger pooled source allocation");
        std::vector<std::byte> poolBytes(4096,std::byte{0xcc});
        std::memcpy(poolBytes.data()+256,source.vertices.data(),source.vertices.size());
        std::memcpy(poolBytes.data()+512,source.indices.data(),source.indices.size()*2);
        auto pool=device.RawBuffer(poolBytes.data(),static_cast<UINT>(poolBytes.size()),D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER);
        Fo4Mesh::BufferRegion vertexRegion{pool.Get(),256,80,true},indexRegion{pool.Get(),512,32,true};
        auto pooled=Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,indexRegion,12,6,9,92);
        Check(pooled && pooled->Bytes()==92,"pool readback allocates only bounded subranges, not entire shared heap");
        Check(pooled->SourceVertexOffset()==256 && pooled->SourceIndexOffset()==512,"pool offsets retained for later live-identity validation");
        Check(Await(*pooled,device,output)==S_OK && output.vertices==source.vertices && output.indices==source.indices,"same-resource disjoint vertex/index subranges read exactly");
        Check(!Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,indexRegion,12,6,9,91),"pooled staging cap includes allocation padding");
        auto invalidRegion=indexRegion;invalidRegion.offset=280;
        Check(!Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,invalidRegion,12,6,9),"overlapping native allocations rejected");
        invalidRegion=indexRegion;invalidRegion.offset=4090;
        Check(!Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,invalidRegion,12,6,9),"out-of-resource allocation rejected without overflow");
        invalidRegion=indexRegion;invalidRegion.offset=513;
        Check(!Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,invalidRegion,12,6,9),"raw allocation offsets must be four-byte aligned");
        invalidRegion=indexRegion;invalidRegion.capacity=16;
        Check(!Fo4Mesh::Readback::BeginRegions(device.device.Get(),device.context.Get(),vertexRegion,invalidRegion,12,6,9),"allocation extent must contain logical source plus native padding");
        Check(!Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),pool.Get(),pool.Get(),12,6,9),"whole-resource API cannot silently admit pooled input");
        auto reserve=Fo4Mesh::DrawReserve::Create(device.device.Get(),72,18);
        Check(reserve && reserve->Bytes()==92,"bounded emergency reserve");
        Check(reserve->Prepare(device.context.Get(),source.View(),(1ull<<44)|3)==S_OK,"reserve upload on actual immediate context");
        auto* shape=reserve->Shape();
        Check(Fo4Mesh::OwningReady(shape->vertices,72) && Fo4Mesh::OwningReady(shape->indices,18),"draw-only wrapper ready without native queue work");
        auto reserveCopy=Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),shape->vertices->buffer,shape->indices->buffer,12,6,9);
        Check(reserveCopy && Await(*reserveCopy,device,output)==S_OK && output.vertices==source.vertices && output.indices==source.indices,"reserve exact full-source restoration bytes");
        Check(reserve->Prepare(other.context.Get(),source.View(),(1ull<<44)|3)==E_INVALIDARG,"reserve rejects foreign context");
        auto changed=source;changed.vertices[8]=std::byte{0x55};
        Check(reserve->Prepare(deferred.Get(),changed.View(),(1ull<<44)|3)==S_OK,"same deferred draw-context upload supported");
        Fo4Mesh::ComPtr<ID3D11CommandList> commands;
        Check(SUCCEEDED(deferred->FinishCommandList(FALSE,commands.GetAddressOf())),"finish deferred reserve upload");
        device.context->ExecuteCommandList(commands.Get(),TRUE);
        reserveCopy=Fo4Mesh::Readback::Begin(device.device.Get(),device.context.Get(),shape->vertices->buffer,shape->indices->buffer,12,6,9);
        Check(reserveCopy && Await(*reserveCopy,device,output)==S_OK && output.vertices==changed.vertices,"deferred reserve copy executes before subsequent work");
    }
}
int main() {
    try { DataTests(); GpuTests(); std::cout<<checks<<" checks passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<"check "<<checks<<": "<<e.what()<<'\n'; return 1; }
}
