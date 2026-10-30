#include "GpuGather.h"
#include "GpuGatherDispatch.h"
#include "Addresses.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr Addr::Location kCreate{ 1379034, 2276868, 0x1D8FE20 };
    constexpr Addr::Location kRetire{ 1278677, 2277426, 0x1DD35E0 };
    constexpr Addr::Location kCamera{ 185153, 2277087, 0x1DA8BF0 };
    constexpr Addr::Location kProcess{ 673619, 2276979, 0 };
    constexpr Addr::Location kDraw{ 1152191, 2318696, 0 };
    constexpr std::array<Addr::Location, 2> kShaderTables{{
        { 343833, 343833, 0 },       // forward lighting
        { 1091304, 1091304, 0 }     // deferred prepass
    }};
    constexpr std::array<Addr::Location, 3> kMaterialTables{{
        { 1194322, 1194322, 0 },    // default
        { 1013115, 1013115, 0 },    // envmap
        { 294570, 294570, 0 }       // glowmap
    }};
    constexpr std::array<Addr::Location, 3> kStaticTables{{
        { 183326, 183326, 0x2E5A0B8 }, // BSTriShape
        { 403494, 403494, 0x30BDB38 }, // BSMeshLODTriShape
        { 248868, 248868, 0x2E757C8 }  // BSSubIndexTriShape
    }};
    constexpr std::array<std::uint8_t, 5> kCreateBytes{ 0x48, 0x89, 0x4C, 0x24, 0x08 };
    constexpr std::array<std::uint8_t, 5> kOtherBytes{ 0x48, 0x89, 0x5C, 0x24, 0x08 };
    constexpr std::size_t kMaximumBounds = 131072;
    using Create = void* (*)(void*, std::uint32_t*, void*, std::uint32_t, std::uint64_t);
    using Retire = void (*)(void*, void*);
    using Camera = void (*)(void*, void*, bool, float, float);
    using Process = void (*)(void*, void*);
    using NativeDraw = void (*)(void*);
    Create g_create{};
    Retire g_retire{};
    Camera g_camera{};
    Process g_process{};
    NativeDraw g_draw{};
    void* (*g_worldCamera)(){};
    GpuGather::Callback g_callback{};
    std::array<std::uintptr_t, 3> g_staticTables{};
    std::array<std::uintptr_t, 3> g_materialTables{};
    std::array<std::uintptr_t, 2> g_shaderTables{};
    std::uintptr_t g_propertyTable{};
    bool g_vr{}, g_installed{};
    std::atomic<std::uint64_t> g_frame{}, g_generation{};
    struct AtomicStats
    {
        std::atomic<std::uint64_t> uploads{}, bounds{}, retired{}, drawCalls{}, accepted{},
            unsupported{}, missingBounds{}, otherView{}, callbackFailures{}, capacityRejected{};
        std::atomic<std::uint64_t> cameraCalls{}, mainCameras{}, rejectedCameras{}, invalidCameras{}, lightingCalls{},
            missingView{}, staleView{}, nullPass{}, shaderSkinned{}, shaderTechnique{}, nullGeometry{},
            geometryType{}, geometrySkinned{}, missingShape{}, missingBuffer{}, descriptorMismatch{},
            invalidTransform{}, nullProperty{}, cachedCalls{}, cachedPasses{}, invalidCachedLists{},
            immediateCalls{}, shaderType{}, materialType{}, deformation{};
    } g_stats;
    struct Entry { GpuBounds::Bounds local; std::uint64_t generation{}, descriptor{}; };
    std::mutex g_lock;
    std::unordered_map<void*, Entry> g_bounds;
    struct CurrentView
    {
        std::array<GpuGather::Eye, 2> eyes;
        std::uint64_t frame{};
        std::uint32_t count{};
    };
    thread_local CurrentView t_view;

    template <class T> T Field(const void* object, std::size_t offset) noexcept
    {
        T value;
        std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
        return value;
    }

    void* OnCreate(void* renderer, std::uint32_t* bytes, void* source, std::uint32_t stride, std::uint64_t desc)
    {
        ++g_stats.uploads;
        std::optional<GpuBounds::Bounds> bounds;
        if (source && bytes && *bytes <= 64u * 1024u * 1024u)
            bounds = GpuBounds::Decode({ static_cast<const std::byte*>(source), *bytes }, stride, desc);
        // Native may rewrite source and byte count, and may split the stream.
        auto* buffer = g_create(renderer, bytes, source, stride, desc);
        if (!buffer) return buffer;
        try {
            std::scoped_lock lock(g_lock);
            g_bounds.erase(buffer); // a newly allocated address never inherits an old bound
            if (bounds) {
                if (g_bounds.size() < kMaximumBounds) {
                    g_bounds.emplace(buffer, Entry{ *bounds, ++g_generation, desc });
                    ++g_stats.bounds;
                } else ++g_stats.capacityRejected;
            }
        } catch (...) { ++g_stats.callbackFailures; }
        return buffer;
    }

    void OnRetire(void* creator, void* buffer)
    {
        try {
            std::scoped_lock lock(g_lock);
            g_stats.retired.fetch_add(g_bounds.erase(buffer), std::memory_order_relaxed);
        } catch (...) { ++g_stats.callbackFailures; }
        g_retire(creator, buffer);
    }

    void ObserveCamera(void* camera) noexcept
    {
        ++g_stats.cameraCalls;
        t_view = {};
        t_view.frame = g_frame.load(std::memory_order_relaxed);
        if (!camera || camera != g_worldCamera()) { ++g_stats.rejectedCameras; return; }
        const auto count = g_vr ? Field<std::uint32_t>(camera, 0x208) : 1u;
        if (!count || count > 2) { ++g_stats.invalidCameras; return; }
        const auto* positions = g_vr ? Field<const std::byte*>(camera, 0x1B8) : static_cast<const std::byte*>(camera) + 0xA0;
        const auto* frusta = g_vr ? Field<const std::byte*>(camera, 0x1A0) : static_cast<const std::byte*>(camera) + 0x160;
        if (!positions || !frusta) { ++g_stats.invalidCameras; return; }
        for (std::uint32_t eye = 0; eye < count; ++eye) {
            auto& result = t_view.eyes[eye];
            result.position = Field<std::array<float, 3>>(positions, eye * 12);
            result.frustum = Field<std::array<float, 6>>(frusta, eye * 28);
            result.orthographic = Field<bool>(frusta, eye * 28 + 24);
            for (float value : result.position) if (!std::isfinite(value)) { ++g_stats.invalidCameras; return; }
            for (float value : result.frustum) if (!std::isfinite(value)) { ++g_stats.invalidCameras; return; }
            if (result.orthographic || !GpuBounds::PerspectiveViewScale(result.frustum[0], result.frustum[1],
                    result.frustum[2], result.frustum[3])) { ++g_stats.invalidCameras; return; }
        }
        t_view.count = count;
        ++g_stats.mainCameras;
    }

    void OnCamera(void* state, void* camera, bool jitter, float nearPlane, float farPlane)
    {
        g_camera(state, camera, jitter, nearPlane, farPlane);
        ObserveCamera(camera);
    }

    enum class RigidFailure { None, Pass, Shader, Technique, Property, Material, Deformation, Geometry, GeometryType, Skin };
    struct RigidPass { void* geometry{}; void* property{}; void* material{}; std::uint32_t descriptor{}; };

    RigidFailure ClassifyRigidGeometry(void* geometry, void* property, RigidPass& result) noexcept
    {
        if (!property || Field<std::uintptr_t>(property, 0) != g_propertyTable) {
            return RigidFailure::Property;
        }
        auto* material = Field<void*>(property, 0x58);
        if (!material || std::find(g_materialTables.begin(), g_materialTables.end(),
                Field<std::uintptr_t>(material, 0)) == g_materialTables.end()) {
            return RigidFailure::Material;
        }
        constexpr auto rejectFlags = (1ull << 1) | (1ull << 10) | (1ull << 14) | (1ull << 17) |
            (1ull << 18) | (1ull << 21) | (1ull << 25) | (1ull << 33) | (1ull << 40) |
            (1ull << 45) | (1ull << 47) | (1ull << 56) | (1ull << 61);
        if (Field<std::uint64_t>(property, 0x30) & rejectFlags) {
            return RigidFailure::Deformation;
        }
        if (!geometry) return RigidFailure::Geometry;
        const auto table = Field<std::uintptr_t>(geometry, 0);
        if (std::find(g_staticTables.begin(), g_staticTables.end(), table) == g_staticTables.end()) {
            return RigidFailure::GeometryType;
        }
        const auto trailing = g_vr ? 0x40u : 0u;
        if (Field<void*>(geometry, 0x140 + trailing)) return RigidFailure::Skin;
        result = { geometry, property, material, 0 };
        return RigidFailure::None;
    }

    RigidFailure ClassifyRigidPass(void* pass, RigidPass& result) noexcept
    {
        if (!pass) return RigidFailure::Pass;
        auto* shader = Field<void*>(pass, 8);
        const auto shaderTable = shader ? Field<std::uintptr_t>(shader, 0) : 0;
        if (std::find(g_shaderTables.begin(), g_shaderTables.end(), shaderTable) == g_shaderTables.end())
            return RigidFailure::Shader;
        // Prepass descriptor bits19/20 select hull/domain shaders. Geometry
        // admission alone cannot prove a later shader technique is rigid.
        const auto descriptor = Field<std::uint32_t>(pass, 0x48);
        if (shaderTable == g_shaderTables[1] && (descriptor & ((1u << 19) | (1u << 20))))
            return RigidFailure::Technique;
        const auto failure = ClassifyRigidGeometry(Field<void*>(pass, 0x18), Field<void*>(pass, 0x10), result);
        if (failure == RigidFailure::None) result.descriptor = descriptor;
        return failure;
    }

    void ObserveDraw(void* pass)
    {
        ++g_stats.drawCalls;
        if (!t_view.count) {
            ++g_stats.missingView;
            ++g_stats.otherView;
            return;
        }
        if (t_view.frame != g_frame.load(std::memory_order_relaxed)) {
            ++g_stats.staleView;
            ++g_stats.otherView;
            return;
        }
        RigidPass classified;
        const auto failure = ClassifyRigidPass(pass, classified);
        if (failure != RigidFailure::None) {
            ++g_stats.unsupported;
            switch (failure) {
            case RigidFailure::Pass: ++g_stats.nullPass; break;
            case RigidFailure::Shader: ++g_stats.shaderType; break;
            case RigidFailure::Technique: ++g_stats.shaderTechnique; break;
            case RigidFailure::Property: ++g_stats.nullProperty; break;
            case RigidFailure::Material: ++g_stats.materialType; break;
            case RigidFailure::Deformation: ++g_stats.deformation; break;
            case RigidFailure::Geometry: ++g_stats.nullGeometry; break;
            case RigidFailure::GeometryType: ++g_stats.geometryType; break;
            case RigidFailure::Skin: ++g_stats.geometrySkinned; break;
            default: break;
            }
            return;
        }
        auto* geometry = classified.geometry;
        const auto trailing = g_vr ? 0x40u : 0u;
        auto* shape = Field<void*>(geometry, 0x148 + trailing);
        if (!shape) { ++g_stats.missingShape; ++g_stats.missingBounds; return; }
        auto* buffer = Field<void*>(shape, 8);
        Entry entry;
        {
            std::scoped_lock lock(g_lock);
            const auto found = g_bounds.find(buffer);
            if (found == g_bounds.end()) { ++g_stats.missingBuffer; ++g_stats.missingBounds; return; }
            entry = found->second;
        }
        if (Field<std::uint64_t>(geometry, 0x150 + trailing) != entry.descriptor) {
            ++g_stats.descriptorMismatch;
            ++g_stats.unsupported;
            return;
        }
        const auto transform = Field<std::array<float, 16>>(geometry, 0x70);
        auto world = GpuBounds::Transform(entry.local, transform.data(), transform.data() + 12, transform[15]);
        if (!world) { ++g_stats.invalidTransform; ++g_stats.unsupported; return; }
        GpuGather::Draw draw;
        draw.geometry = geometry;
        draw.shaderProperty = classified.property;
        draw.material = classified.material;
        draw.vertexBuffer = reinterpret_cast<std::uintptr_t>(buffer);
        draw.bufferGeneration = entry.generation;
        draw.frame = t_view.frame;
        draw.worldBounds = *world;
        draw.eyes = t_view.eyes;
        draw.eyeCount = t_view.count;
        draw.passDescriptor = classified.descriptor;
        draw.vertexDescriptor = entry.descriptor;
        g_callback(draw);
        ++g_stats.accepted;
    }

    void OnDraw(void* pass)
    {
        ++g_stats.immediateCalls;
        g_draw(pass);
        try { ObserveDraw(pass); } catch (...) { ++g_stats.callbackFailures; }
    }

    void OnProcess(void* renderer, void* commands, void* head)
    {
        ++g_stats.cachedCalls;
        g_process(renderer, commands); // native success-path submission first
        try {
            // Original consumes but does not alter the command pointers/list.
            // Reentrant callbacks fail closed without overwriting active scratch.
            thread_local bool active{};
            if (active) { ++g_stats.invalidCachedLists; return; }
            active = true;
            struct Reset { bool& value; ~Reset() { value = false; } } reset{ active };
            thread_local std::vector<std::uintptr_t> submitted;
            thread_local std::vector<void*> passes;
            if (!GpuGather::Dispatch::SubmittedPasses(commands, head, submitted, passes, [](void* pass) {
                ++g_stats.cachedPasses;
                ObserveDraw(pass);
            })) ++g_stats.invalidCachedLists;
        } catch (...) { ++g_stats.callbackFailures; }
    }

    bool CodeMatches(std::uintptr_t address, std::span<const std::uint8_t> bytes)
    {
        const auto text = REL::Module::get().segment(REL::Segment::text);
        return address >= text.address() && address + bytes.size() <= text.address() + text.size() &&
            std::memcmp(reinterpret_cast<const void*>(address), bytes.data(), bytes.size()) == 0;
    }

    std::uintptr_t Replay(std::uintptr_t address, std::span<const std::uint8_t> bytes)
    {
        auto* block = static_cast<std::uint8_t*>(F4SE::GetTrampoline().allocate(bytes.size() + 14));
        std::memcpy(block, bytes.data(), bytes.size());
        constexpr std::uint8_t jump[]{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(block + bytes.size(), jump, 6);
        const auto next = address + bytes.size();
        std::memcpy(block + bytes.size() + 6, &next, 8);
        if (!REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), block, bytes.size() + 14)) return 0;
        return reinterpret_cast<std::uintptr_t>(block);
    }

    std::vector<std::uintptr_t> CallsTo(std::uintptr_t target)
    {
        std::vector<std::uintptr_t> sites;
        const auto text = REL::Module::get().segment(REL::Segment::text);
        const auto* begin = reinterpret_cast<const std::uint8_t*>(text.address());
        for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
            if (begin[i] != 0xE8) continue;
            const auto site = text.address() + i;
            if (site + 5 + Field<std::int32_t>(begin + i, 1) == target) sites.push_back(site);
        }
        return sites;
    }

    std::uintptr_t MakeGateway(bool inlineBatch)
    {
        const auto code = GpuGather::Dispatch::Gateway(inlineBatch, reinterpret_cast<std::uintptr_t>(OnProcess));
        auto* block = static_cast<std::uint8_t*>(F4SE::GetTrampoline().allocate(code.size()));
        std::memcpy(block, code.data(), code.size());
        if (!REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), block, code.size())) return 0;
        return reinterpret_cast<std::uintptr_t>(block);
    }
}

