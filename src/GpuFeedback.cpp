#include "GpuFeedback.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace GpuFeedback
{
    using Microsoft::WRL::ComPtr;

    // Arithmetic recovered from local Starfield 1.16.236 GPUCulling and
    // ScreenSizeLOD DXIL. This shader does NOT implement Starfield's HiZ or
    // frustum visibility; eligible flags are an explicit gather input.
    constexpr char kShader[] = R"hlsl(
struct Geometry {
    float3 center; float meshRatio;
    float3 extents; uint materialSlot;
    float3 camera; float viewScale;
    uint meshSlot; uint flags; uint2 reserved;
};
struct Binding { uint materialSlot; uint textureSlot; uint halfUV; uint applyUV; };
StructuredBuffer<Geometry> geometry : register(t0);
StructuredBuffer<Binding> bindings : register(t1);
RWByteAddressBuffer materialDemand : register(u0);
RWByteAddressBuffer outputDemand : register(u1);
cbuffer Counts : register(b0) {
    uint recordCount; uint bindingCount; uint materialCount; uint textureCount;
    uint meshCount; uint invalidOffset; uint2 unused;
};
uint Encode(float value) { return (uint)(clamp(value, 0.0f, 4.0f) * 1073741760.0f); }
[numthreads(128,1,1)]
void Produce(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= recordCount) return;
    Geometry g = geometry[tid.x];
    float demand = 2.0f * g.viewScale * max(g.extents.x, max(g.extents.y,g.extents.z))
        / max(length(g.center-g.camera), 1.0f);
    uint ignored;
    if ((g.flags & 1) && g.materialSlot < materialCount)
        materialDemand.InterlockedMax(4*g.materialSlot, Encode(demand), ignored);
    if ((g.flags & 2) && g.meshSlot < meshCount && demand != 0.0f) {
        float meshDemand = (g.flags & 4) ? 4.0f : demand*g.meshRatio;
        outputDemand.InterlockedMax(4*(textureCount+g.meshSlot), Encode(meshDemand), ignored);
    }
}
[numthreads(128,1,1)]
void Reduce(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= bindingCount) return;
    Binding b = bindings[tid.x];
    uint encoded = materialDemand.Load(4*b.materialSlot);
    if (encoded == 0) return;
    if (b.applyUV) {
        float uv = max(0.00001f, min(f16tof32(b.halfUV & 65535), f16tof32(b.halfUV >> 16)));
        float scaled = (float)encoded / uv;
        // DXIL's fptoui is undefined outside uint range. Surface a failed
        // result instead of inventing saturation or accepting a stale demand.
        if (!isfinite(scaled) || scaled < 0.0f || scaled >= 4294967296.0f) {
            uint ignored; outputDemand.InterlockedAdd(invalidOffset, 1, ignored); return;
        }
        encoded = (uint)scaled;
    }
    uint ignored; outputDemand.InterlockedMax(4*b.textureSlot, encoded, ignored);
}
)hlsl";

    namespace
    {
        // Only the two CS SRV/UAV slots, CB0, shader and class instances change.
        // Get calls AddRef; ComPtr restoration releases every acquired reference.
        struct ComputeState
        {
            ID3D11DeviceContext* context;
            ComPtr<ID3D11ComputeShader> shader;
            std::array<ID3D11ClassInstance*, 256> classes{};
            UINT classCount{ 256 };
            std::array<ComPtr<ID3D11ShaderResourceView>, 2> srvs;
            std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
            ComPtr<ID3D11Buffer> constant;
            ComPtr<ID3D11Predicate> predicate;
            BOOL predicateValue{};
            explicit ComputeState(ID3D11DeviceContext* c) : context(c)
            {
                c->CSGetShader(shader.GetAddressOf(), classes.data(), &classCount);
                ID3D11ShaderResourceView* s[2]{};
                ID3D11UnorderedAccessView* u[2]{};
                c->CSGetShaderResources(0, 2, s);
                c->CSGetUnorderedAccessViews(0, 2, u);
                for (unsigned i=0; i<2; ++i) { srvs[i].Attach(s[i]); uavs[i].Attach(u[i]); }
                c->CSGetConstantBuffers(0, 1, constant.GetAddressOf());
                c->GetPredication(predicate.GetAddressOf(), &predicateValue);
                c->SetPredication(nullptr, FALSE);
            }
            ~ComputeState()
            {
                ID3D11ShaderResourceView* s[]{ srvs[0].Get(), srvs[1].Get() };
                ID3D11UnorderedAccessView* u[]{ uavs[0].Get(), uavs[1].Get() };
                ID3D11Buffer* b = constant.Get();
                UINT keep[]{ UINT(-1), UINT(-1) };
                // Unbind our UAVs first; restoration may rebind those slots as
                // SRVs. Counters belonging to game UAVs are preserved with -1.
                ID3D11UnorderedAccessView* none[2]{};
                context->CSSetUnorderedAccessViews(0, 2, none, keep);
                context->CSSetShaderResources(0, 2, s);
                context->CSSetUnorderedAccessViews(0, 2, u, keep);
                context->CSSetConstantBuffers(0, 1, &b);
                context->CSSetShader(shader.Get(), classes.data(), classCount);
                context->SetPredication(predicate.Get(), predicateValue);
                for (UINT i=0; i<classCount; ++i) if (classes[i]) classes[i]->Release();
            }
        };
    }

    struct Backend::Impl
    {
        struct Slot
        {
            ComPtr<ID3D11Buffer> staging;
            ComPtr<ID3D11Query> event;
            std::vector<Identity> textures, meshes;
            std::uint64_t frame{};
            bool pending{}, expired{};
        };
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11ComputeShader> produce, reduce;
        ComPtr<ID3D11Buffer> records, bindings, materials, output, constants;
        ComPtr<ID3D11ShaderResourceView> recordView, bindingView;
        ComPtr<ID3D11UnorderedAccessView> materialView, outputView;
        std::array<Slot, 3> slots;
        Options options;
        Stats stats;
        std::string error;
        DWORD thread{};
        bool ready{};
        std::uint64_t lastSubmit{}, lastPoll{};

        bool Fail(const char* what, HRESULT hr = E_FAIL)
        {
            error = what;
            error += " HRESULT=" + std::to_string(static_cast<std::uint32_t>(hr));
            ++stats.failures;
            return false;
        }
        bool Owner() { return thread == GetCurrentThreadId() || Fail("Wrong immediate-context owner thread"); }

        bool Buffer(std::uint32_t bytes, UINT bind, UINT misc, UINT stride, D3D11_USAGE usage,
            ComPtr<ID3D11Buffer>& destination)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = bytes; desc.BindFlags = bind; desc.MiscFlags = misc;
            desc.StructureByteStride = stride; desc.Usage = usage;
            desc.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
            const auto hr = device->CreateBuffer(&desc, nullptr, destination.GetAddressOf());
            if (FAILED(hr)) return Fail("CreateBuffer", hr);
            stats.allocatedBytes += bytes;
            return true;
        }
        bool Structured(std::uint32_t count, UINT stride, ComPtr<ID3D11Buffer>& buffer,
            ComPtr<ID3D11ShaderResourceView>& view)
        {
            if (!Buffer(count*stride, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                    stride, D3D11_USAGE_DEFAULT, buffer)) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = count;
            const auto hr = device->CreateShaderResourceView(buffer.Get(), &desc, view.GetAddressOf());
            return SUCCEEDED(hr) || Fail("Create structured SRV", hr);
        }
        bool Raw(std::uint32_t words, ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& view)
        {
            if (!Buffer(words*4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                    0, D3D11_USAGE_DEFAULT, buffer)) return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.Buffer.NumElements = words; desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            const auto hr = device->CreateUnorderedAccessView(buffer.Get(), &desc, view.GetAddressOf());
            return SUCCEEDED(hr) || Fail("Create raw UAV", hr);
        }
        bool Shader(const char* entry, ComPtr<ID3D11ComputeShader>& shader)
        {
            ComPtr<ID3DBlob> bytecode, messages;
            auto hr = D3DCompile(kShader, sizeof(kShader)-1, "StarfieldDemand", nullptr, nullptr,
                entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS,
                0, bytecode.GetAddressOf(), messages.GetAddressOf());
            if (FAILED(hr)) {
                Fail("Compile recovered demand shader", hr);
                if (messages) error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
                return false;
            }
            hr = device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.GetAddressOf());
            return SUCCEEDED(hr) || Fail("CreateComputeShader", hr);
        }
        bool Valid(const Batch& b)
        {
            if (b.records.empty() || b.records.size() > options.maxRecords || b.bindings.size() > options.maxBindings ||
                b.textures.size() > options.maxTextures || b.meshes.size() > options.maxMeshes ||
                b.materialCount > options.maxMaterials || (!b.textures.size() && !b.meshes.size())) return false;
            for (const auto& id : b.textures) if (!id.key || !id.generation) return false;
            for (const auto& id : b.meshes) if (!id.key || !id.generation) return false;
            for (const auto& r : b.records) {
                if ((r.flags & ~7u) || r.reserved[0] || r.reserved[1] ||
                    ((r.flags & MaterialVisible) && r.materialSlot >= b.materialCount) ||
                    ((r.flags & MeshEligible) && r.meshSlot >= b.meshes.size())) return false;
                if (!std::isfinite(r.meshRatio) || r.meshRatio < 0 || !std::isfinite(r.viewScale) || r.viewScale < 0) return false;
                for (unsigned i=0; i<3; ++i)
                    if (!std::isfinite(r.center[i]) || !std::isfinite(r.camera[i]) ||
                        !std::isfinite(r.halfExtents[i]) || r.halfExtents[i] < 0 ||
                        std::abs(r.center[i]) > 1e12f || std::abs(r.camera[i]) > 1e12f ||
                        r.halfExtents[i] > 1e12f) return false;
                if (r.viewScale > 1e6f || r.meshRatio > 1e6f) return false;
            }
            for (const auto& v : b.bindings) {
                if (v.materialSlot >= b.materialCount || v.textureSlot >= b.textures.size() || v.applyUV > 1) return false;
                if (v.applyUV && (((v.halfUV & 0x7c00u) == 0x7c00u) || ((v.halfUV & 0x7c000000u) == 0x7c000000u))) return false;
            }
            return true;
        }
    };

    Backend::Backend() : impl_(std::make_unique<Impl>()) {}
    Backend::~Backend() = default;
    const Stats& Backend::Statistics() const noexcept { return impl_->stats; }
    const std::string& Backend::LastError() const noexcept { return impl_->error; }

    bool Backend::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, const Options& options)
    {
        auto& p = *impl_;
        if (p.thread) return p.Fail("Backend initialization is one-shot");
        p.thread = GetCurrentThreadId(); p.options = options;
        const std::uint64_t bytes = std::uint64_t(options.maxRecords)*64 + std::uint64_t(options.maxBindings)*16 +
            std::uint64_t(options.maxMaterials)*4 + (std::uint64_t(options.maxTextures)+options.maxMeshes+1)*16 + 32;
        if (!device || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 || bytes > 64*1024*1024 ||
            !options.maxRecords || !options.maxBindings || !options.maxMaterials ||
            (!options.maxTextures && !options.maxMeshes) || !options.maxReadbackAge || options.maxReadbackAge > 120 ||
            options.maxRecords > 128*65535 || options.maxBindings > 128*65535)
            return p.Fail("Invalid GPU feedback capacities/context/feature level");
        ComPtr<ID3D11Device> owner;
        context->GetDevice(owner.GetAddressOf());
        if (owner.Get() != device) return p.Fail("Device/context mismatch");
        p.device = device; p.context = context;
        const auto words = options.maxTextures + options.maxMeshes + 1;
        if (!p.Shader("Produce", p.produce) || !p.Shader("Reduce", p.reduce) ||
            !p.Structured(options.maxRecords, sizeof(Record), p.records, p.recordView) ||
            !p.Structured(options.maxBindings, sizeof(Binding), p.bindings, p.bindingView) ||
            !p.Raw(options.maxMaterials, p.materials, p.materialView) || !p.Raw(words, p.output, p.outputView) ||
            !p.Buffer(32, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, p.constants)) return false;
        for (auto& slot : p.slots) {
            if (!p.Buffer(words*4, 0, 0, 0, D3D11_USAGE_STAGING, slot.staging)) return false;
            D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
            const auto hr = device->CreateQuery(&desc, slot.event.GetAddressOf());
            if (FAILED(hr)) return p.Fail("Create readback event", hr);
        }
        p.ready = true;
        return true;
    }

    bool Backend::Submit(const Batch& batch, std::uint64_t frame)
    {
        auto& p = *impl_;
        if (!p.ready || !p.Owner()) return false;
        if (!frame || frame <= p.lastSubmit || !p.Valid(batch)) { ++p.stats.rejected; return false; }
        auto free = std::find_if(p.slots.begin(), p.slots.end(), [](const auto& s) { return !s.pending; });
        if (free == p.slots.end()) { ++p.stats.ringFull; return false; }
        // Copy identity metadata before touching context state. The consumer
        // must reject retired/reused generations before merging into its plan.
        free->textures.assign(batch.textures.begin(), batch.textures.end());
        free->meshes.assign(batch.meshes.begin(), batch.meshes.end());
        const std::uint32_t counts[]{ static_cast<std::uint32_t>(batch.records.size()),
            static_cast<std::uint32_t>(batch.bindings.size()), batch.materialCount,
            static_cast<std::uint32_t>(batch.textures.size()), static_cast<std::uint32_t>(batch.meshes.size()),
            static_cast<std::uint32_t>((batch.textures.size()+batch.meshes.size())*4), 0, 0 };
        auto* c = p.context.Get();
        ComputeState restore(c);
        const D3D11_BOX recordBox{ 0, 0, 0, static_cast<UINT>(batch.records.size_bytes()), 1, 1 };
        c->UpdateSubresource(p.records.Get(), 0, &recordBox, batch.records.data(), 0, 0);
        if (!batch.bindings.empty()) {
            const D3D11_BOX bindingBox{ 0, 0, 0, static_cast<UINT>(batch.bindings.size_bytes()), 1, 1 };
            c->UpdateSubresource(p.bindings.Get(), 0, &bindingBox, batch.bindings.data(), 0, 0);
        }
        c->UpdateSubresource(p.constants.Get(), 0, nullptr, counts, 0, 0);
        constexpr UINT zero[4]{};
        c->ClearUnorderedAccessViewUint(p.materialView.Get(), zero);
        c->ClearUnorderedAccessViewUint(p.outputView.Get(), zero);
        ID3D11ShaderResourceView* srvs[]{ p.recordView.Get(), p.bindingView.Get() };
        ID3D11UnorderedAccessView* uavs[]{ p.materialView.Get(), p.outputView.Get() };
        ID3D11Buffer* cb = p.constants.Get();
        UINT preserve[]{ UINT(-1), UINT(-1) };
        c->CSSetShaderResources(0, 2, srvs);
        c->CSSetUnorderedAccessViews(0, 2, uavs, preserve);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetShader(p.produce.Get(), nullptr, 0);
        c->Dispatch((counts[0]+127)/128, 1, 1);
        if (counts[1]) { c->CSSetShader(p.reduce.Get(), nullptr, 0); c->Dispatch((counts[1]+127)/128, 1, 1); }
        ID3D11UnorderedAccessView* none[2]{};
        c->CSSetUnorderedAccessViews(0, 2, none, preserve);
        c->CopyResource(free->staging.Get(), p.output.Get());
        c->End(free->event.Get());
        free->frame = frame; free->pending = true; free->expired = false;
        p.lastSubmit = frame; ++p.stats.submitted;
        return true;
    }

    std::vector<Result> Backend::Poll(std::uint64_t frame)
    {
        auto& p = *impl_;
        std::vector<Result> results;
        if (!p.ready || !p.Owner()) return results;
        if (frame < p.lastPoll) { p.Fail("Nonmonotonic feedback frame"); return results; }
        p.lastPoll = frame;
        for (auto& slot : p.slots) {
            if (!slot.pending || frame < slot.frame) continue;
            const auto age = frame-slot.frame;
            if (age > p.options.maxReadbackAge && !slot.expired) { slot.expired = true; ++p.stats.expired; }
            BOOL done{};
            const auto hr = p.context->GetData(slot.event.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE || (hr == S_OK && !done)) continue;
            if (FAILED(hr)) { p.Fail("Readback event", hr); p.ready = false; break; }
            if (slot.expired) { slot.pending = false; slot.textures.clear(); slot.meshes.clear(); continue; }
            D3D11_MAPPED_SUBRESOURCE map{};
            const auto mapped = p.context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (mapped == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (FAILED(mapped)) { p.Fail("Readback map", mapped); p.ready = false; break; }
            Result result;
            result.sourceFrame = slot.frame; result.completedFrame = frame;
            const auto* values = static_cast<const std::uint32_t*>(map.pData);
            result.invalidConversions = values[slot.textures.size()+slot.meshes.size()];
            result.valid = result.invalidConversions == 0;
            // Reserve before Map would avoid allocation exceptions with a
            // mapped resource; the catch guarantees Unmap on every path.
            try {
                for (std::size_t i=0; i<slot.textures.size(); ++i)
                    result.textures.push_back({ slot.textures[i], values[i], static_cast<float>(values[i])*0x1p-30f });
                for (std::size_t i=0; i<slot.meshes.size(); ++i) {
                    const auto value = values[slot.textures.size()+i];
                    result.meshes.push_back({ slot.meshes[i], value, static_cast<float>(value)*0x1p-30f });
                }
            } catch (...) { p.context->Unmap(slot.staging.Get(), 0); throw; }
            p.context->Unmap(slot.staging.Get(), 0);
            slot.pending = false;
            slot.textures.clear(); slot.meshes.clear();
            ++p.stats.completed; p.stats.maxCompletedAge = std::max(p.stats.maxCompletedAge, age);
            results.push_back(std::move(result));
        }
        std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.sourceFrame < b.sourceFrame; });
        return results;
    }
}
