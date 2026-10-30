#include "TextureFeedback.h"
#include "Addresses.h"
#include "GpuGather.h"
#include "GpuFeedback.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

namespace
{
    constexpr std::size_t kMaxRecords = 32768, kMaxBindings = 65536, kMaxTextures = 16384;
    TextureFeedback::Resolve g_resolve{};
    TextureFeedback::Deliver g_deliver{};
    std::atomic<bool> g_enabled{};
    std::atomic<std::uint64_t> g_dropped{};
    std::uintptr_t g_deviceSlot{};
    // Hooks and renderer resources live for the process. Avoid running the
    // backend destructor later from the loader/CRT thread during shutdown.
    GpuFeedback::Backend* g_backend{};
    std::mutex g_batchLock, g_statsLock;
    TextureFeedback::Stats g_stats;

    struct Storage
    {
        std::vector<GpuFeedback::Record> records;
        std::vector<GpuFeedback::Binding> bindings;
        std::vector<GpuFeedback::Identity> textures;
        std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint32_t>> indices;
        std::uint32_t materials{};
        void Reserve()
        {
            records.reserve(kMaxRecords);
            bindings.reserve(kMaxBindings);
            textures.reserve(kMaxTextures);
            indices.reserve(kMaxTextures);
        }
        void Clear() { records.clear(); bindings.clear(); textures.clear(); indices.clear(); materials = 0; }
    } g_collect, g_submit;

    struct Visitor
    {
        const std::uintptr_t* table{};
        std::array<GpuFeedback::Identity, 32> textures{};
        std::size_t count{};
        bool overflow{};
    };

    void IgnoreDestructor(Visitor*) {}
    std::int32_t Visit(Visitor* visitor, RE::NiTexture* owner)
    {
        if (!owner) return 1;
        const auto* texture = owner->rendererTexture;
        if (!texture) return 1;
        std::uint64_t generation{};
        if (!g_resolve(texture, generation)) return 1;
        const GpuFeedback::Identity identity{ reinterpret_cast<std::uint64_t>(texture), generation };
        const auto end = visitor->textures.begin() + visitor->count;
        if (std::find(visitor->textures.begin(), end, identity) != end) return 1;
        if (visitor->count == visitor->textures.size()) { visitor->overflow = true; return 0; }
        visitor->textures[visitor->count++] = identity;
        return 1;
    }

    void Gather(const GpuGather::Draw& draw)
    {
        if (!g_enabled.load(std::memory_order_relaxed) || !draw.shaderProperty || !draw.eyeCount) return;
        if (draw.eyeCount > draw.eyes.size()) { ++g_dropped; return; }
        std::array<float, 2> viewScales{};
        for (std::uint32_t eye = 0; eye < draw.eyeCount; ++eye) {
            const auto& frustum = draw.eyes[eye].frustum;
            const auto scale = GpuBounds::PerspectiveViewScale(frustum[0], frustum[1], frustum[2], frustum[3]);
            if (draw.eyes[eye].orthographic || !scale) { ++g_dropped; return; }
            viewScales[eye] = *scale;
        }
        // Verified ForEachTexture ABI: Visit is vslot1, continue iff >=1.
        static const std::uintptr_t table[]{ reinterpret_cast<std::uintptr_t>(&IgnoreDestructor),
                                             reinterpret_cast<std::uintptr_t>(&Visit) };
        Visitor visitor;
        visitor.table = table;
        const auto* propertyTable = *static_cast<const std::uintptr_t* const*>(draw.shaderProperty);
        using Enumerate = std::int32_t (*)(void*, Visitor*);
        reinterpret_cast<Enumerate>(propertyTable[Addr::ForEachTextureSlot()])(draw.shaderProperty, &visitor);
        if (!visitor.count) return;
        if (visitor.overflow) { ++g_dropped; return; }

        float u = 1, v = 1;
        if (draw.material) {
            if (REL::Module::IsVR()) {
                const auto* uv = reinterpret_cast<const RE::NiPoint2*>(static_cast<const std::byte*>(draw.material) + 0x14);
                u = uv->x; v = uv->y;
            } else {
                const auto* material = static_cast<const RE::BSShaderMaterial*>(draw.material);
                u = std::min(material->texCoordScale[0].x, material->texCoordScale[1].x);
                v = std::min(material->texCoordScale[0].y, material->texCoordScale[1].y);
            }
        }
        if (!std::isfinite(u) || !std::isfinite(v)) { ++g_dropped; return; }
        const auto packed = static_cast<std::uint32_t>(DirectX::PackedVector::XMConvertFloatToHalf(u)) |
            (static_cast<std::uint32_t>(DirectX::PackedVector::XMConvertFloatToHalf(v)) << 16);
        std::scoped_lock lock(g_batchLock);
        if (g_collect.records.size() + draw.eyeCount > kMaxRecords ||
            g_collect.bindings.size() + visitor.count * 2 > kMaxBindings ||
            g_collect.textures.size() + visitor.count > kMaxTextures || g_collect.materials == 16384) {
            ++g_dropped;
            return;
        }
        try {
            const auto material = g_collect.materials++;
            for (std::size_t i = 0; i < visitor.count; ++i) {
                const auto identity = visitor.textures[i];
                auto& index = g_collect.indices[identity.key];
                if (index.first != identity.generation || index.second >= g_collect.textures.size() ||
                    g_collect.textures[index.second] != identity) {
                    index = { identity.generation, static_cast<std::uint32_t>(g_collect.textures.size()) };
                    g_collect.textures.push_back(identity);
                }
                // Until semantic slots are classified, combine direct and UV
                // demand. Lookup/projected textures cannot lose detail here.
                g_collect.bindings.push_back({ material, index.second, packed, 0 });
                g_collect.bindings.push_back({ material, index.second, packed, 1 });
            }
            for (std::uint32_t eye = 0; eye < draw.eyeCount; ++eye) {
                GpuFeedback::Record record;
                record.center = draw.worldBounds.center;
                record.halfExtents = draw.worldBounds.halfExtent;
                record.camera = draw.eyes[eye].position;
                record.materialSlot = material;
                record.flags = GpuFeedback::MaterialVisible;
                record.viewScale = viewScales[eye];
                g_collect.records.push_back(record);
            }
        } catch (...) {
            g_collect.Clear(); // never submit partially constructed identities
            ++g_dropped;
        }
    }
}