namespace GpuGather
{
    bool Install(bool experimental, Callback callback)
    {
        if (!experimental || !callback) return false;
        if (g_installed) return callback == g_callback;
        // VR's shader submission ABI differs. Do not install the obsolete
        // forward hook or infer its cached layout from the flat renderer.
        if (REL::Module::IsVR()) {
            logger::error("Experimental GPU gather unavailable: VR submitted-draw ABI not yet verified");
            return false;
        }
        const auto create = Addr::Resolve(kCreate), retire = Addr::Resolve(kRetire), camera = Addr::Resolve(kCamera);
        const auto process = Addr::Resolve(kProcess), nativeDraw = Addr::Resolve(kDraw);
        const auto worldCamera = Addr::Resolve(Addr::WorldRootCamera);
        for (std::size_t i = 0; i < g_staticTables.size(); ++i) g_staticTables[i] = Addr::Resolve(kStaticTables[i]);
        for (std::size_t i = 0; i < g_materialTables.size(); ++i) g_materialTables[i] = Addr::Resolve(kMaterialTables[i]);
        for (std::size_t i = 0; i < g_shaderTables.size(); ++i) g_shaderTables[i] = Addr::Resolve(kShaderTables[i]);
        g_propertyTable = Addr::Resolve(Addr::LightingShaderPropertyVtbl);
        constexpr std::array<std::uint8_t, 16> processBytes{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 };
        if (!worldCamera || !g_propertyTable || std::find(g_staticTables.begin(), g_staticTables.end(), 0) != g_staticTables.end() ||
            std::find(g_materialTables.begin(), g_materialTables.end(), 0) != g_materialTables.end() ||
            std::find(g_shaderTables.begin(), g_shaderTables.end(), 0) != g_shaderTables.end() ||
            !CodeMatches(create, kCreateBytes) || !CodeMatches(retire, kOtherBytes) || !CodeMatches(camera, kOtherBytes) ||
            !CodeMatches(process, processBytes) || !nativeDraw || F4SE::GetTrampoline().free_size() < 384) {
            logger::error("Experimental GPU gather signature/ABI validation failed; no hooks installed");
            return false;
        }
        for (std::size_t i = 0; i < g_materialTables.size(); ++i) {
            const auto table = reinterpret_cast<void*>(g_materialTables[i]);
            const auto feature = Field<std::uintptr_t>(table, 5 * 8), type = Field<std::uintptr_t>(table, 6 * 8);
            const std::array<std::uint8_t, 6> expectedFeature{ 0xB8, static_cast<std::uint8_t>(i), 0, 0, 0, 0xC3 };
            constexpr std::array<std::uint8_t, 3> featureZero{ 0x33, 0xC0, 0xC3 };
            constexpr std::array<std::uint8_t, 6> lightingType{ 0xB8, 2, 0, 0, 0, 0xC3 };
            if (!CodeMatches(type, lightingType) || !(i ? CodeMatches(feature, expectedFeature) : CodeMatches(feature, featureZero))) {
                logger::error("Experimental GPU gather material ABI validation failed; no hooks installed");
                return false;
            }
        }
        const auto cachedSites = CallsTo(process), drawSites = CallsTo(nativeDraw);
        const auto modern = REL::Module::IsNG();
        if (cachedSites.size() != (modern ? 2u : 1u) || drawSites.size() != 5) {
            logger::error("Experimental GPU gather call counts differ: cached={} draw={}; no hooks installed", cachedSites.size(), drawSites.size());
            return false;
        }
        // Exact surrounding instructions prove the borrowed head register and
        // location after successful SetupTechnique. Modern RenderBatches also
        // contains an inlined copy of the cached submission branch.
        constexpr std::array<std::uint8_t, 10> outAfter{ 0x48,0x8B,0x1B,0x48,0x85,0xDB,0x74,0x2F,0x0F,0xB6 };
        constexpr std::array<std::uint8_t, 9> inlineBefore{ 0x01,0x00,0x48,0x8B,0xD7,0x48,0x89,0x28,0xE8 };
        constexpr std::array<std::uint8_t, 7> inlineAfter{ 0x4A,0x8B,0x1C,0xF6,0x48,0x85,0xDB };
        constexpr std::array<std::uint8_t, 3> outBefore{ 0x4C,0x89,0x38 };
        std::vector<bool> inlineSites;
        std::size_t inlineCount{};
        for (const auto site : cachedSites) {
            const bool isInline = CodeMatches(site + 5, inlineAfter) && CodeMatches(site - 8, inlineBefore);
            const bool isOut = CodeMatches(site + 5, outAfter) && CodeMatches(site - 3, outBefore);
            if (isInline == isOut) {
                logger::error("Experimental GPU gather cached-call context mismatch; no hooks installed");
                return false;
            }
            inlineSites.push_back(isInline);
            inlineCount += isInline;
        }
        if (inlineCount != (modern ? 1u : 0u)) return false;
        // Only the standard rigid immediate path is admitted. Skinned/custom
        // and light-volume direct calls retain their exact original behavior.
        constexpr std::array<std::uint8_t, 12> standardBefore{ 0x48,0x8B,0xD7,0x48,0x8B,0xCE,0xFF,0x50,0x38,0x48,0x8B,0xCF };
        constexpr std::array<std::uint8_t, 12> standardAfter{ 0x48,0x8B,0x06,0x48,0x8B,0xD7,0x48,0x8B,0xCE,0xFF,0x50,0x40 };
        std::uintptr_t standardSite{};
        for (const auto site : drawSites) {
            if (CodeMatches(site - standardBefore.size(), standardBefore) && CodeMatches(site + 5, standardAfter)) {
                if (standardSite) return false;
                standardSite = site;
            }
        }
        if (!standardSite) {
            logger::error("Experimental GPU gather immediate-call context mismatch; no hooks installed");
            return false;
        }
        // Allocate and validate all replay blocks before writing any game code.
        const auto createReplay = Replay(create, kCreateBytes), retireReplay = Replay(retire, kOtherBytes), cameraReplay = Replay(camera, kOtherBytes);
        const auto outGateway = MakeGateway(false), inlineGateway = modern ? MakeGateway(true) : outGateway;
        if (!createReplay || !retireReplay || !cameraReplay || !outGateway || !inlineGateway) return false;
        g_create = reinterpret_cast<Create>(createReplay);
        g_retire = reinterpret_cast<Retire>(retireReplay);
        g_camera = reinterpret_cast<Camera>(cameraReplay);
        g_process = reinterpret_cast<Process>(process);
        g_draw = reinterpret_cast<NativeDraw>(nativeDraw);
        g_worldCamera = reinterpret_cast<decltype(g_worldCamera)>(worldCamera);
        g_callback = callback;
        g_vr = REL::Module::IsVR();
        F4SE::GetTrampoline().write_branch<5>(create, OnCreate);
        F4SE::GetTrampoline().write_branch<5>(retire, OnRetire);
        F4SE::GetTrampoline().write_branch<5>(camera, OnCamera);
        for (std::size_t i = 0; i < cachedSites.size(); ++i)
            F4SE::GetTrampoline().write_call<5>(cachedSites[i], inlineSites[i] ? inlineGateway : outGateway);
        F4SE::GetTrampoline().write_call<5>(standardSite, OnDraw);
        g_installed = true;
        logger::warn("Experimental GPU gather enabled: {} cached submission calls + rigid immediate draw; verified actual command membership, material classes and main camera; no HiZ/per-pixel visibility or displaced/skinned/effect coverage", cachedSites.size());
        return true;
    }

    void SetFrame(std::uint64_t frame) noexcept { g_frame.store(frame, std::memory_order_relaxed); }
    bool CompatibleRigidPass(void* pass) noexcept
    {
        if (!g_installed) return false;
        RigidPass classified;
        return ClassifyRigidPass(pass, classified) == RigidFailure::None;
    }
    bool CompatibleRigidGeometry(void* geometry, void* property) noexcept
    {
        if (!g_installed) return false;
        RigidPass classified;
        return ClassifyRigidGeometry(geometry, property, classified) == RigidFailure::None;
    }
    Stats Statistics() noexcept
    {
        return { g_stats.uploads.load(), g_stats.bounds.load(), g_stats.retired.load(), g_stats.drawCalls.load(), g_stats.accepted.load(),
            g_stats.unsupported.load(), g_stats.missingBounds.load(), g_stats.otherView.load(), g_stats.callbackFailures.load(), g_stats.capacityRejected.load(),
            g_stats.cameraCalls.load(), g_stats.mainCameras.load(), g_stats.rejectedCameras.load(), g_stats.invalidCameras.load(), g_stats.lightingCalls.load(),
            g_stats.missingView.load(), g_stats.staleView.load(), g_stats.nullPass.load(), g_stats.shaderSkinned.load(), g_stats.shaderTechnique.load(),
            g_stats.nullGeometry.load(), g_stats.geometryType.load(), g_stats.geometrySkinned.load(), g_stats.missingShape.load(), g_stats.missingBuffer.load(),
            g_stats.descriptorMismatch.load(), g_stats.invalidTransform.load(), g_stats.nullProperty.load(),
            g_stats.cachedCalls.load(), g_stats.cachedPasses.load(), g_stats.invalidCachedLists.load(),
            g_stats.immediateCalls.load(), g_stats.shaderType.load(), g_stats.materialType.load(), g_stats.deformation.load() };
    }
}