namespace TextureFeedback
{
    bool Install(bool enabled, Resolve resolve, Deliver deliver)
    {
        if (!enabled) return false;
        if (!resolve || !deliver) return false;
        g_deviceSlot = Addr::Resolve(Addr::D3DDevice);
        if (!g_deviceSlot) return false;
        try { g_collect.Reserve(); g_submit.Reserve(); }
        catch (...) { return false; }
        g_resolve = resolve;
        g_deliver = deliver;
        g_enabled.store(true, std::memory_order_release);
        logger::info("Experimental GPU texture feedback installed: true static AABBs, verified main-view draws, "
            "actual eye positions and per-eye projection scale; additive demand, conservative slot UV, no HiZ claim");
        return true;
    }

    void Observe(const GpuGather::Draw& draw) { Gather(draw); }

    void OnFrame(std::uint64_t frame) noexcept
    {
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        try {
            {
                std::scoped_lock lock(g_batchLock);
                std::swap(g_collect, g_submit);
            }
            // Menu/video presentation can use a different thread from actual
            // world rendering. Establish context ownership only after the
            // verified draw path has supplied its first nonempty batch.
            if (!g_backend && g_submit.records.empty()) return;
            if (!g_backend) {
                auto* device = *reinterpret_cast<ID3D11Device**>(g_deviceSlot);
                if (!device) return;
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(&context);
                auto backend = std::make_unique<GpuFeedback::Backend>();
                GpuFeedback::Options options;
                options.maxRecords = kMaxRecords;
                options.maxBindings = kMaxBindings;
                options.maxTextures = kMaxTextures;
                options.maxMeshes = 1;
                if (!backend->Initialize(device, context.Get(), options)) {
                    logger::error("GPU feedback disabled: {}", backend->LastError());
                    g_enabled.store(false);
                    return;
                }
                g_backend = backend.release();
            }
            std::uint64_t delivered{};
            for (const auto& result : g_backend->Poll(frame)) {
                if (!result.valid) continue;
                for (const auto& demand : result.textures) {
                    g_deliver(demand.identity.key, demand.identity.generation, demand.decoded, result.sourceFrame);
                    ++delivered;
                }
            }
            if (!g_submit.records.empty()) {
                const GpuFeedback::Batch batch{ g_submit.records, g_submit.bindings, g_submit.textures, {}, g_submit.materials };
                g_backend->Submit(batch, frame);
            }
            g_submit.Clear();
            const auto stats = g_backend->Statistics();
            std::scoped_lock lock(g_statsLock);
            if (stats.failures > g_stats.failures && g_stats.failures < 3)
                logger::warn("GPU feedback operation failed: {}", g_backend->LastError());
            g_stats.submitted = stats.submitted;
            g_stats.completed = stats.completed;
            g_stats.expired = stats.expired;
            g_stats.rejected = stats.rejected + stats.ringFull;
            g_stats.failures = stats.failures;
            g_stats.bytes = stats.allocatedBytes;
            g_stats.delivered += delivered;
            g_stats.ready = true;
        } catch (...) {
            g_enabled.store(false, std::memory_order_release);
            std::scoped_lock lock(g_statsLock);
            ++g_stats.failures;
        }
    }

    Stats Statistics()
    {
        std::scoped_lock lock(g_statsLock);
        auto result = g_stats;
        result.enabled = g_enabled.load();
        result.droppedDraws = g_dropped.load();
        return result;
    }
}
