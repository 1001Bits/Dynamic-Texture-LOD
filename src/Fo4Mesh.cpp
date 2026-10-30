#include "Fo4Mesh.h"
#include "Fo4MeshNative.h"
#include "Fo4MeshPolicy.h"
#include "Fo4MeshResources.h"
#include "Fo4MeshRetireStub.h"
#include "Fo4MeshCountStub.h"
#include "Fo4MeshHeapHooks.h"
#include "Fo4MeshPass.h"
#include "Fo4MeshPilot.h"
#include "Fo4MeshOwners.h"
#include "Fo4MeshDetach.h"
#include "Fo4MeshLifetime.h"
#include "Addresses.h"
#include "core/Settings.h"
#include <Windows.h>
#include <intrin.h>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Fo4Mesh {
namespace {
    // All-consumer admission is not yet proven: an unknown future shader can
    // bind native geometry SRVs before the draw hook sees the pass. Keep the
    // adapter compile-tested but uninstalled until that exclusion is verified.
    constexpr bool kAllConsumerAdmissionVerified=false;
    // Scoped audit: exact hash-pinned four-block rigid fixture/plain STAT only.
    // Cloth/dismember are excluded; native property/copy/combiner boundaries
    // restore before use and prevent enrollment during in-flight consumers.
    // Evidence: docs/research/equivalence/fo4-pilot-nonrender-consumers.md.
    constexpr bool kPilotNonRenderingConsumersVerified=true;
    // The observed clone certificate is necessary but is not a substitute for
    // auditing every applicable native shape-reference writer. Saved call
    // xrefs alone do not exclude inlined writes. Keep refs2 mutation held until
    // that exact-fixture lifecycle coverage is established independently.
    constexpr bool kPilotSharedReferenceWritersVerified=false;
    // Private fixture only: source remains full while a separately owned full
    // native pair is verified and transferred to the placed clone.
    // Independently reviewed against native creation, the pure leaf setter,
    // both cached factories and the exact pruned plain-STAT fixture lifecycle.
    // This does not authorize shared-source mutation or ordinary vanilla meshes.
    constexpr bool kPilotDetachVerified=true;
    constexpr std::uint64_t kSourceCap=32ull<<20,kStagingCap=16ull<<20,kRetainedCap=64ull<<20;
    constexpr std::uint32_t kMaximumVertices=65535*60,kMaximumIndices=2u<<20;
    constexpr auto kTimeout=std::chrono::seconds(5);
    using Clock=std::chrono::steady_clock;
    using Draw=void(*)(void*,NativeShape*,std::uint32_t,std::uint32_t);
    using Count=std::uint32_t(*)(void*,std::uint32_t);
    using Create=NativeShape*(*)(void*,std::uint32_t*,void*,std::uint64_t,std::uint16_t*,std::uint32_t);
    using Release=void(*)(void*,NativeShape*);
    using Cache=void*(*)(void*,void*);
    using Immediate=void(*)(void*,bool);
    using CustomImmediate=void(*)(void*);
    using PropertyChange=void(*)(void*,void*);
    using CopyMembers=void(*)(void*,void*,void*);
    using AddTriShape=void(*)(void*,void*,bool,std::uint16_t,std::uint32_t,std::uint32_t,void*,std::uint32_t*);
    using DoCombine=void*(*)(void*,const char*);
    using SetRendererData=void(*)(void*,NativeShape*);
    Draw g_draw{};Count g_count{},g_start{};Create g_create{};Release g_release{},g_addRef{};Cache g_cache{};
    Immediate g_standard{},g_skinned{};CustomImmediate g_custom{};
    PropertyChange g_attachProperty{},g_setProperty{};CopyMembers g_copyMembers{},g_shapeCopyMembers{};
    AddTriShape g_addTriShape{};DoCombine g_doCombine{};
    SetRendererData g_setRendererData{};
    std::array<std::uintptr_t,4> g_nativeShaders{};
    std::uintptr_t g_meshTable{},g_type5Return{},g_deviceSlot{};
    std::uint32_t* g_tlsIndex{};
    void* g_renderer{};
    ID3D11Device* g_device{};ID3D11DeviceContext* g_context{};
    ComPtr<ID3D11DeviceContext> g_contextOwner;
    std::optional<DrawReserve> g_reserve;
    std::optional<Pilot::Identity> g_pilot;
    Pilot::OwnerProvenance g_ownerProof;
    std::uint32_t g_ownerProofReceipts{};
    std::atomic<bool> g_installed{};
    std::atomic<bool> g_inspecting{};
    std::atomic<bool> g_passObserving{};
    std::recursive_mutex g_lock;
    Stats g_stats;
    std::uint64_t g_generation{},g_frame{},g_round=1;
    std::uint64_t g_nonRenderingActive{}; // registry lock protects admission vs native consumer scopes
    dtl::StreamingLedger g_ledger;
    F4SE::Trampoline g_trampoline{"DynamicTextureLOD.Fo4Mesh"};
    RUNTIME_FUNCTION g_retireFunction{};

    struct Record {
        NativeShape* native{};
        NativeBuffer* expectedVertices{};NativeBuffer* expectedIndices{};
        ID3D11Buffer* expectedVertexResource{};ID3D11Buffer* expectedIndexResource{};
        std::uint64_t generation{},descriptor{},reservedBytes{},initialAllocatedBytes{},pairRevision=1;
        std::uint64_t ownerCertificate{};
        std::uint64_t detachTicket{};
        std::uint32_t vertexOffset{},indexOffset{},vertexCapacity{},indexCapacity{},vertexSize{},indexSize{};
        bool vertexPooled{},indexPooled{};
        std::uint32_t vertices{},triangles{},stride{};
        std::array<std::uint32_t,3> segments{};
        Data source;
        GpuBounds::Bounds localBounds;
        std::optional<Readback> readback;
        std::optional<Readback> candidateReadback;
        std::optional<PairIdentity> candidateIdentity;
        std::unique_ptr<PolicyState> policy;
        NativeShape* candidate{};
        Clock::time_point copyAt{},candidateAt{};
        std::uint8_t candidateLevel{};
        std::uint64_t candidateBytes{};
        bool pinned{},forceFull{},candidateManual{},awaitingNativeReady{},preferPooled{},candidateLifetimeObserved{};
        bool candidateVerified{},candidateAbandoned{},candidateQueryFailed{};
        Pilot::FullPair pilotPair;
        std::uint8_t pilotSeenDrawStates{};
        std::array<std::uint64_t,12> pilotPolicyState{};
        std::uint8_t pilotPolicyReceipts{};
    };
    struct Retired {
        NativeShape* native{};
        ComPtr<ID3D11Query> completion;
        std::uint64_t bytes{};
        Clock::time_point submitted;
        bool failed{};
    };
    std::unordered_map<NativeShape*,Record> g_records;
    struct DetachPending {
        Pilot::DetachOwnership ownership;
        Record source;
        NativeShape* full{};
        std::optional<Readback> readback;
        Clock::time_point started{};
        std::uint64_t newNative{},fullCapacity{};
        std::uint32_t refsBefore{},refsAfter{},candidateReferences{};
        bool candidateReadback{},queryFailed{},sourceVerified{},candidateVerified{},candidateLifetimeObserved{};
        std::optional<PairIdentity> candidateIdentity;
    };
    std::optional<DetachPending> g_detach;
    struct OrphanReadback {NativeShape* native{};Readback readback;std::uint64_t bytes{};bool failed{};};
    std::optional<OrphanReadback> g_orphanReadback;
    std::array<std::uintptr_t,8> g_unknownShaderTables{};
    std::deque<Retired> g_retired;
    thread_local void* t_geometry{};
    thread_local void* t_pass{};
    thread_local IndexRange t_nativeRange{};
    thread_local bool t_selected{};
    thread_local NativeShape* t_fullSourceOverride{};
    thread_local std::uint64_t t_ownerCopy{};
    void OnNativeFrame(void* creator,ID3D11DeviceContext* context);
    bool RestorePilot(Record& record,const char* reason,bool pin);
    void InvalidateOwnerProof(NativeShape* native,const char* reason);
    bool DetachCache(void* geometry);
    void TryPublishDetach(void* geometry);
    void CancelDetach(const char* reason);
    void CancelDetachForNative(NativeShape* native,const char* reason);

    template<class T> T Field(const void* object,std::size_t offset) {
        T value;std::memcpy(&value,static_cast<const std::byte*>(object)+offset,sizeof(value));return value;
    }
    std::byte* ThreadData() {
        auto** slots=reinterpret_cast<std::byte**>(__readgsqword(0x58));
        return slots && g_tlsIndex ? slots[*g_tlsIndex] : nullptr;
    }
    ID3D11DeviceContext* DrawContext() {
        auto* data=ThreadData();return data ? Field<ID3D11DeviceContext*>(data,0xb18) : nullptr;
    }
    auto Thresholds() {
        const auto& settings=dtl::GetSettings();
        return dtl::StarfieldMeshThresholds(settings.sourceMeshScreenSizes,settings.sourceMeshFadePercent,settings.sourceMeshStreamingBias);
    }
    bool Contains(IndexRange outer,IndexRange inner) {
        return inner.first>=outer.first && inner.first-outer.first<=outer.count && inner.count<=outer.count-(inner.first-outer.first);
    }
    bool SamePair(const Record& r) {
        return r.native->descriptor==r.descriptor && r.native->vertices==r.expectedVertices && r.native->indices==r.expectedIndices &&
            r.expectedVertices->buffer==r.expectedVertexResource && r.expectedIndices->buffer==r.expectedIndexResource &&
            r.expectedVertices->offset==r.vertexOffset && r.expectedIndices->offset==r.indexOffset &&
            r.expectedVertices->capacity==r.vertexCapacity && r.expectedIndices->capacity==r.indexCapacity &&
            r.expectedVertices->size==r.vertexSize && r.expectedIndices->size==r.indexSize &&
            r.expectedVertices->suballocated==r.vertexPooled && r.expectedIndices->suballocated==r.indexPooled;
    }
    bool ShapeOwnership(const Record& r) {
        const auto references=AtomicValue(r.native->references);
        return r.ownerCertificate ? (g_pilot && kPilotSharedReferenceWritersVerified &&
            g_ownerProof.Certifies(reinterpret_cast<std::uint64_t>(r.native),references,r.ownerCertificate)) : references==1;
    }
    bool Unique(const Record& r) {
        return r.native->vertices && r.native->indices && ShapeOwnership(r) &&
            ExclusiveWindowReady(r.native->vertices,r.native->vertices->size) &&
            ExclusiveWindowReady(r.native->indices,r.native->indices->size);
    }
    bool ProvenOwnerGeometry(const Record& r,void* geometry) {
        return !r.ownerCertificate || g_ownerProof.Includes(reinterpret_cast<std::uint64_t>(r.native),
            reinterpret_cast<std::uint64_t>(geometry),AtomicValue(r.native->references),r.ownerCertificate);
    }
    void Cancel(Record& r) {
        if (r.policy && r.policy->Cancel(g_ledger)) ++g_stats.cancelled;
    }
    void UpdatePair(Record& r) {
        r.expectedVertices=r.native->vertices;r.expectedIndices=r.native->indices;
        r.expectedVertexResource=r.native->vertices->buffer;r.expectedIndexResource=r.native->indices->buffer;
        r.vertexOffset=r.native->vertices->offset;r.indexOffset=r.native->indices->offset;
        r.vertexCapacity=r.native->vertices->capacity;r.indexCapacity=r.native->indices->capacity;
        r.vertexSize=r.native->vertices->size;r.indexSize=r.native->indices->size;
        r.vertexPooled=r.native->vertices->suballocated;r.indexPooled=r.native->indices->suballocated;
    }
    bool ShapeGeometry(void* geometry,NativeShape* native) {
        return geometry && Field<std::uintptr_t>(geometry,0)==g_meshTable && Field<std::uint8_t>(geometry,0x158)==5 &&
            !Field<void*>(geometry,0x140) && Field<NativeShape*>(geometry,0x148)==native && native &&
            Field<std::uint64_t>(geometry,0x150)==native->descriptor;
    }
    bool PilotGeometry(void* geometry) {
        return !g_pilot || (geometry && g_pilot->Metadata(static_cast<RE::NiObjectNET*>(geometry)->GetName(),
            Field<std::uint64_t>(geometry,0x150),Field<std::uint16_t>(geometry,0x164),
            Field<std::uint32_t>(geometry,0x160),Field<std::array<std::uint32_t,3>>(geometry,0x170)));
    }
    void OwnerProofReceipt(const char* event,const char* reason,std::uint32_t references) noexcept {
        if (!g_pilot || g_ownerProofReceipts>=32) return;
        ++g_ownerProofReceipts;
        const auto& proof=g_ownerProof.Get();
        try {
            logger::info("[fo4-mesh-pilot-owners] event={} reason={} certificate={} native={:X} sourceGeometry={:X} "
                "destinationGeometry={:X} stage={} matchedAddRefs={} observedRefs={} sharedAdmissionVerified={} "
                "scope=hash-pinned-native-clone addresses=identity-only",
                event,reason,proof.certificate,proof.native,proof.source,proof.destination,
                static_cast<unsigned>(proof.stage),proof.matchedAdds,references,kPilotSharedReferenceWritersVerified);
        } catch(const std::exception&) {}
    }
    void InvalidateOwnerProof(NativeShape* native,const char* reason) {
        if (!g_pilot || !native) return;
        CancelDetachForNative(native,reason);
        if (g_ownerProof.Invalidate(reinterpret_cast<std::uint64_t>(native)))
            OwnerProofReceipt("invalidated",reason,AtomicValue(native->references));
        if (const auto found=g_records.find(native);found!=g_records.end())
            RestorePilot(found->second,reason,true);
    }
    void LogPilotAdmissionRejected(void* geometry,NativeShape* native,bool allowPending,bool ready,
        std::uint32_t vertices,std::uint32_t stride,std::uint32_t triangles,
        const std::array<std::uint32_t,3>& segments) noexcept {
        // Called only after exact pilot metadata and the rigid classifier have
        // matched, while the caller owns the borrowed geometry and g_lock.
        // Record at most 16 distinct scalar snapshots; never retain native
        // objects, inspect GPU data, or change the admission decision here.
        if (!g_pilot) return;
        using Snapshot=std::array<std::uint64_t,24>;
        static std::array<Snapshot,16> seen{};
        static std::size_t seenCount{};
        if (seenCount==seen.size()) return;
        const auto vertexBytes=std::uint64_t(vertices)*stride,indexBytes=std::uint64_t(triangles)*6;
        const auto shapeRefs=AtomicValue(native->references);
        const auto* vb=native->vertices;const auto* ib=native->indices;
        const auto vertexRefs=AtomicValue(native->vertices->references),indexRefs=AtomicValue(native->indices->references);
        const auto vertexPending=AtomicValue(native->vertices->pendingRequests),indexPending=AtomicValue(native->indices->pendingRequests);
        const auto vertexSR=AtomicValue(native->vertices->shaderAcquires),indexSR=AtomicValue(native->indices->shaderAcquires);
        const auto vertexUA=AtomicValue(native->vertices->unorderedAcquires),indexUA=AtomicValue(native->indices->unorderedAcquires);
        const bool layoutValid=vertices && stride && (native->descriptor&(1ull<<44)) && !(native->descriptor&(1ull<<50));
        const bool rangeValid=bool(NativeLODRange(segments,3,triangles));
        const bool byteLimitsValid=vertexBytes<=kMaximumVertices && indexBytes<=kMaximumIndices;
        const bool vertexSizeValid=vb->size==AllocationBytes(vertexBytes),indexSizeValid=ib->size==AllocationBytes(indexBytes);
        const Snapshot snapshot{allowPending,ready,shapeRefs,vertexRefs,indexRefs,vertexPending,indexPending,
            vb->pendingCopy,ib->pendingCopy,vb->size,ib->size,vb->capacity,ib->capacity,vb->offset,ib->offset,
            vb->suballocated,ib->suballocated,vertexSR,indexSR,vertexUA,indexUA,
            vb->buffer!=nullptr,ib->buffer!=nullptr,
            std::uint64_t(layoutValid)|(std::uint64_t(rangeValid)<<1)|(std::uint64_t(byteLimitsValid)<<2)};
        if (std::find(seen.begin(),seen.begin()+seenCount,snapshot)!=seen.begin()+seenCount) return;
        const auto diagnostic=++seenCount;seen[diagnostic-1]=snapshot;
        try {
            logger::warn("[fo4-mesh-pilot-admission-rejected] diagnostic={} phase={} geometry={:X} native={:X} "
                "descriptor={:016X} vertices={} stride={} triangles={} segments=({},{},{}) "
                "layoutValid={} rangeValid={} byteLimitsValid={} shapeRefs={} ready={} allowPending={} "
                "vertexSizeValid={} indexSizeValid={} mutation=none",
                diagnostic,allowPending ? "cache-builder" : "live-count",reinterpret_cast<std::uintptr_t>(geometry),
                reinterpret_cast<std::uintptr_t>(native),native->descriptor,vertices,stride,triangles,
                segments[0],segments[1],segments[2],layoutValid,rangeValid,byteLimitsValid,shapeRefs,ready,allowPending,
                vertexSizeValid,indexSizeValid);
            const auto logBuffer=[diagnostic](const char* role,const NativeBuffer* buffer,std::uint64_t expectedBytes,
                std::uint32_t refs,std::uint32_t pending,std::uint32_t sr,std::uint32_t ua) {
                logger::warn("[fo4-mesh-pilot-admission-buffer] diagnostic={} role={} wrapper={:X} resource={:X} "
                    "expectedSize={} size={} capacity={} offset={} pooled={} refs={} pendingRequests={} pendingCopy={} "
                    "shaderAcquires={} unorderedAcquires={} cpuDataPresent={} invalidCpuData={} mutation=none",
                    diagnostic,role,reinterpret_cast<std::uintptr_t>(buffer),reinterpret_cast<std::uintptr_t>(buffer->buffer),
                    AllocationBytes(expectedBytes),buffer->size,buffer->capacity,buffer->offset,buffer->suballocated,refs,
                    pending,buffer->pendingCopy,sr,ua,buffer->data!=nullptr,buffer->invalidCpuData);
            };
            logBuffer("vertices",vb,vertexBytes,vertexRefs,vertexPending,vertexSR,vertexUA);
            logBuffer("indices",ib,indexBytes,indexRefs,indexPending,indexSR,indexUA);
        } catch (const std::exception&) {
            // A diagnostic allocation or logger failure must not alter the
            // original rejection, retain resources, or interrupt native draw.
        }
    }
    void Inspect(const GpuGather::Draw& draw) {
        // This synchronous borrowed view takes no engine/COM reference and
        // keeps no raw pointer. It measures only the submitted gather subset.
        auto* native=Field<NativeShape*>(draw.geometry,0x148);
        if (!ShapeGeometry(draw.geometry,native) || !native->vertices || !native->indices ||
            draw.vertexDescriptor!=native->descriptor) return;
        const auto vertexBytes=std::uint64_t(Field<std::uint16_t>(draw.geometry,0x164))*((native->descriptor&15)*4);
        const auto indexBytes=std::uint64_t(Field<std::uint32_t>(draw.geometry,0x160))*6;
        std::scoped_lock lock(g_lock);++g_stats.diagnosticDraws;
        if (native->vertices->suballocated || native->indices->suballocated ||
            native->vertices->offset || native->indices->offset) ++g_stats.diagnosticPooled;
        if (AtomicValue(native->references)!=1) ++g_stats.diagnosticSharedShapes;
        if (draw.shaderProperty && Field<void*>(draw.shaderProperty,0x50)) ++g_stats.diagnosticEffectData;
        if (AtomicValue(native->references)==1 && vertexBytes<=kMaximumVertices && indexBytes<=kMaximumIndices &&
            OwningReady(native->vertices,vertexBytes) && OwningReady(native->indices,indexBytes)) ++g_stats.diagnosticOwningReady;
        if (AtomicValue(native->references)==1 && vertexBytes<=kMaximumVertices && indexBytes<=kMaximumIndices &&
            ExclusiveWindowReady(native->vertices,vertexBytes) && ExclusiveWindowReady(native->indices,indexBytes)) ++g_stats.diagnosticExclusiveReady;
    }
    Record* Enroll(void* geometry,bool allowPending=false) {
        auto* native=geometry ? Field<NativeShape*>(geometry,0x148) : nullptr;
        if (!g_stats.healthy || g_nonRenderingActive || !ShapeGeometry(geometry,native) || !native->vertices || !native->indices) return nullptr;
        if (!PilotGeometry(geometry) || (g_pilot && g_stats.enrolled)) return nullptr;
        if (!GpuGather::CompatibleRigidGeometry(geometry,Field<void*>(geometry,0x138))) return nullptr;
        const auto nativeIdentity=reinterpret_cast<std::uint64_t>(native);
        const auto geometryIdentity=reinterpret_cast<std::uint64_t>(geometry);
        const auto nativeReferences=AtomicValue(native->references);
        if (g_pilot && g_ownerProof.ValidateDestination(nativeIdentity,geometryIdentity,nativeReferences,true))
            OwnerProofReceipt("destination-validated","borrowed-final-Mesh-metadata",nativeReferences);
        const auto ownerCertificate=g_pilot && kPilotSharedReferenceWritersVerified &&
            g_ownerProof.Includes(nativeIdentity,geometryIdentity,nativeReferences,g_ownerProof.Get().certificate) ?
            g_ownerProof.Get().certificate : 0;
        const auto count=Field<std::uint32_t>(geometry,0x160);
        const auto vertices=Field<std::uint16_t>(geometry,0x164);
        const auto stride=static_cast<std::uint32_t>(native->descriptor&15)*4;
        const auto segments=Field<std::array<std::uint32_t,3>>(geometry,0x170);
        const auto vertexBytes=std::uint64_t(vertices)*stride,indexBytes=std::uint64_t(count)*6;
        const bool ready=ExclusiveWindowReady(native->vertices,vertexBytes) && ExclusiveWindowReady(native->indices,indexBytes);
        if (!vertices || !stride || !(native->descriptor&(1ull<<44)) || (native->descriptor&(1ull<<50)) ||
            !NativeLODRange(segments,3,count) || vertexBytes>kMaximumVertices || indexBytes>kMaximumIndices ||
            (!ownerCertificate && nativeReferences!=1) || (!ready && !allowPending) ||
            native->vertices->size!=AllocationBytes(vertexBytes) || native->indices->size!=AllocationBytes(indexBytes)) {
            ++g_stats.owningRejected;
            LogPilotAdmissionRejected(geometry,native,allowPending,ready,vertices,stride,count,segments);
            return nullptr;
        }
        const auto bytes=vertexBytes+indexBytes;
        if (g_records.size()>=4096 || bytes>kSourceCap-g_stats.sourceBytes) return nullptr;
        Record r;r.native=native;r.generation=++g_generation;r.descriptor=native->descriptor;r.reservedBytes=bytes;
        r.ownerCertificate=ownerCertificate;
        r.vertices=vertices;r.triangles=count;r.stride=stride;r.segments=segments;
        r.expectedVertices=native->vertices;r.expectedIndices=native->indices;
        if (ready) {UpdatePair(r);r.initialAllocatedBytes=std::uint64_t(r.vertexCapacity)+r.indexCapacity;}
        r.preferPooled=r.vertexPooled || r.indexPooled;
        r.awaitingNativeReady=!ready;r.copyAt=Clock::now();
        const auto [it,inserted]=g_records.emplace(native,std::move(r));
        if (!inserted) return &it->second;
        g_stats.sourceBytes+=bytes;++g_stats.enrolled;
        if (ready && (it->second.vertexPooled || it->second.indexPooled)) ++g_stats.initialPoolSources;
        if (g_pilot) {
            const auto position=Field<std::array<float,3>>(geometry,0xa0);
            logger::info("[fo4-mesh-pilot-owner] generation={} geometry={:X} native={:X} worldPosition=({},{},{}) initialRefs={} maximumRecords=1 ownerCertificate={} provenOwners={}",
                it->second.generation,reinterpret_cast<std::uintptr_t>(geometry),reinterpret_cast<std::uintptr_t>(native),
                position[0],position[1],position[2],AtomicValue(native->references),ownerCertificate,ownerCertificate ? 2 : 1);
        }
        return &it->second;
    }
    void* HookCache(void* shader,void* descriptor) {
        // Both native creators propagate nullptr into a pass; the submitted
        // renderer then takes its ordinary live-draw fallback. Serialize the
        // unregistered path too so its wrapper AddRefs cannot race enrollment.
        std::scoped_lock lock(g_lock);
        auto* geometry=descriptor ? Field<void*>(descriptor,0) : nullptr;
        auto* native=geometry ? Field<NativeShape*>(geometry,0x148) : nullptr;
        if (!g_installed.load()) {
            // Bounded routing diagnostic, not a performance treatment. Both
            // native creators support nullptr as the live-draw fallback. Only
            // exact rigid type5 is routed, with no native buffer/reference or
            // draw-count mutation. This reveals consumers hidden by caches.
            if (!g_pilot && g_stats.diagnosticCacheBypasses<1024 && ShapeGeometry(geometry,native) &&
                AtomicValue(native->references)==1 &&
                GpuGather::CompatibleRigidGeometry(geometry,Field<void*>(geometry,0x138))) {
                ++g_stats.diagnosticCacheBypasses;return nullptr;
            }
            return g_cache(shader,descriptor);
        }
        // Admission precedes this native factory's wrapper AddRefs. A source
        // still uploading stays full/native until ready, while returning null
        // here keeps its passes on the verified live path. No GPU data is read
        // from a pending wrapper, and a later shared owner prevents publication.
        if (g_pilot && DetachCache(geometry)) {++g_stats.cacheBypasses;return nullptr;}
        native=geometry ? Field<NativeShape*>(geometry,0x148) : nullptr;
        if (native && !g_records.contains(native) && ShapeGeometry(geometry,native)) {
            try {Enroll(geometry,true);} catch (const std::bad_alloc&) {g_stats.healthy=false;}
        }
        if (native && g_records.contains(native)) {
            auto& record=g_records.at(native);
            if (g_pilot && (!PilotGeometry(geometry) || !ProvenOwnerGeometry(record,geometry)))
                InvalidateOwnerProof(native,"cache-owner-metadata-change");
            ++g_stats.cacheBypasses;return nullptr;
        }
        return g_cache(shader,descriptor);
    }
    bool KnownNativeConsumer(void* pass,Record& r) {
        auto* geometry=pass ? Field<void*>(pass,0x18) : nullptr;
        auto* shader=pass ? Field<void*>(pass,8) : nullptr;
        return shader && ShapeGeometry(geometry,r.native) && ProvenOwnerGeometry(r,geometry) &&
            std::find(g_nativeShaders.begin(),g_nativeShaders.end(),Field<std::uintptr_t>(shader,0))!=g_nativeShaders.end();
    }
    void InspectPass(void* pass) {
        auto* geometry=pass ? Field<void*>(pass,0x18) : nullptr;
        if (!geometry || Field<std::uintptr_t>(geometry,0)!=g_meshTable || Field<std::uint8_t>(geometry,0x158)!=5) return;
        auto* shader=Field<void*>(pass,8);
        const auto table=shader ? Field<std::uintptr_t>(shader,0) : 0;
        std::scoped_lock lock(g_lock);++g_stats.diagnosticPasses;
        const auto found=std::find(g_nativeShaders.begin(),g_nativeShaders.end(),table);
        if (found!=g_nativeShaders.end()) ++g_stats.diagnosticNativePasses[found-g_nativeShaders.begin()];
        else {
            ++g_stats.diagnosticUnknownPasses;
            if (table && std::find(g_unknownShaderTables.begin(),g_unknownShaderTables.end(),table)==g_unknownShaderTables.end())
                for (auto& saved:g_unknownShaderTables) if (!saved) {
                    saved=table;logger::warn("[fo4-mesh-pass-unknown] shaderVtable={:X} passDescriptor={:X} mutation=none",
                        table,Field<std::uint32_t>(pass,0x48));break;
                }
        }
    }
    struct FullSourcePass {
        NativeShape* native{};
        std::optional<ScopedNativePair> pair;
        NativeShape* previousOverride{};
        Pilot::FullPair* pilotScope{};
        Record* pilotRecord{};
        explicit FullSourcePass(void* pass) {
            ++g_stats.preSetupCalls;
            auto* geometry=pass ? Field<void*>(pass,0x18) : nullptr;
            if (g_pilot) TryPublishDetach(geometry);
            auto* shape=geometry ? Field<NativeShape*>(geometry,0x148) : nullptr;
            const auto found=g_records.find(shape);
            if (found==g_records.end()) return;
            auto& r=found->second;
            // An unknown consumer of a still-full or not-yet-verified record
            // must prevent future reduction too; absence of a current coarse
            // pair is not lifetime admission proof.
            if (g_pilot && (!PilotGeometry(geometry) || !KnownNativeConsumer(pass,r))) {
                ++g_stats.unsupportedConsumers;InvalidateOwnerProof(r.native,"pre-Setup-consumer-change");return;
            }
            if (!r.policy || !r.policy->Current() || !SamePair(r)) return;
            if (g_pilot && !Unique(r)) {RestorePilot(r,"pre-Setup-owner-change",true);return;}
            const bool full=r.forceFull || !Unique(r) || !GpuGather::CompatibleRigidPass(pass) ||
                (Field<std::uint8_t>(pass,0x4e)&0x80);
            if (!full) return;
            if (!KnownNativeConsumer(pass,r)) {
                r.pinned=r.forceFull=true;Cancel(r);++g_stats.unsupportedConsumers;
                // This unresolved admission boundary is why Install remains
                // held. A future unknown class cannot borrow one shared pair
                // safely: multiple retained consumers could outlive the draw.
                return;
            }
            if (g_pilot) {
                // Both native pairs stay owned for this diagnostic's lifetime.
                // Known native fallback uses the record's original full pair;
                // a genuinely unknown consumer permanently restores above.
                if (t_fullSourceOverride || !r.pilotPair.Restore()) {
                    RestorePilot(r,"nested-full-fallback",true);++g_stats.fallbackFailures;return;
                }
                native=shape;pilotScope=&r.pilotPair;pilotRecord=&r;
                previousOverride=std::exchange(t_fullSourceOverride,native);
                ++g_stats.preSetupFallbacks;return;
            }
            if (t_fullSourceOverride || !g_reserve ||
                FAILED(g_reserve->Prepare(DrawContext(),r.source.View(),r.descriptor))) {
                r.pinned=r.forceFull=true;Cancel(r);++g_stats.fallbackFailures;g_stats.healthy=false;return;
            }
            // Exact native rigid Lighting/PrePass/Utility/Effect paths do not
            // bind geometry VB/IB as SRVs and do not retain these wrapper
            // pointers. Keep the registry lock across Setup->Draw->Restore;
            // the real per-thread context records upload before native reads.
            auto* reserve=g_reserve->Shape();pair.emplace(*shape,*reserve);
            if (!*pair) {pair.reset();++g_stats.fallbackFailures;g_stats.healthy=false;return;}
            native=shape;
            previousOverride=std::exchange(t_fullSourceOverride,native);
            ++g_stats.preSetupFallbacks;
        }
        ~FullSourcePass() {
            if (native) {
                pair.reset();
                if (pilotScope && pilotRecord->policy->Current() && !pilotScope->UseReduced()) g_stats.healthy=false;
                t_fullSourceOverride=previousOverride;
            }
        }
        FullSourcePass(const FullSourcePass&)=delete;
        FullSourcePass& operator=(const FullSourcePass&)=delete;
    };
    void HookStandard(void* pass,bool alpha) {
        try {InspectPass(pass);} catch (const std::exception&) {} // diagnostics cannot suppress the native pass
        if (!g_installed.load()) {g_standard(pass,alpha);return;}
        std::scoped_lock lock(g_lock);FullSourcePass fallback(pass);g_standard(pass,alpha);
    }
    void HookSkinned(void* pass,bool alpha) {
        try {InspectPass(pass);} catch (const std::exception&) {}
        if (!g_installed.load()) {g_skinned(pass,alpha);return;}
        std::scoped_lock lock(g_lock);FullSourcePass fallback(pass);g_skinned(pass,alpha);
    }
    void HookCustom(void* pass) {
        try {InspectPass(pass);} catch (const std::exception&) {}
        if (!g_installed.load()) {g_custom(pass);return;}
        std::scoped_lock lock(g_lock);FullSourcePass fallback(pass);g_custom(pass);
    }
    std::uint32_t HookCount(void* geometry,std::uint32_t mode,void* pass) {
        const auto nativeCount=g_count(geometry,mode);
        t_geometry=geometry;t_pass=pass;t_selected=false;
        t_nativeRange={g_start(geometry,mode),nativeCount<=UINT32_MAX/3 ? nativeCount*3 : 0};
        std::scoped_lock lock(g_lock);
        ++g_stats.type5Draws;
        if (!ShapeGeometry(geometry,Field<NativeShape*>(geometry,0x148))) return nativeCount;
        auto* native=Field<NativeShape*>(geometry,0x148);
        if (native==t_fullSourceOverride) return nativeCount;
        auto found=g_records.find(native);
        Record* r=found==g_records.end() ? nullptr : &found->second;
        const bool safe=GpuGather::CompatibleRigidPass(pass);
        if (!r && safe && !(mode&0x80)) {
            try {r=Enroll(geometry);} catch(const std::bad_alloc&) {g_stats.healthy=false;}
        }
        if (!r || !r->policy) return nativeCount;
        const bool identityAndOwnership=Field<std::array<std::uint32_t,3>>(geometry,0x170)==r->segments &&
            Unique(*r) && ProvenOwnerGeometry(*r,geometry);
        if ((mode&0x80) || !identityAndOwnership) {
            r->pinned=r->forceFull=true;Cancel(*r);
            if (g_pilot) RestorePilot(*r,"count-owner-or-mode-change",true);
            return nativeCount;
        }
        if (!SamePair(*r)) {r->pinned=true;Cancel(*r);return nativeCount;}
        if (!safe) {
            // FullSourcePass needs no temporary swap while current==0. Known
            // Utility/Effect passes therefore arrive here without an override;
            // their unchanged full binding must not permanently pin demand.
            if (KeepFullNativePass(g_pilot.has_value(),KnownNativeConsumer(pass,*r),
                    identityAndOwnership,(mode&0x80)!=0,r->policy->Current())) return nativeCount;
            r->pinned=r->forceFull=true;Cancel(*r);
            if (g_pilot) RestorePilot(*r,"count-unsupported-consumer",true);
            return nativeCount;
        }
        if (!r->forceFull && r->policy->Current()) {
            t_selected=true;
            return std::min(t_nativeRange.count,r->policy->Levels().levels[r->policy->Current()].range.count)/3;
        }
        return nativeCount;
    }
    std::uint32_t HookStart(void* geometry,std::uint32_t mode) {
        return t_geometry==geometry && t_selected ? 0 : g_start(geometry,mode);
    }
    void PilotDrawReceipt(Record* r,NativeShape* native,std::uint32_t start,std::uint32_t triangles) {
        const auto pilotState=r && r->policy ? (native==t_fullSourceOverride ? 4u : r->policy->Current() ?
            (g_stats.pilotReducedReuses ? 3u : 1u) : (g_stats.pilotFullRestores>=2 ? 5u : g_stats.pilotFullRestores ? 2u : 0u)) : 0u;
        if (g_pilot && r && r->policy && !(r->pilotSeenDrawStates&(1u<<pilotState))) {
            // Native DrawTriShape synchronously binds these exact wrappers and
            // offsets. This is actual submitted geometry, not a planner count.
            logger::info("[fo4-mesh-pilot-draw] generation={} plannedLevel={} fullScope={} startIndex={} triangles={} vertexResource={:X} indexResource={:X} vertexOffset={} indexOffset={} vertexPayload={} indexPayload={} inactiveCapacity={} fullRestores={} reducedReuses={} "
                "vertexWrapper={:X} indexWrapper={:X} vertexCapacity={} indexCapacity={} nativeDrawReturned=true",
                r->generation,r->policy->Current(),native==t_fullSourceOverride,start,triangles,
                reinterpret_cast<std::uintptr_t>(native->vertices->buffer),reinterpret_cast<std::uintptr_t>(native->indices->buffer),
                native->vertices->offset,native->indices->offset,native->vertices->size,native->indices->size,r->pilotPair.InactiveCapacity(),
                g_stats.pilotFullRestores,g_stats.pilotReducedReuses,reinterpret_cast<std::uintptr_t>(native->vertices),
                reinterpret_cast<std::uintptr_t>(native->indices),native->vertices->capacity,native->indices->capacity);
            ++g_stats.pilotDrawReceipts;
            r->pilotSeenDrawStates|=static_cast<std::uint8_t>(1u<<pilotState);
        }
    }
    void HookDraw(void* renderer,NativeShape* native,std::uint32_t start,std::uint32_t triangles) {
        const bool specific=reinterpret_cast<std::uintptr_t>(_ReturnAddress())==g_type5Return;
        auto* geometry=specific ? t_geometry : nullptr;
        auto* pass=specific ? t_pass : nullptr;
        t_geometry=nullptr;t_pass=nullptr;t_selected=false;
        std::scoped_lock lock(g_lock);
        auto found=g_records.find(native);
        auto* r=found==g_records.end() ? nullptr : &found->second;
        if (r && r->policy && r->policy->Current() && SamePair(*r)) {
            const auto range=IndexRange{start,triangles<=UINT32_MAX/3 ? triangles*3 : 0};
            if (!geometry || r->forceFull || !Contains(r->policy->Levels().levels[r->policy->Current()].range,range)) {
                r->pinned=r->forceFull=true;Cancel(*r);
                if (g_pilot && RestorePilot(*r,"unexpected-native-draw",true)) {
                    g_draw(renderer,native,start,triangles);
                    PilotDrawReceipt(r,native,start,triangles);
                    ++g_stats.fallbackDraws;return;
                }
                // Exact verified rigid paths have no separate geometry-buffer
                // SRV bindings. The shared reserve is reuploaded on this draw's
                // own context before native IA bindings; it is never published.
                if (geometry && GpuGather::CompatibleRigidPass(pass) && g_reserve &&
                    SUCCEEDED(g_reserve->Prepare(DrawContext(),r->source.View(),r->descriptor))) {
                    g_draw(renderer,g_reserve->Shape(),start,triangles);++g_stats.fallbackDraws;return;
                }
                // Unknown shader/geometry use cannot safely consume a compact
                // pair. It must be excluded before enrollment by the native
                // dispatcher guard; never issue an out-of-range GPU draw.
                logger::critical("Mesh encountered an unsupported draw after enrollment; restoring full source is required");
                g_stats.healthy=false;
                return;
            }
            ++g_stats.compactedDraws;
        }
        g_draw(renderer,native,start,triangles);
        PilotDrawReceipt(r,native,start,triangles);
    }
    void OnDestroy(NativeShape* native) noexcept {
        NativeShape* candidate{};NativeShape* pilotOwned{};
        std::uint64_t pilotGeneration{},pilotTicket{},liveCapacity{},inactiveCapacity{};
        std::uintptr_t retiredPairIdentity{};
        // Capture scalars only while the zero-reference native object and its
        // wrappers are still borrowed. No pointer is dereferenced after native
        // release. These receipts describe ownership, never physical memory.
        try {
            std::scoped_lock lock(g_lock);
            InvalidateOwnerProof(native,"zero-ref-retirement");
            auto found=g_records.find(native);
            if (found==g_records.end()) return;
            auto& r=found->second;Cancel(r);
            if (g_pilot && r.detachTicket) {
                pilotGeneration=r.generation;pilotTicket=r.detachTicket;
                liveCapacity=std::uint64_t(native->vertices->capacity)+native->indices->capacity;
                inactiveCapacity=r.pilotPair.InactiveCapacity();
                retiredPairIdentity=reinterpret_cast<std::uintptr_t>(r.pilotPair.Owned());
                try {
                    logger::info("[fo4-mesh-pilot-retirement] event=owner-zero-ref generation={} detachTicket={} native={:X} "
                        "ownerRefs={} current={} liveCapacity={} inactiveCapacity={} retainedPair={:X} retainedRefs={} "
                        "candidatePresent={} readbackPresent={} pending={} noPhysicalFreeClaim=true",
                        pilotGeneration,pilotTicket,reinterpret_cast<std::uintptr_t>(native),AtomicValue(native->references),
                        r.policy ? r.policy->Current() : 0,liveCapacity,inactiveCapacity,retiredPairIdentity,
                        r.pilotPair.Owned() ? AtomicValue(r.pilotPair.Owned()->references) : 0,
                        r.candidate!=nullptr,r.candidateReadback.has_value(),r.policy && r.policy->Pending());
                } catch(const std::exception&) {}
            }
            if (r.detachTicket) g_stats.pilotDetachFullCapacity=0;
            candidate=r.candidate;
            if (candidate && r.candidateReadback) {
                // The pilot admits one record/candidate. Preserve native pool
                // ownership until this readback has completed, even when its
                // geometry is destroyed before publication.
                if (!g_orphanReadback) g_orphanReadback.emplace(OrphanReadback{candidate,std::move(*r.candidateReadback),r.candidateBytes,r.candidateQueryFailed});
                else g_stats.healthy=false; // retain candidate's native ref on this impossible second pilot orphan
                g_stats.retainedBytes+=r.candidateBytes;candidate=nullptr;
            }
            pilotOwned=r.pilotPair.TakeForOwnerDestruction();
            g_stats.pendingBufferBytes-=r.candidateBytes;
            g_stats.sourceBytes-=r.reservedBytes;
            if (r.readback) g_stats.stagingBytes-=r.readback->Bytes();
            g_records.erase(found);
        } catch (...) {
            // The native zero-ref callback must always return to native release.
            // Only plugin bookkeeping lives in this boundary, never engine code.
            g_stats.healthy=false;
        }
        // Native faults must propagate. No engine call belongs inside the
        // plugin bookkeeping catch, including this recursively hooked DecRef.
        if (candidate) g_release(g_renderer,candidate);
        if (pilotOwned) g_release(g_renderer,pilotOwned);
        if (pilotGeneration) {
            try {
                logger::info("[fo4-mesh-pilot-retirement] event=retained-release-returned generation={} detachTicket={} native={:X} "
                    "retainedPair={:X} releasedOwnedPair={} liveCapacity={} inactiveCapacity={} nativeCallReturned=true noPhysicalFreeClaim=true",
                    pilotGeneration,pilotTicket,reinterpret_cast<std::uintptr_t>(native),retiredPairIdentity,
                    pilotOwned!=nullptr,liveCapacity,inactiveCapacity);
            } catch(const std::exception&) {}
        }
    }
    void HookAddRef(void* renderer,NativeShape* native) {
        // A sampled refs==1 is insufficient while a clone can increment it on
        // another thread. Serialize the actual native increment with pair
        // publication; a new unattributed owner pins the record until a future
        // all-owner adapter can establish its final geometry type and ranges.
        std::scoped_lock lock(g_lock);
        CancelDetachForNative(native,"AddRef-entry-before-native");
        const bool matchedCopy=g_pilot && g_ownerProof.AddRef(reinterpret_cast<std::uint64_t>(native),
            AtomicValue(native->references),t_ownerCopy);
        if (matchedCopy) OwnerProofReceipt("matched-addref","inside-outer-CopyMembers",AtomicValue(native->references));
        else if (g_pilot && g_ownerProof.Get().native==reinterpret_cast<std::uint64_t>(native))
            OwnerProofReceipt("invalidated","unattributed-AddRef-before-native",AtomicValue(native->references));
        if (const auto found=g_records.find(native);found!=g_records.end()) {
            auto& r=found->second;++g_stats.trackedAddRefs;r.pinned=true;
            if (g_pilot) RestorePilot(r,"native-AddRef-before-owner-publication",true);
            if (r.policy && r.policy->Current()) r.forceFull=true;
            Cancel(r);
        }
        g_addRef(renderer,native);
    }
    void HookRelease(void* renderer,NativeShape* native) {
        // Invalidate before EVERY decrement, not just the zero-ref branch.
        // Do not dereference native after the original: it may have freed it.
        // Release the adapter lock while native queueing/pool teardown runs.
        struct ReferenceScope {
            ReferenceScope(NativeShape* shape) {
                std::scoped_lock lock(g_lock);++g_nonRenderingActive;
                InvalidateOwnerProof(shape,"DecRef-entry-before-native");
            }
            ~ReferenceScope() {std::scoped_lock lock(g_lock);--g_nonRenderingActive;}
        } scope(native);
        g_release(renderer,native);
    }
    void BeforeGeometryMutation(void* geometry,const char* reason) {
        if (!g_pilot || !geometry || Field<std::uintptr_t>(geometry,0)!=g_meshTable) return;
        std::scoped_lock lock(g_lock);
        if (g_detach && g_detach->ownership.Proof().destination==reinterpret_cast<std::uint64_t>(geometry))
            CancelDetach(reason);
        const auto native=Field<NativeShape*>(geometry,0x148);
        // The initial audited copy performs these base operations itself. Its
        // post-call validation verifies both currently borrowed objects. Once
        // certified, every later mutation invalidates before native use.
        if (!g_ownerProof.CopyIncludes(reinterpret_cast<std::uint64_t>(geometry),t_ownerCopy))
            InvalidateOwnerProof(native,reason);
        if (const auto found=g_records.find(native);found!=g_records.end()) RestorePilot(found->second,reason,true);
    }
    struct NonRenderingScope {
        NonRenderingScope(void* geometry,const char* reason,bool all=false) {
            std::scoped_lock lock(g_lock);++g_nonRenderingActive;
            if (all) {
                CancelDetach(reason);
                if (g_ownerProof.Invalidate(g_ownerProof.Get().native)) OwnerProofReceipt("invalidated",reason,0);
                for (auto& [native,r]:g_records) RestorePilot(r,reason,true);
            }
            else BeforeGeometryMutation(geometry,reason);
        }
        ~NonRenderingScope() {std::scoped_lock lock(g_lock);--g_nonRenderingActive;}
        NonRenderingScope(const NonRenderingScope&)=delete;
        NonRenderingScope& operator=(const NonRenderingScope&)=delete;
    };
    void HookAttachProperty(void* geometry,void* property) {
        // Native AttachProperty calls the NEW property's SetupGeometry before
        // publishing it into geometry+138. Restore before entering that call.
        NonRenderingScope scope(geometry,"native-AttachProperty-before-property-Setup");
        g_attachProperty(geometry,property);
    }
    void HookSetProperty(void* geometry,void* property) {
        NonRenderingScope scope(geometry,"native-SetProperty");g_setProperty(geometry,property);
    }
    void HookCopyMembers(void* source,void* destination,void* cloning) {
        // BSGeometry CopyMembers assigns/clones properties before the derived
        // base-copy renderer AddRef; source and old destination both matter.
        NonRenderingScope scope(source,"native-CopyMembers-source");
        BeforeGeometryMutation(destination,"native-CopyMembers-destination");
        g_copyMembers(source,destination,cloning);
    }
    void HookShapeCopyMembers(void* source,void* destination,void* cloning) {
        // This OUTER base-shape call includes renderer AddRef, old-destination
        // DecRef, SetRendererData and the base vertex/triangle count copies.
        // Mesh's three derived segment counts are verified on a later borrowed
        // admission. No stored geometry address is dereferenced later.
        NonRenderingScope scope(source,"outer-BSTriShape-CopyMembers");
        BeforeGeometryMutation(destination,"outer-BSTriShape-CopyMembers-destination");
        NativeShape* native{};NativeBuffer* vertices{};NativeBuffer* indices{};
        std::uint64_t ticket{};
        {
            std::scoped_lock lock(g_lock);
            native=source ? Field<NativeShape*>(source,0x148) : nullptr;
            const bool sourceAudited=g_pilot && !g_stats.enrolled && ShapeGeometry(source,native) && PilotGeometry(source) &&
                GpuGather::CompatibleRigidGeometry(source,Field<void*>(source,0x138));
            const bool destinationExact=destination && Field<std::uintptr_t>(destination,0)==g_meshTable &&
                Field<std::uint8_t>(destination,0x158)==5 && !Field<void*>(destination,0x140);
            if (sourceAudited && native->vertices && native->indices) {
                ticket=g_ownerProof.Begin(reinterpret_cast<std::uint64_t>(native),reinterpret_cast<std::uint64_t>(source),
                    reinterpret_cast<std::uint64_t>(destination),AtomicValue(native->references),true,destinationExact);
                if (ticket) {
                    vertices=native->vertices;indices=native->indices;
                    OwnerProofReceipt("copy-begin","unique-audited-source",AtomicValue(native->references));
                }
            }
        }
        struct CopyScope {
            std::uint64_t previous;
            explicit CopyScope(std::uint64_t ticket):previous(std::exchange(t_ownerCopy,ticket)) {}
            ~CopyScope() {t_ownerCopy=previous;}
        } copyScope(ticket);
        g_shapeCopyMembers(source,destination,cloning); // native calls run without a newly held adapter lock
        if (ticket) {
            std::scoped_lock lock(g_lock);
            const bool aliases=Field<NativeShape*>(source,0x148)==native && Field<NativeShape*>(destination,0x148)==native;
            const bool sourceAudited=aliases && ShapeGeometry(source,native) && PilotGeometry(source) &&
                GpuGather::CompatibleRigidGeometry(source,Field<void*>(source,0x138)) &&
                native->vertices==vertices && native->indices==indices;
            const bool destinationRigid=aliases && ShapeGeometry(destination,native) &&
                GpuGather::CompatibleRigidGeometry(destination,Field<void*>(destination,0x138)) &&
                Field<std::uint16_t>(destination,0x164)==Field<std::uint16_t>(source,0x164) &&
                Field<std::uint32_t>(destination,0x160)==Field<std::uint32_t>(source,0x160);
            const auto references=aliases ? AtomicValue(native->references) : 0;
            const bool complete=g_ownerProof.Complete(ticket,references,aliases,sourceAudited,destinationRigid);
            OwnerProofReceipt(complete ? "copy-certified" : "invalidated","outer-CopyMembers-return",references);
        }
    }
    void HookAddTriShape(void* combiner,void* geometry,bool flag,std::uint16_t start,std::uint32_t primitives,
                         std::uint32_t lod,void* leaf,std::uint32_t* index) {
        NonRenderingScope scope(geometry,"native-combiner-AddTriShape");
        g_addTriShape(combiner,geometry,flag,start,primitives,lod,leaf,index);
    }
    void* HookDoCombine(void* combiner,const char* name) {
        // A collection can have retained the fixture BEFORE admission. Its
        // actual CPU/GPU payload consumers both execute inside DoCombine.
        // Block new admission and restore the sole existing record, but release
        // g_lock while native work waits for resource/upload/Present progress.
        NonRenderingScope scope(nullptr,"native-combiner-DoCombine",true);
        return g_doCombine(combiner,name);
    }

    bool CallMatches(std::uintptr_t site,std::uintptr_t target) {
        return Field<std::uint8_t>(reinterpret_cast<void*>(site),0)==0xe8 &&
            site+5+Field<std::int32_t>(reinterpret_cast<void*>(site),1)==target;
    }
    std::uintptr_t Replay(std::uintptr_t address,std::span<const std::uint8_t> bytes) {
        auto* memory=static_cast<std::byte*>(g_trampoline.allocate(bytes.size()+14));
        std::memcpy(memory,bytes.data(),bytes.size());
        constexpr std::array<std::uint8_t,6> jump{0xff,0x25,0,0,0,0};
        std::memcpy(memory+bytes.size(),jump.data(),jump.size());
        const auto next=address+bytes.size();std::memcpy(memory+bytes.size()+6,&next,8);
        FlushInstructionCache(GetCurrentProcess(),memory,bytes.size()+14);
        return reinterpret_cast<std::uintptr_t>(memory);
    }
    bool InstallPassObservation(std::uintptr_t base) {
        if (g_passObserving.load()) return true;
        constexpr std::array<std::uint8_t,5> prologue{0x48,0x89,0x5c,0x24,0x08};
        const std::array<std::uintptr_t,3> entries{base+0x287fc70,base+0x287fd20,base+0x287fdc0};
        constexpr std::array<std::uint32_t,4> shaderIds{343833,1091304,417167,1203179};
        constexpr std::array<std::uintptr_t,4> setupOffsets{0x289dd10,0x2863c20,0x28bab50,0x2886600};
        constexpr std::array<std::array<std::uint8_t,12>,4> setupPrefixes{{
            {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55,0x56},
            {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55},
            {0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x53,0x56},
            {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55}}};
        for (std::size_t i=0;i<shaderIds.size();++i) {
            g_nativeShaders[i]=Addr::Resolve({shaderIds[i],0,0});
            if (!g_nativeShaders[i] || Field<std::uintptr_t>(reinterpret_cast<void*>(g_nativeShaders[i]),0x38)!=base+setupOffsets[i] ||
                std::memcmp(reinterpret_cast<void*>(base+setupOffsets[i]),setupPrefixes[i].data(),setupPrefixes[i].size())) return false;
        }
        for (auto entry:entries) if (std::memcmp(reinterpret_cast<void*>(entry),prologue.data(),prologue.size())) return false;
        const auto cache=base+0x28911f0;
        if (std::memcmp(reinterpret_cast<void*>(cache),prologue.data(),prologue.size())) return false;
        g_trampoline.create(1024);
        g_standard=reinterpret_cast<Immediate>(Replay(entries[0],prologue));
        g_skinned=reinterpret_cast<Immediate>(Replay(entries[1],prologue));
        g_custom=reinterpret_cast<CustomImmediate>(Replay(entries[2],prologue));
        g_cache=reinterpret_cast<Cache>(Replay(cache,prologue));
        g_trampoline.write_branch<5>(entries[0],HookStandard);
        g_trampoline.write_branch<5>(entries[1],HookSkinned);
        g_trampoline.write_branch<5>(entries[2],HookCustom);
        g_trampoline.write_branch<5>(cache,HookCache);
        g_stats.passObservationInstalled=true;g_passObserving.store(true);
        return true;
    }
    bool InstallPilotPropertyGuards(std::uintptr_t base) {
        constexpr std::array<std::uint8_t,5> attach{0x48,0x89,0x5c,0x24,0x10};
        constexpr std::array<std::uint8_t,10> set{0x48,0x8b,0xc1,0x48,0x8b,0x89,0x38,0x01,0,0};
        constexpr std::array<std::uint8_t,5> copy{0x48,0x89,0x6c,0x24,0x18};
        constexpr std::array<std::uint8_t,5> shapeCopy{0x48,0x89,0x5c,0x24,0x08};
        constexpr std::array<std::uint8_t,8> add{0x48,0x8b,0xc4,0x66,0x44,0x89,0x48,0x20};
        constexpr std::array<std::uint8_t,5> combine{0x48,0x89,0x74,0x24,0x18};
        const auto attachEntry=base+0x196590,setEntry=base+0x3aa920,copyEntry=base+0x1bb0890;
        const auto addEntry=base+0x283b090,combineEntry=base+0x283b6a0;
        const auto shapeCopyEntry=base+0x1bb6090;
        if (std::memcmp(reinterpret_cast<void*>(attachEntry),attach.data(),attach.size()) ||
            std::memcmp(reinterpret_cast<void*>(setEntry),set.data(),set.size()) ||
            std::memcmp(reinterpret_cast<void*>(copyEntry),copy.data(),copy.size()) ||
            std::memcmp(reinterpret_cast<void*>(shapeCopyEntry),shapeCopy.data(),shapeCopy.size()) ||
            std::memcmp(reinterpret_cast<void*>(addEntry),add.data(),add.size()) ||
            std::memcmp(reinterpret_cast<void*>(combineEntry),combine.data(),combine.size())) return false;
        g_attachProperty=reinterpret_cast<PropertyChange>(Replay(attachEntry,attach));
        g_setProperty=reinterpret_cast<PropertyChange>(Replay(setEntry,set));
        g_copyMembers=reinterpret_cast<CopyMembers>(Replay(copyEntry,copy));
        g_shapeCopyMembers=reinterpret_cast<CopyMembers>(Replay(shapeCopyEntry,shapeCopy));
        g_addTriShape=reinterpret_cast<AddTriShape>(Replay(addEntry,add));
        g_doCombine=reinterpret_cast<DoCombine>(Replay(combineEntry,combine));
        g_trampoline.write_branch<5>(attachEntry,HookAttachProperty);
        g_trampoline.write_branch<5>(setEntry,HookSetProperty);
        constexpr std::array<std::uint8_t,5> nops{0x90,0x90,0x90,0x90,0x90};
        REL::safe_write(setEntry+5,nops.data(),nops.size());
        g_trampoline.write_branch<5>(copyEntry,HookCopyMembers);
        g_trampoline.write_branch<5>(shapeCopyEntry,HookShapeCopyMembers);
        g_trampoline.write_branch<5>(addEntry,HookAddTriShape);
        REL::safe_write(addEntry+5,nops.data(),3);
        g_trampoline.write_branch<5>(combineEntry,HookDoCombine);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(setEntry),set.size());
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(addEntry),add.size());
        return true;
    }
    void DiscardCandidate(Record& r) {
        if (r.candidateReadback) {r.candidateAbandoned=true;return;}
        auto* candidate=std::exchange(r.candidate,nullptr);
        g_stats.pendingBufferBytes-=std::exchange(r.candidateBytes,0);
        r.candidateManual=false;
        if (candidate && Lifetime::Enabled() && !r.candidateLifetimeObserved) ++g_stats.lifetimeUnobservedCandidateDiscards;
        if (candidate) g_release(g_renderer,candidate);
    }
    bool InitializeDevice() {
        if (g_device) return g_pilot.has_value() || g_reserve.has_value();
        auto* device=*reinterpret_cast<ID3D11Device**>(g_deviceSlot);
        if (!device) return false;
        ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(context.GetAddressOf());
        if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        std::optional<DrawReserve> reserve;
        if (!g_pilot) {
            reserve=DrawReserve::Create(device,kMaximumVertices,kMaximumIndices);
            if (!reserve) {g_stats.healthy=false;return false;}
        }
        g_contextOwner=std::move(context);g_context=g_contextOwner.Get();g_device=device;
        g_reserve=std::move(reserve);
        logger::info("[fo4-mesh] reserveBytes={} sourceCap={} stagingCap={} retainedCap={}",
            g_reserve ? g_reserve->Bytes() : 0,kSourceCap,kStagingCap,kRetainedCap);
        return true;
    }
    void DetachReceipt(const char* event,const char* reason,bool nativeCallsReturned=false) noexcept {
        if (!g_detach || !g_pilot) return;
        const auto& d=*g_detach;const auto& proof=d.ownership.Proof();
        const bool published=d.ownership.GetStage()==Pilot::DetachOwnership::Stage::Published;
        const auto pair=d.candidateIdentity.value_or(PairIdentity{});
        try {
            logger::info("[fo4-mesh-pilot-detach] event={} reason={} ticket={} sourceOwnerCertificate={} sourceNative={:X} "
                "destinationGeometry={:X} newNative={:X} oldRefsBefore={} oldRefsAfter={} sourceHold=1 formerClone={} "
                "pluginOldRefs={} candidateRefs={} duplicateFullCapacity={} candidateToGeometry={} oldCloneRefTransferred={} "
                "nativeCallsReturned={} vertexWrapper={:X} indexWrapper={:X} vertexResource={:X} indexResource={:X} "
                "vertexOffset={} indexOffset={} vertexPayload={} indexPayload={} vertexCapacity={} indexCapacity={} fullPairReady={} "
                "bindingUnchangedAtPublication={} noPhysicalFreeClaim=true netMemorySavingClaim=false",
                event,reason,proof.certificate,proof.certificate,proof.native,proof.destination,d.newNative,d.refsBefore,d.refsAfter,
                published ? 1 : 0,d.ownership.SourceReferences(),d.candidateReferences,d.fullCapacity,published,published,nativeCallsReturned,
                reinterpret_cast<std::uintptr_t>(pair.vertices.wrapper),reinterpret_cast<std::uintptr_t>(pair.indices.wrapper),
                reinterpret_cast<std::uintptr_t>(pair.vertices.resource),reinterpret_cast<std::uintptr_t>(pair.indices.resource),
                pair.vertices.offset,pair.indices.offset,pair.vertices.size,pair.indices.size,pair.vertices.capacity,pair.indices.capacity,
                d.candidateVerified,published && d.candidateVerified);
        } catch(const std::exception&) {}
    }
    void DetachHashReceipt(const char* event) noexcept {
        if (!g_detach || !g_pilot) return;
        const auto& d=*g_detach;const auto& proof=d.ownership.Proof();
        const auto pair=d.candidateIdentity.value_or(PairIdentity{});
        try {
            logger::info("[fo4-mesh-pilot-detach] event={} ticket={} sourceOwnerCertificate={} sourceNative={:X} destinationGeometry={:X} "
                "newNative={:X} verticesSHA256={} indicesSHA256={} completedGPUReadback=true candidateRefs={} fullPairReady={} "
                "vertexWrapper={:X} indexWrapper={:X} vertexResource={:X} indexResource={:X} vertexOffset={} indexOffset={} "
                "vertexPayload={} indexPayload={} vertexCapacity={} indexCapacity={} "
                "noPhysicalFreeClaim=true netMemorySavingClaim=false",
                event,proof.certificate,proof.certificate,proof.native,proof.destination,d.newNative,
                Pilot::Hex(g_pilot->verticesDigest),Pilot::Hex(g_pilot->indicesDigest),d.candidateReferences,d.candidateVerified,
                reinterpret_cast<std::uintptr_t>(pair.vertices.wrapper),reinterpret_cast<std::uintptr_t>(pair.indices.wrapper),
                reinterpret_cast<std::uintptr_t>(pair.vertices.resource),reinterpret_cast<std::uintptr_t>(pair.indices.resource),
                pair.vertices.offset,pair.indices.offset,pair.vertices.size,pair.indices.size,pair.vertices.capacity,pair.indices.capacity);
        } catch(const std::exception&) {}
    }
    void CancelDetach(const char* reason) {
        if (g_detach && g_detach->ownership.Cancel()) DetachReceipt("cancelled",reason);
    }
    void CancelDetachForNative(NativeShape* native,const char* reason) {
        if (g_detach && g_detach->ownership.Proof().native==reinterpret_cast<std::uint64_t>(native)) CancelDetach(reason);
    }
    bool DetachSourceUnchanged(const DetachPending& d) {
        // The real plugin-held native reference keeps these objects and their
        // pool windows alive; this is not a borrowed geometry dereference.
        return d.ownership.SourceReferences() && SamePair(d.source) && AtomicValue(d.source.native->references)==3 &&
            ExclusiveWindowReady(d.source.native->vertices,std::uint64_t(d.source.vertices)*d.source.stride) &&
            ExclusiveWindowReady(d.source.native->indices,std::uint64_t(d.source.triangles)*6);
    }
    bool DetachCache(void* geometry) {
        if (!g_pilot || !kPilotDetachVerified || !geometry || !g_stats.healthy) return false;
        if (g_detach) {
            if (g_detach->ownership.Proof().destination!=reinterpret_cast<std::uint64_t>(geometry) || !g_detach->ownership.Pending()) return false;
            if (!ShapeGeometry(geometry,g_detach->source.native) || !PilotGeometry(geometry) ||
                !GpuGather::CompatibleRigidGeometry(geometry,Field<void*>(geometry,0x138))) {
                CancelDetach("cache-borrowed-identity-changed");return false;
            }
            TryPublishDetach(geometry);
            // A pending exact destination never acquires cached command refs.
            return g_detach->ownership.Pending() || g_detach->ownership.GetStage()==Pilot::DetachOwnership::Stage::Published;
        }
        auto* native=Field<NativeShape*>(geometry,0x148);
        if (g_stats.enrolled || g_nonRenderingActive || !ShapeGeometry(geometry,native) || !PilotGeometry(geometry) ||
            !GpuGather::CompatibleRigidGeometry(geometry,Field<void*>(geometry,0x138))) return false;
        const auto references=AtomicValue(native->references);
        if (g_ownerProof.ValidateDestination(reinterpret_cast<std::uint64_t>(native),reinterpret_cast<std::uint64_t>(geometry),references,true))
            OwnerProofReceipt("destination-validated","borrowed-final-Mesh-metadata",references);
        const auto proof=g_ownerProof.Get();
        if (!g_ownerProof.Certifies(reinterpret_cast<std::uint64_t>(native),references,proof.certificate) ||
            proof.destination!=reinterpret_cast<std::uint64_t>(geometry)) return false;
        const auto vertices=Field<std::uint16_t>(geometry,0x164);
        const auto stride=static_cast<std::uint32_t>(native->descriptor&15)*4;
        const auto triangles=Field<std::uint32_t>(geometry,0x160);
        const auto vertexBytes=std::uint64_t(vertices)*stride,indexBytes=std::uint64_t(triangles)*6;
        if (vertexBytes>kMaximumVertices || indexBytes>kMaximumIndices ||
            vertexBytes+indexBytes>kSourceCap-g_stats.sourceBytes ||
            !ExclusiveWindowReady(native->vertices,vertexBytes) || !ExclusiveWindowReady(native->indices,indexBytes)) return false;
        // Both verified central cache producers retain BOTH wrappers. refs1 at
        // this first interception excludes an existing native cached command;
        // every subsequent builder for this destination is returned nullptr.
        g_detach.emplace();auto& d=*g_detach;
        if (!d.ownership.Begin(proof,reinterpret_cast<std::uint64_t>(geometry),true)) {g_detach.reset();return false;}
        d.source.native=native;d.source.descriptor=native->descriptor;d.source.vertices=vertices;d.source.stride=stride;
        d.source.triangles=triangles;d.source.segments=Field<std::array<std::uint32_t,3>>(geometry,0x170);
        d.source.reservedBytes=vertexBytes+indexBytes;UpdatePair(d.source);
        d.source.preferPooled=d.source.vertexPooled || d.source.indexPooled;
        d.refsBefore=references;d.started=Clock::now();
        // A REAL native AddRef, attributed to this transaction. Do not run the
        // foreign-owner hook for our own hold; its exact balance is below.
        g_addRef(g_renderer,native);d.refsAfter=AtomicValue(native->references);
        g_ownerProof.Invalidate(reinterpret_cast<std::uint64_t>(native));
        OwnerProofReceipt("invalidated","explicit-plugin-source-hold",d.refsAfter);
        ++g_stats.pilotDetachTickets;DetachReceipt("source-held","first-cache-builder-no-prior-command");
        if (d.refsAfter!=3) CancelDetach("unexpected-count-after-source-hold");
        return d.ownership.Pending();
    }
    void TryPublishDetach(void* geometry) {
        if (!g_detach || !g_pilot || !kPilotDetachVerified || g_nonRenderingActive || !g_stats.healthy ||
            g_detach->ownership.GetStage()!=Pilot::DetachOwnership::Stage::Ready || !geometry) return;
        auto& d=*g_detach;
        if (d.ownership.Proof().destination!=reinterpret_cast<std::uint64_t>(geometry)) return;
        const bool borrowed=ShapeGeometry(geometry,d.source.native) && PilotGeometry(geometry) &&
            GpuGather::CompatibleRigidGeometry(geometry,Field<void*>(geometry,0x138)) && DetachSourceUnchanged(d);
        if (!d.ownership.CanPublish(reinterpret_cast<std::uint64_t>(geometry),reinterpret_cast<std::uint64_t>(d.source.native),
                reinterpret_cast<std::uint64_t>(d.full),borrowed) || !d.candidateVerified ||
            !d.full || !d.candidateIdentity || !d.candidateIdentity->Matches(*d.full) ||
            !ExclusiveWindowReady(d.full->vertices,std::uint64_t(d.source.vertices)*d.source.stride) ||
            !ExclusiveWindowReady(d.full->indices,std::uint64_t(d.source.triangles)*6) ||
            AtomicValue(d.full->references)!=1 || g_stats.enrolled) {CancelDetach("pre-publication-identity-changed");return;}
        try {
            // Allocate policy/map storage BEFORE publication. The new pair is
            // still plugin-owned, so any allocation failure leaves old native
            // geometry untouched. After the setter only noexcept bookkeeping
            // and caught diagnostics remain.
            auto levels=BuildPrefixLevels(d.source.source.View(),d.source.segments);
            auto bounds=GpuBounds::Decode(d.source.source.vertices,d.source.stride,d.source.descriptor);
            if (!levels || levels->count!=2 || !bounds) {CancelDetach("verified-source-layout-invalid");return;}
            Record r;r.native=d.full;r.generation=++g_generation;r.descriptor=d.source.descriptor;
            r.vertices=d.source.vertices;r.triangles=d.source.triangles;r.stride=d.source.stride;r.segments=d.source.segments;
            r.reservedBytes=d.source.reservedBytes;r.detachTicket=d.ownership.Proof().certificate;
            UpdatePair(r);r.initialAllocatedBytes=d.fullCapacity;r.preferPooled=r.vertexPooled || r.indexPooled;r.localBounds=*bounds;
            r.policy=std::make_unique<PolicyState>(dtl::TextureIdentity{reinterpret_cast<std::uint64_t>(r.native),r.generation},
                r.pairRevision,*levels,r.initialAllocatedBytes);
            r.source=std::move(d.source.source);
            const auto [inserted,ok]=g_records.emplace(d.full,std::move(r));
            if (!ok) {CancelDetach("new-native-already-tracked");return;}
            auto& live=inserted->second;
            // Reserve the exact-once transfer before the verified leaf setter.
            // It performs only pointer/descriptor stores and no callbacks;
            // g_lock excludes observers across these adjacent operations.
            if (!d.ownership.Published()) {g_records.erase(inserted);CancelDetach("transfer-state-invalid");return;}
            g_setRendererData(geometry,d.full);d.full=nullptr;
            g_stats.pendingBufferBytes-=d.fullCapacity;g_stats.pilotDetachFullCapacity=d.fullCapacity;
            ++g_stats.enrolled;++g_stats.verified;
            if (live.vertexPooled || live.indexPooled) ++g_stats.initialPoolSources;
            DetachReceipt("published","verified-full-copy-to-placed-clone");
            try {
                const auto position=Field<std::array<float,3>>(geometry,0xa0);
                logger::info("[fo4-mesh-pilot-owner] generation={} geometry={:X} native={:X} worldPosition=({},{},{}) initialRefs=1 maximumRecords=1 "
                    "ownerCertificate=0 provenOwners=1 detachTicket={}",live.generation,reinterpret_cast<std::uintptr_t>(geometry),
                    reinterpret_cast<std::uintptr_t>(live.native),position[0],position[1],position[2],live.detachTicket);
                logger::info("[fo4-mesh-pilot] sourceIdentity=true generation={} vertices={} triangles={} fullPayload={} reducedPayload={} "
                    "mode=detached-retained-full-pair netMemorySavingClaim=false",live.generation,live.vertices,live.triangles,
                    levels->levels[0].bytes,levels->levels[1].bytes);
            } catch(const std::exception&) {}
        } catch(const std::bad_alloc&) {CancelDetach("pre-publication-allocation-failed");}
    }
    void PumpDetach(Clock::time_point now) {
        if (!g_detach || !g_pilot || g_detach->queryFailed) return;
        auto& d=*g_detach;
        if (d.ownership.Pending() && (!DetachSourceUnchanged(d) || now-d.started>kTimeout)) CancelDetach("source-change-or-timeout");
        if (d.readback) {
            Data bytes;const auto result=d.readback->Poll(g_context,bytes);
            if (result==S_FALSE || result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (FAILED(result)) {d.queryFailed=true;CancelDetach("readback-failure-retaining-native-ownership");return;}
            const bool bindingMatches=!d.candidateReadback || (d.full && d.candidateIdentity && d.candidateIdentity->Matches(*d.full) &&
                AtomicValue(d.full->references)==1 &&
                d.readback->SourceVertices()==d.candidateIdentity->vertices.resource && d.readback->SourceIndices()==d.candidateIdentity->indices.resource &&
                d.readback->SourceVertexOffset()==d.candidateIdentity->vertices.offset && d.readback->SourceIndexOffset()==d.candidateIdentity->indices.offset &&
                ExclusiveWindowReady(d.full->vertices,std::uint64_t(d.source.vertices)*d.source.stride) &&
                ExclusiveWindowReady(d.full->indices,std::uint64_t(d.source.triangles)*6));
            g_stats.stagingBytes-=d.readback->Bytes();d.readback.reset();
            if (!bindingMatches) {CancelDetach("full-copy-binding-changed-during-readback");return;}
            if (!d.ownership.Pending()) return;
            if (!g_pilot->Source(bytes.View())) {CancelDetach("GPU-source-hash-mismatch");return;}
            if (!d.candidateReadback) {
                if (!d.ownership.SourceVerified(true)) return;
                d.source.source=std::move(bytes);g_stats.sourceBytes+=d.source.reservedBytes;d.sourceVerified=true;
                DetachHashReceipt("source-verified");
            } else {
                if (!d.ownership.CandidateVerified(true,true)) return;
                d.candidateReferences=AtomicValue(d.full->references);
                d.candidateVerified=true;DetachHashReceipt("candidate-verified");return;
            }
        }
        if (!d.ownership.Pending()) return;
        using Stage=Pilot::DetachOwnership::Stage;
        if (d.ownership.GetStage()==Stage::SourceHeld) {
            d.readback=Readback::BeginRegions(g_device,g_context,
                {d.source.expectedVertexResource,d.source.vertexOffset,d.source.vertexCapacity,d.source.vertexPooled},
                {d.source.expectedIndexResource,d.source.indexOffset,d.source.indexCapacity,d.source.indexPooled},
                d.source.stride,d.source.vertices,d.source.triangles*3,kStagingCap-g_stats.stagingBytes);
            if (!d.readback) {CancelDetach("source-readback-allocation-failed");return;}
            g_stats.stagingBytes+=d.readback->Bytes();return;
        }
        if (d.ownership.GetStage()==Stage::SourceVerified) {
            auto* tls=ThreadData();
            if (!tls || DrawContext()!=g_context || Field<std::uint32_t>(tls,0xb28)!=0 || tls[0x1534]!=std::byte{0}) return;
            struct NativePolicy {std::byte& field;std::byte previous;~NativePolicy(){field=previous;}} policy{tls[0x1534],tls[0x1534]};
            tls[0x1534]=d.source.preferPooled ? std::byte{0} : std::byte{1};
            // With TLS+B28==0, native CreateVertexBuffer consumes this byte
            // count and writes0 after enqueueing its own CPU copy. A nonzero
            // remainder describes the excluded split-stream path, not success.
            // Evidence: fo4-og-draw-aabb-05.txt, CreateVertexBuffer141D0D530.
            auto remainingVertexBytes=static_cast<std::uint32_t>(d.source.source.vertices.size());
            d.full=g_create(g_renderer,&remainingVertexBytes,d.source.source.vertices.data(),d.source.descriptor,
                d.source.source.indices.data(),static_cast<std::uint32_t>(d.source.source.indices.size()));
            if (!d.full) {CancelDetach("native-full-copy-allocation-failed");return;}
            d.newNative=reinterpret_cast<std::uint64_t>(d.full);d.candidateReferences=AtomicValue(d.full->references);
            if (!d.ownership.OwnCandidate(d.newNative,d.candidateReferences)) {CancelDetach("native-full-copy-ownership-invalid");return;}
            d.fullCapacity=d.source.reservedBytes;g_stats.pendingBufferBytes+=d.fullCapacity;
            if (remainingVertexBytes!=0 || d.full->descriptor!=d.source.descriptor || !d.full->vertices || !d.full->indices) {
                CancelDetach("native-full-copy-layout-invalid");return;
            }
            return;
        }
        if (d.ownership.GetStage()==Stage::CandidateOwned) {
            if (AtomicValue(d.full->references)!=1) {CancelDetach("candidate-native-ownership-changed");return;}
            if (!ExclusiveWindowReady(d.full->vertices,std::uint64_t(d.source.vertices)*d.source.stride) ||
                !ExclusiveWindowReady(d.full->indices,std::uint64_t(d.source.triangles)*6)) return;
            const auto capacity=std::uint64_t(d.full->vertices->capacity)+d.full->indices->capacity;
            g_stats.pendingBufferBytes-=d.fullCapacity;d.fullCapacity=capacity;g_stats.pendingBufferBytes+=capacity;
            if (!d.candidateLifetimeObserved) {
                if (Lifetime::Enabled()) {
                    Lifetime::Attach(d.full->vertices->buffer,Lifetime::Role::Candidate);
                    Lifetime::Attach(d.full->indices->buffer,Lifetime::Role::Candidate);
                }
                d.candidateLifetimeObserved=true;
            }
            d.candidateIdentity=PairIdentity::Capture(*d.full);
            d.readback=Readback::BeginRegions(g_device,g_context,
                {d.full->vertices->buffer,d.full->vertices->offset,d.full->vertices->capacity,d.full->vertices->suballocated},
                {d.full->indices->buffer,d.full->indices->offset,d.full->indices->capacity,d.full->indices->suballocated},
                d.source.stride,d.source.vertices,d.source.triangles*3,kStagingCap-g_stats.stagingBytes);
            if (!d.readback) {CancelDetach("full-copy-readback-allocation-failed");return;}
            d.candidateReadback=true;g_stats.stagingBytes+=d.readback->Bytes();
        }
    }
    void DrainDetachReferences() {
        NativeShape* source{};NativeShape* candidate{};std::uint8_t references{};
        {
            std::scoped_lock lock(g_lock);
            if (!g_detach || !g_detach->ownership.Finished() || g_detach->readback || g_detach->queryFailed) return;
            auto& d=*g_detach;
            references=d.ownership.TakeSourceReferences();
            if (references) source=d.source.native;
            if (d.ownership.GetStage()==Pilot::DetachOwnership::Stage::Cancelled) {
                d.ownership.TakeCandidate();candidate=std::exchange(d.full,nullptr);
                if (candidate) {
                    g_stats.pendingBufferBytes-=d.fullCapacity;d.fullCapacity=0;
                    if (Lifetime::Enabled() && !d.candidateLifetimeObserved) ++g_stats.lifetimeUnobservedCandidateDiscards;
                }
                if (d.sourceVerified) {g_stats.sourceBytes-=d.source.reservedBytes;d.sourceVerified=false;d.source.source={};}
            }
        }
        // These are exactly the references transferred into plugin ownership;
        // after publication the new geometry-owned pair is never released here.
        if (candidate) g_release(g_renderer,candidate);
        for (std::uint8_t i=0;i<references;++i) g_release(g_renderer,source);
        if (references) {std::scoped_lock lock(g_lock);DetachReceipt("old-references-released","plugin-owned-reference-balance",true);}
    }
    void DrainOrphanReadback() {
        NativeShape* release{};
        {
            std::scoped_lock lock(g_lock);
            if (!g_orphanReadback || g_orphanReadback->failed || !g_context) return;
            Data unused;const auto result=g_orphanReadback->readback.Poll(g_context,unused);
            if (result==S_FALSE || result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (FAILED(result)) {g_orphanReadback->failed=true;return;}
            g_stats.stagingBytes-=g_orphanReadback->readback.Bytes();g_stats.retainedBytes-=g_orphanReadback->bytes;
            release=g_orphanReadback->native;g_orphanReadback.reset();
        }
        if (release) g_release(g_renderer,release);
    }
    void PollAbandonedCandidate(Record& r) {
        if (!r.candidateReadback || !r.candidateAbandoned || r.candidateQueryFailed) return;
        Data unused;const auto result=r.candidateReadback->Poll(g_context,unused);
        if (result==S_FALSE || result==DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(result)) {r.candidateQueryFailed=true;return;}
        g_stats.stagingBytes-=r.candidateReadback->Bytes();r.candidateReadback.reset();DiscardCandidate(r);
    }
    void PollSource(Record& r,Clock::time_point now) {
        if (r.policy || r.pinned) return;
        if (r.awaitingNativeReady) {
            if (r.native->descriptor!=r.descriptor || r.native->vertices!=r.expectedVertices ||
                r.native->indices!=r.expectedIndices || !ShapeOwnership(r)) {
                r.pinned=true;return;
            }
            if (!ExclusiveWindowReady(r.native->vertices,std::uint64_t(r.vertices)*r.stride) ||
                !ExclusiveWindowReady(r.native->indices,std::uint64_t(r.triangles)*6)) {
                if (now-r.copyAt>kTimeout) {r.pinned=true;++g_stats.sourceRejected;}
                return;
            }
            UpdatePair(r);r.initialAllocatedBytes=std::uint64_t(r.vertexCapacity)+r.indexCapacity;
            r.preferPooled=r.vertexPooled || r.indexPooled;
            r.awaitingNativeReady=false;
            if (r.vertexPooled || r.indexPooled) ++g_stats.initialPoolSources;
        }
        if (!SamePair(r) || !Unique(r)) {
            if (r.readback) {g_stats.stagingBytes-=r.readback->Bytes();r.readback.reset();}
            r.pinned=true;return;
        }
        if (!r.readback) {
            if (!g_stats.healthy) return;
            const auto bytes=AllocationBytes(std::uint64_t(r.vertices)*r.stride)+AllocationBytes(std::uint64_t(r.triangles)*6);
            if (bytes>kStagingCap-g_stats.stagingBytes) return;
            r.readback=Readback::BeginRegions(g_device,g_context,
                {r.expectedVertexResource,r.vertexOffset,r.vertexCapacity,r.vertexPooled},
                {r.expectedIndexResource,r.indexOffset,r.indexCapacity,r.indexPooled},
                r.stride,r.vertices,r.triangles*3);
            if (!r.readback) {r.pinned=true;++g_stats.sourceRejected;return;}
            g_stats.stagingBytes+=r.readback->Bytes();r.copyAt=now;return;
        }
        Data data;
        const auto result=r.readback->Poll(g_context,data);
        if ((result==S_FALSE || result==DXGI_ERROR_WAS_STILL_DRAWING) && now-r.copyAt<kTimeout) return;
        g_stats.stagingBytes-=r.readback->Bytes();r.readback.reset();
        if (result!=S_OK) {
            r.pinned=true;++g_stats.sourceRejected;
            if (result!=E_OUTOFMEMORY) {++g_stats.queryFailures;g_stats.healthy=false;}
            return;
        }
        auto bounds=GpuBounds::Decode(data.vertices,data.stride,r.descriptor);
        auto levels=BuildPrefixLevels(data.View(),r.segments);
        if (!bounds || !levels || levels->count<2) {r.pinned=true;++g_stats.sourceRejected;return;}
        if (g_pilot && (levels->count!=2 || !g_pilot->Source(data.View()))) {
            r.pinned=true;++g_stats.sourceRejected;
            logger::error("[fo4-mesh-pilot] sourceIdentity=false mutation=none");return;
        }
        auto policy=std::make_unique<PolicyState>(dtl::TextureIdentity{reinterpret_cast<std::uint64_t>(r.native),r.generation},
            r.pairRevision,*levels,r.initialAllocatedBytes);
        r.localBounds=*bounds;r.source=std::move(data);r.policy=std::move(policy);++g_stats.verified;
        if (g_pilot) logger::info("[fo4-mesh-pilot] sourceIdentity=true generation={} vertices={} triangles={} fullPayload={} reducedPayload={} mode=retained-full-pair netMemorySavingClaim=false",
            r.generation,r.vertices,r.triangles,levels->levels[0].bytes,levels->levels[1].bytes);
    }
    bool RestorePilot(Record& r,const char* reason,bool pin) {
        if (!g_pilot) return false;
        if (pin) {r.pinned=true;Cancel(r);}
        if (!r.policy) return false;
        if (!r.policy->Current()) {r.forceFull=false;return true;}
        if (!r.pilotPair.FullIsLive() && (!SamePair(r) || !r.pilotPair.Restore())) {
            r.pinned=r.forceFull=true;g_stats.healthy=false;return false;
        }
        const auto bytes=std::uint64_t(r.native->vertices->capacity)+r.native->indices->capacity;
        UpdatePair(r);++r.pairRevision;
        if (r.policy->Pending() && r.policy->Target()==0) {
            r.policy->Complete(r.pairRevision,g_ledger,bytes);++g_stats.completed;
        } else r.policy->FullSourcePublished(r.pairRevision,g_ledger,bytes);
        r.forceFull=false;++g_stats.restores;++g_stats.pilotFullRestores;
        try {
            logger::info("[fo4-mesh-pilot-change] generation={} level=0 reason={} liveCapacity={} inactiveCapacity={} sourcePairRestored=true netMemorySavingClaim=false",
                r.generation,reason,bytes,r.pilotPair.InactiveCapacity());
        } catch(const std::exception&) {} // Diagnostics must not suppress the native consumer.
        return true;
    }
    bool ReusePilotReduced(Record& r) {
        if (!g_pilot || !r.pilotPair.Owned() || r.policy->Current() || r.policy->Target()!=1 || !r.pilotPair.UseReduced()) return false;
        const auto bytes=std::uint64_t(r.native->vertices->capacity)+r.native->indices->capacity;
        UpdatePair(r);++r.pairRevision;r.policy->Complete(r.pairRevision,g_ledger,bytes);
        ++g_stats.completed;++g_stats.compactions;++g_stats.pilotReducedReuses;
        logger::info("[fo4-mesh-pilot-change] generation={} level=1 reason=reuse-retained-ready-pair liveCapacity={} inactiveCapacity={} allocation=false netMemorySavingClaim=false",
            r.generation,bytes,r.pilotPair.InactiveCapacity());
        return true;
    }
    bool StartCandidate(Record& r,std::uint8_t target,bool manual,Clock::time_point now) {
        const auto& level=r.policy->Levels().levels[target];
        const auto held=g_stats.retainedBytes+g_stats.pendingBufferBytes;
        if (held>kRetainedCap || level.bytes>kRetainedCap-held) return false;
        auto* tls=ThreadData();
        if (!tls || DrawContext()!=g_context || Field<std::uint32_t>(tls,0xb28)!=0 || tls[0x1534]!=std::byte{0}) {
            ++g_stats.creationThreadRejected;return false;
        }
        std::optional<Data> reduced;
        if (target) {
            reduced=CompactPreservingVertexIDs(r.source.View(),level.range);
            if (!reduced) return false;
        }
        auto& data=target ? *reduced : r.source;
        struct AllocationPolicy {
            std::byte& field;std::byte previous;
            AllocationPolicy(std::byte& value,bool pooled):field(value),previous(value) {field=pooled ? std::byte{0} : std::byte{1};}
            ~AllocationPolicy() {field=previous;}
        } allocationPolicy(tls[0x1534],r.preferPooled);
        auto remainingVertexBytes=static_cast<std::uint32_t>(data.vertices.size());
        // Native AddBufferToCreate owns a synchronous CPU copy before return.
        // The engine services its queue later; never wait/Flush in Present.
        auto* candidate=g_create(g_renderer,&remainingVertexBytes,data.vertices.data(),r.descriptor,data.indices.data(),
            static_cast<std::uint32_t>(data.indices.size()));
        if (!candidate) return false;
        // The native unsplit path acknowledges consumption by writing0.
        if (remainingVertexBytes!=0 || candidate->descriptor!=r.descriptor ||
            !candidate->vertices || !candidate->indices || AtomicValue(candidate->references)!=1) {
            g_release(g_renderer,candidate);return false;
        }
        r.candidate=candidate;r.candidateBytes=level.bytes;r.candidateLevel=target;
        r.candidateLifetimeObserved=false;
        r.candidateVerified=r.candidateAbandoned=r.candidateQueryFailed=false;
        r.candidateIdentity.reset();
        r.candidateManual=manual;r.candidateAt=now;g_stats.pendingBufferBytes+=level.bytes;
        ++g_stats.candidateSubmitted;
        return true;
    }
    bool PublishCandidate(Record& r,Clock::time_point now) {
        const auto target=r.candidateLevel;
        const auto& desired=r.policy->Levels().levels[target];
        const auto oldBytes=r.policy->AllocatedBytes();
        if (AtomicValue(r.candidate->references)!=1 ||
            !ExclusiveWindowReady(r.candidate->vertices,std::uint64_t(desired.vertices)*r.stride) ||
            !ExclusiveWindowReady(r.candidate->indices,std::uint64_t(desired.range.count)*2)) return false;
        if (!r.candidateLifetimeObserved) {
            if (Lifetime::Enabled()) {
                Lifetime::Attach(r.candidate->vertices->buffer,Lifetime::Role::Candidate);
                Lifetime::Attach(r.candidate->indices->buffer,Lifetime::Role::Candidate);
            }
            r.candidateLifetimeObserved=true;
        }
        if (g_pilot && !r.candidateVerified) {
            if (!r.candidateReadback) {
                r.candidateIdentity=PairIdentity::Capture(*r.candidate);
                r.candidateReadback=Readback::BeginRegions(g_device,g_context,
                    {r.candidate->vertices->buffer,r.candidate->vertices->offset,r.candidate->vertices->capacity,r.candidate->vertices->suballocated},
                    {r.candidate->indices->buffer,r.candidate->indices->offset,r.candidate->indices->capacity,r.candidate->indices->suballocated},
                    r.stride,desired.vertices,desired.range.count,kStagingCap-g_stats.stagingBytes);
                if (!r.candidateReadback) {r.pinned=true;Cancel(r);DiscardCandidate(r);return false;}
                g_stats.stagingBytes+=r.candidateReadback->Bytes();return false;
            }
            Data actual;const auto result=r.candidateReadback->Poll(g_context,actual);
            if (result==S_FALSE || result==DXGI_ERROR_WAS_STILL_DRAWING) return false;
            if (FAILED(result)) {r.candidateQueryFailed=true;r.candidateAbandoned=true;r.pinned=true;Cancel(r);return false;}
            const bool bindingMatches=r.candidateIdentity && r.candidateIdentity->Matches(*r.candidate) &&
                AtomicValue(r.candidate->references)==1 &&
                r.candidateReadback->SourceVertices()==r.candidateIdentity->vertices.resource &&
                r.candidateReadback->SourceIndices()==r.candidateIdentity->indices.resource &&
                r.candidateReadback->SourceVertexOffset()==r.candidateIdentity->vertices.offset &&
                r.candidateReadback->SourceIndexOffset()==r.candidateIdentity->indices.offset &&
                ExclusiveWindowReady(r.candidate->vertices,std::uint64_t(desired.vertices)*r.stride) &&
                ExclusiveWindowReady(r.candidate->indices,std::uint64_t(desired.range.count)*2);
            g_stats.stagingBytes-=r.candidateReadback->Bytes();r.candidateReadback.reset();
            if (!bindingMatches) {r.pinned=true;Cancel(r);DiscardCandidate(r);return false;}
            const auto expected=CompactPreservingVertexIDs(r.source.View(),desired.range);
            if (!expected || actual.stride!=expected->stride || actual.vertices!=expected->vertices || actual.indices!=expected->indices) {
                r.pinned=true;Cancel(r);DiscardCandidate(r);return false;
            }
            const auto vertexHash=Pilot::Hash(actual.vertices),indexHash=Pilot::Hash(std::as_bytes(std::span(actual.indices)));
            if (!vertexHash || !indexHash) {r.pinned=true;Cancel(r);DiscardCandidate(r);return false;}
            r.candidateVerified=true;
            logger::info("[fo4-mesh-pilot] reducedCandidateIdentity=true generation={} detachTicket={} target={} native={:X} "
                "vertexResource={:X} indexResource={:X} vertexOffset={} indexOffset={} vertexPayload={} indexPayload={} verticesSHA256={} indicesSHA256={} "
                "completedGPUReadback=true reducedPayload={}",r.generation,r.detachTicket,target,reinterpret_cast<std::uintptr_t>(r.candidate),
                reinterpret_cast<std::uintptr_t>(r.candidate->vertices->buffer),reinterpret_cast<std::uintptr_t>(r.candidate->indices->buffer),
                r.candidate->vertices->offset,r.candidate->indices->offset,actual.vertices.size(),actual.indices.size()*2,Pilot::Hex(*vertexHash),Pilot::Hex(*indexHash),
                actual.vertices.size()+actual.indices.size()*2);
        }
        const auto actualBytes=std::uint64_t(r.candidate->vertices->capacity)+r.candidate->indices->capacity;
        g_stats.pendingBufferBytes-=r.candidateBytes;r.candidateBytes=actualBytes;g_stats.pendingBufferBytes+=actualBytes;
        if (actualBytes>kRetainedCap || g_stats.pendingBufferBytes+g_stats.retainedBytes>kRetainedCap ||
            (target>r.policy->Current() && actualBytes>=oldBytes)) {
            // Allocator block slack can erase a theoretical payload saving.
            // Do not publish a degradation that saves no logical capacity.
            ++g_stats.allocationRejected;r.pinned=true;Cancel(r);DiscardCandidate(r);return false;
        }
        const auto otherCandidates=g_stats.pendingBufferBytes-r.candidateBytes;
        if (g_stats.retainedBytes+otherCandidates>kRetainedCap ||
            oldBytes>kRetainedCap-g_stats.retainedBytes-otherCandidates) return false;
        if (g_pilot) {
            if (target!=1 || r.policy->Current() || r.pilotPair.Owned() || !r.candidateVerified ||
                AtomicValue(r.candidate->references)!=1 ||
                !r.candidateIdentity || !r.candidateIdentity->Matches(*r.candidate) ||
                !ExclusiveWindowReady(r.candidate->vertices,std::uint64_t(desired.vertices)*r.stride) ||
                !ExclusiveWindowReady(r.candidate->indices,std::uint64_t(desired.range.count)*2)) return false;
            // The candidate's native shape becomes the owner of the original
            // full pair. Never release it or credit it as a completed saving.
            std::swap(r.native->vertices,r.candidate->vertices);
            std::swap(r.native->indices,r.candidate->indices);
            if (!r.pilotPair.Retain(*r.native,*r.candidate)) {
                std::swap(r.native->vertices,r.candidate->vertices);
                std::swap(r.native->indices,r.candidate->indices);return false;
            }
            r.candidate=nullptr;g_stats.pendingBufferBytes-=std::exchange(r.candidateBytes,0);
            r.candidateManual=false;UpdatePair(r);++r.pairRevision;
            r.policy->Complete(r.pairRevision,g_ledger,actualBytes);++g_stats.completed;++g_stats.compactions;
            logger::info("[fo4-mesh-pilot-change] generation={} level=1 reason=first-native-pair-publication liveCapacity={} inactiveCapacity={} originalFullPairRetained=true netMemorySavingClaim=false",
                r.generation,actualBytes,r.pilotPair.InactiveCapacity());
            return true;
        }
        auto completion=Completion(g_device);
        if (!completion) {Cancel(r);DiscardCandidate(r);g_stats.healthy=false;return false;}
        // Allocate ownership storage before changing either native object.
        // On bad_alloc both engine pairs and the logical request stay intact.
        g_retired.push_back({r.candidate,completion,oldBytes,now,false});
        const auto manual=r.candidateManual;
        std::swap(r.native->vertices,r.candidate->vertices);
        std::swap(r.native->indices,r.candidate->indices);
        r.candidate=nullptr;g_stats.pendingBufferBytes-=std::exchange(r.candidateBytes,0);
        r.candidateManual=false;g_stats.retainedBytes+=oldBytes;UpdatePair(r);
        ++r.pairRevision;
        if (manual) r.policy->FullSourcePublished(r.pairRevision,g_ledger,actualBytes);
        else r.policy->Complete(r.pairRevision,g_ledger,actualBytes);
        g_context->End(completion.Get());
        if (!manual) ++g_stats.completed;
        HeapHooks::SetReclaimEnabled(true);
        if (target) ++g_stats.compactions;else {++g_stats.restores;r.forceFull=false;}
        return true;
    }
}

bool Install() {
    if (g_installed.load()) return true;
    const auto& settings=dtl::GetSettings();
    if (!settings.meshStreaming || settings.dryRun || settings.policy!=1) return false;
    if (REL::Module::get().version()!=REL::Version{1,10,163,0}) {
        logger::warn("FO4 mesh adapter unavailable: only OG1.10.163 native ownership and draw ABI is verified");return false;
    }
    const auto base=REL::Module::get().base();
    g_meshTable=Addr::Resolve({403494,0,0});
    if (g_meshTable!=base+0x309bb68) return false;
    std::array<wchar_t,32768> pilotPath{};
    const auto pilotLength=GetEnvironmentVariableW(Pilot::kEnvironment,pilotPath.data(),static_cast<DWORD>(pilotPath.size()));
    if (pilotLength) {
        if (pilotLength>=pilotPath.size()) return false;
        std::array<wchar_t,32768> executable{};
        const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if (!length || length>=executable.size()) return false;
        try {g_pilot=Pilot::Load(pilotPath.data(),std::filesystem::path(executable.data()).parent_path());}
        catch(const std::exception&) {g_pilot.reset();}
        const auto auditedFixture=Pilot::ParseDigest(L"68e334ed878ffdb2edb5c3a148b5d12146ef0e2cb22da9e023ff72ec5cfc20e8");
        if (!g_pilot || g_pilot->fixtureDigest!=*auditedFixture) {
            g_pilot.reset();logger::error("FO4 mesh pilot manifest/fixture identity invalid or not the audited four-block closure; no mesh hooks installed");return false;
        }
        logger::warn("[fo4-mesh-pilot] mode=SingleFixtureRetainedFull manifestSHA256={} fixtureSHA256={} geometry={} maximumRecords=1 retainedFullNativePair=true netMemorySavingClaim=false performanceEligible=false vanillaActivation=false",
            Pilot::Hex(g_pilot->manifestDigest),Pilot::Hex(g_pilot->fixtureDigest),g_pilot->geometryName);
    }
    g_inspecting.store(true);
    if (!InstallPassObservation(base)) {
        logger::warn("FO4 mesh native pass observation unavailable: shader/entry identity differs; borrowed main-view diagnostics only");
        return false;
    }
    if (!kAllConsumerAdmissionVerified && !g_pilot) {
        logger::warn("FO4 mesh mutation held: unknown future consumer/owner admission remains unverified; bounded1024 native cache bypasses and pre-Setup observations enabled; no buffer mutation or mesh planning; performance-ineligible diagnostics");
        return false;
    }
    if (g_pilot && !kPilotNonRenderingConsumersVerified) {
        logger::warn("[fo4-mesh-pilot] installed=false reason=non-rendering-consumer-boundary-unverified mutation=none");
        return false;
    }
    const auto draw=base+0x1d0bfb0,countSite=base+0x287f177,startSite=base+0x287f186;
    const auto releaseEntry=base+0x1d0c5d0,retireSite=base+0x1d0c5eb,addRef=base+0x1d0c5c0;
    constexpr std::array<std::uint8_t,5> prologue{0x48,0x89,0x5c,0x24,0x08};
    constexpr std::array<std::uint8_t,8> retireBytes{0x48,0x8b,0x52,0x10,0xf0,0xff,0x4a,0x38};
    constexpr std::array<std::uint8_t,5> addRefBytes{0xf0,0xff,0x42,0x18,0xc3};
    g_deviceSlot=Addr::Resolve(Addr::D3DDevice);
    if (!g_deviceSlot || std::memcmp(reinterpret_cast<void*>(draw),prologue.data(),5) ||
        std::memcmp(reinterpret_cast<void*>(releaseEntry),prologue.data(),prologue.size()) ||
        std::memcmp(reinterpret_cast<void*>(retireSite),retireBytes.data(),8) ||
        std::memcmp(reinterpret_cast<void*>(addRef),addRefBytes.data(),addRefBytes.size()) ||
        !CallMatches(countSite,base+0x28bd270) || !CallMatches(startSite,base+0x28bd160) ||
        !CallMatches(base+0x287f1a7,draw) ||
        Addr::Resolve({403494,0,0})!=base+0x309bb68 ||
        Addr::Resolve({99624,0,0})!=base+0x1d0bc70 ||
        Addr::Resolve({1039714,0,0})!=base+0x1d0c5d0) {
        logger::error("FO4 mesh instruction/address identity differs; no hooks installed");return false;
    }
    g_meshTable=base+0x309bb68;g_type5Return=base+0x287f1ac;
    constexpr std::array<std::uint8_t,23> setRendererBytes{
        0x48,0x89,0x91,0x48,0x01,0,0,0x48,0x85,0xd2,0x74,0x0a,0x48,0x8b,0x02,0x48,0x89,0x81,0x50,0x01,0,0,0xf3};
    if (g_pilot && (std::memcmp(reinterpret_cast<void*>(base+0x1bb15e0),setRendererBytes.data(),setRendererBytes.size()) ||
        Field<std::uint8_t>(reinterpret_cast<void*>(base+0x1bb15e0),23)!=0xc3)) return false;
    g_setRendererData=reinterpret_cast<SetRendererData>(base+0x1bb15e0);
    g_tlsIndex=reinterpret_cast<std::uint32_t*>(base+0x67347b4);g_renderer=reinterpret_cast<void*>(base+0x61e0900);
    g_count=reinterpret_cast<Count>(base+0x28bd270);g_start=reinterpret_cast<Count>(base+0x28bd160);
    g_create=reinterpret_cast<Create>(base+0x1d0bc70);g_release=reinterpret_cast<Release>(base+0x1d0c5d0);
    if (!HeapHooks::Install(OnNativeFrame)) {
        logger::error("FO4 mesh native frame/allocator lifetime identity differs; no mesh hooks installed");return false;
    }
    if (g_pilot && !InstallPilotPropertyGuards(base)) {
        logger::error("FO4 mesh pilot property/clone boundary identity differs; mutation remains disabled");return false;
    }
    g_draw=reinterpret_cast<Draw>(Replay(draw,prologue));
    g_addRef=reinterpret_cast<Release>(Replay(addRef,addRefBytes));
    if (g_pilot) g_release=reinterpret_cast<Release>(Replay(releaseEntry,prologue));
    const auto countCode=MakeCountStub(reinterpret_cast<std::uintptr_t>(&HookCount));
    auto* countMemory=static_cast<std::byte*>(g_trampoline.allocate(countCode.size()));
    std::memcpy(countMemory,countCode.data(),countCode.size());
    auto* retireStorage=static_cast<std::byte*>(g_trampoline.allocate(sizeof(RetireStub)+3));
    auto* retireCode=reinterpret_cast<RetireStub*>((reinterpret_cast<std::uintptr_t>(retireStorage)+3)&~std::uintptr_t(3));
    *retireCode=MakeRetireStub(reinterpret_cast<std::uintptr_t>(&OnDestroy),retireSite+8);
    g_retireFunction={0,44,44};
    if (!RtlAddFunctionTable(&g_retireFunction,1,reinterpret_cast<DWORD64>(retireCode))) return false;
    const auto displacement=reinterpret_cast<std::intptr_t>(retireCode)-static_cast<std::intptr_t>(retireSite+5);
    if (displacement<INT32_MIN || displacement>INT32_MAX) {RtlDeleteFunctionTable(&g_retireFunction);return false;}
    std::array<std::uint8_t,8> retirePatch{0xe9,0,0,0,0,0x90,0x90,0x90};
    const auto relative=static_cast<std::int32_t>(displacement);std::memcpy(retirePatch.data()+1,&relative,4);
    FlushInstructionCache(GetCurrentProcess(),countMemory,countCode.size());
    FlushInstructionCache(GetCurrentProcess(),retireCode,sizeof(RetireStub));
    g_trampoline.write_call<5>(countSite,reinterpret_cast<std::uintptr_t>(countMemory));
    g_trampoline.write_call<5>(startSite,HookStart);
    g_trampoline.write_branch<5>(draw,HookDraw);
    g_trampoline.write_branch<5>(addRef,HookAddRef);
    if (g_pilot) g_trampoline.write_branch<5>(releaseEntry,HookRelease);
    REL::safe_write(retireSite,retirePatch.data(),retirePatch.size());
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(retireSite),retirePatch.size());
    g_stats.installed=g_stats.healthy=true;g_installed.store(true);
    logger::warn("Experimental FO4 mesh adapter enabled: OG type5 rigid owning prefix buffers; cached creation guard; source Core mesh stages; physical destruction unmeasured");
    return true;
}

void Observe(GpuGather::Draw& draw,std::uint64_t round) {
    if (!draw.geometry || !draw.eyeCount) return;
    if (!g_installed.load()) {if (g_inspecting.load()) Inspect(draw);return;}
    std::scoped_lock lock(g_lock);
    auto* native=Field<NativeShape*>(draw.geometry,0x148);
    const auto found=g_records.find(native);
    if (found==g_records.end()) return;
    auto& r=found->second;
    if (g_pilot && !ProvenOwnerGeometry(r,draw.geometry)) {
        InvalidateOwnerProof(native,"gather-owner-identity-change");return;
    }
    if (!r.policy || !ShapeGeometry(draw.geometry,native) || !SamePair(r)) return;
    const auto transform=Field<std::array<float,16>>(draw.geometry,0x70);
    const auto bounds=GpuBounds::Transform(r.localBounds,transform.data(),transform.data()+12,transform[15]);
    if (!bounds) {r.pinned=r.forceFull=true;Cancel(r);return;}
    draw.worldBounds=*bounds;
    const auto eyeCount=std::min<std::size_t>(draw.eyeCount,draw.eyes.size());
    for (std::size_t eye=0;eye<eyeCount;++eye) {
        float distanceSquared=0,half=0;
        for (unsigned axis=0;axis<3;++axis) {
            const float delta=bounds->center[axis]-draw.eyes[eye].position[axis];
            distanceSquared+=delta*delta;half=std::max(half,bounds->halfExtent[axis]);
        }
        r.policy->Observe(round,dtl::StarfieldAngularSize(half,std::sqrt(distanceSquared)));
    }
}

std::vector<dtl::PlanEntry> Gather(std::uint64_t round) {
    std::vector<dtl::PlanEntry> entries;
    if (!g_installed.load()) return entries;
    std::scoped_lock lock(g_lock);g_round=round;entries.reserve(g_records.size());
    for (auto& [native,r]:g_records) if (r.policy) {
        const bool samePair=SamePair(r),unique=Unique(r);
        // A mismatched binding is diagnostic only and is not sent to Core.
        const auto entry=r.policy->Snapshot(round,Thresholds(),r.pinned || r.forceFull || !g_stats.healthy || !unique || !samePair);
        if (g_pilot && r.pilotPolicyReceipts<32) {
            const std::array<std::uint64_t,12> state{entry.current,entry.wanted,entry.lowest,entry.seen,
                entry.pending,entry.noDegrade,r.pinned,r.forceFull,unique,samePair,g_stats.healthy,r.pairRevision};
            if (!r.pilotPolicyReceipts || state!=r.pilotPolicyState) {
                r.pilotPolicyState=state;++r.pilotPolicyReceipts;
                try {
                    logger::info("[fo4-mesh-pilot-policy] generation={} detachTicket={} diagnostic={} round={} "
                        "rank={} seen={} wanted={} current={} lowest={} pending={} noDegrade={} pinned={} forceFull={} "
                        "unique={} samePair={} healthy={} pairRevision={} activeBytes={} fullBytes={} lowestBytes={} "
                        "fullSourceHalfExtent=({},{},{})",
                        r.generation,r.detachTicket,r.pilotPolicyReceipts,round,entry.priority,entry.seen,entry.wanted,
                        entry.current,entry.lowest,entry.pending,entry.noDegrade,r.pinned,r.forceFull,unique,samePair,
                        g_stats.healthy,r.pairRevision,entry.bytes[entry.current],entry.bytes[0],entry.bytes[entry.lowest],
                        r.localBounds.halfExtent[0],r.localBounds.halfExtent[1],r.localBounds.halfExtent[2]);
                } catch(const std::exception&) {}
            }
        }
        if (samePair) entries.push_back(entry);
    }
    return entries;
}
dtl::RequestReceipt Accept(void*,const dtl::PlanRequest& request) {
    if (!g_installed.load()) return {};
    std::scoped_lock lock(g_lock);
    const auto found=g_records.find(reinterpret_cast<NativeShape*>(request.identity.key));
    if (found==g_records.end()) return {dtl::RequestStatus::Stale,0};
    auto& r=found->second;
    if (!g_stats.healthy || g_nonRenderingActive || !r.policy || r.candidate || !SamePair(r) || !Unique(r) || r.forceFull ||
        (r.pinned && request.target>r.policy->Current())) return {dtl::RequestStatus::Stale,0};
    const auto receipt=r.policy->Accept(request,g_ledger,g_round,Thresholds());
    if (receipt.status==dtl::RequestStatus::AcceptedPending) ++g_stats.accepted;
    return receipt;
}
void OnPresent(std::uint64_t frame) {
    if (!g_installed.load()) return;
    std::scoped_lock lock(g_lock);g_frame=frame;
}
namespace {
void OnNativeFrame(void*,ID3D11DeviceContext* context) {
    if (!g_installed.load()) return;
    {std::scoped_lock lock(g_lock);if (g_context && context!=g_context) {++g_stats.creationThreadRejected;return;}}
    DrainDetachReferences();
    DrainOrphanReadback();
    std::scoped_lock lock(g_lock);
    if (g_nonRenderingActive) return;
    if (!InitializeDevice()) return;
    if (context!=g_context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        ++g_stats.creationThreadRejected;return;
    }
    ++g_stats.nativeFramePumps;
    const auto now=Clock::now();
    try {PumpDetach(now);} catch(const std::bad_alloc&) {CancelDetach("detach-allocation-failed");}
    for (auto it=g_retired.begin();it!=g_retired.end();) {
        if (it->failed) {++it;continue;}
        const auto result=PollCompletion(g_context,it->completion.Get());
        if (result==S_OK) {
            auto* native=it->native;g_stats.retainedBytes-=it->bytes;
            if (native->vertices->suballocated) {
                HeapHooks::MarkMeshPoolRelease(native->vertices->buffer);
                g_stats.logicalPoolBytesReleased+=native->vertices->capacity;
            }
            if (native->indices->suballocated) {
                HeapHooks::MarkMeshPoolRelease(native->indices->buffer);
                g_stats.logicalPoolBytesReleased+=native->indices->capacity;
            }
            g_stats.completedRetirementBytes+=it->bytes;++g_stats.nativePairReleases;
            it=g_retired.erase(it);g_release(g_renderer,native);
        } else {
            if (FAILED(result) || now-it->submitted>kTimeout) {
                it->failed=true;++g_stats.queryFailures;g_stats.healthy=false;
                // Keep the old native pair owned on query failure/timeout.
            }
            ++it;
        }
    }
    bool created=false;
    for (auto& [native,r]:g_records) {
        PollAbandonedCandidate(r);
        if (r.candidateAbandoned && r.candidateReadback) continue;
        try {PollSource(r,now);} catch(const std::bad_alloc&) {
            if (r.readback) {g_stats.stagingBytes-=r.readback->Bytes();r.readback.reset();}
            r.pinned=true;Cancel(r);
        }
        if (!r.policy) continue;
        if (!SamePair(r)) {r.pinned=true;Cancel(r);DiscardCandidate(r);continue;}
        if (g_pilot && r.policy->Current() && (r.forceFull || !g_stats.healthy || !Unique(r))) {
            RestorePilot(r,"frame-health-or-ownership-change",true);DiscardCandidate(r);continue;
        }
        const bool pending=r.policy->Pending();
        const bool fresh=r.policy->Publishable(g_round,Thresholds(),{g_stats.healthy,Unique(r),r.pinned,r.forceFull});
        if (pending && !fresh) Cancel(r);
        if (r.candidate) {
            if (!g_stats.healthy || (!r.candidateManual && !fresh) || now-r.candidateAt>kTimeout) {
                if (now-r.candidateAt>kTimeout) {
                    ++g_stats.candidateTimeouts;g_stats.healthy=false;
                    if (r.policy->Current()) r.forceFull=true;
                }
                Cancel(r);DiscardCandidate(r);continue;
            }
            try {PublishCandidate(r,now);} catch(const std::bad_alloc&) {Cancel(r);DiscardCandidate(r);}
            continue;
        }
        if (g_pilot && fresh && r.policy->Target()==0) {
            RestorePilot(r,"source-planner-upgrade",false);continue;
        }
        if (g_pilot && fresh && r.pilotPair.Owned()) {
            if (!ReusePilotReduced(r)) {r.pinned=true;Cancel(r);}
            continue;
        }
        const bool manual=r.forceFull && r.policy->Current()!=0;
        if ((!manual && !fresh) || created || !g_stats.healthy || !Unique(r)) continue;
        try {
            created=StartCandidate(r,manual ? 0 : r.policy->Target(),manual,now);
            if (!created) {
                Cancel(r);
                if (!manual) {r.pinned=true;if (r.policy->Current()) r.forceFull=true;}
            }
        } catch(const std::bad_alloc&) {
            Cancel(r);r.pinned=true;if (r.policy->Current()) r.forceFull=true;
        }
    }
}
}
Stats Statistics() {
    std::scoped_lock lock(g_lock);auto result=g_stats;
    result.pending=g_ledger.Snapshot().pendingOperations;result.reducedAllocationBytes=0;
    result.pilot=g_pilot.has_value();result.pilotInactiveCapacity=result.pilotFullFallbackCapacity=result.pilotActiveBindingReduction=0;
    result.pilotLiveRecords=g_pilot ? g_records.size() : 0;
    result.pilotDetachSourceReferences=g_detach ? g_detach->ownership.SourceReferences() : 0;
    for (const auto& [native,r]:g_records) if (r.policy && SamePair(r)) {
        if (g_pilot) {
            result.pilotInactiveCapacity+=r.pilotPair.InactiveCapacity();
            result.pilotFullFallbackCapacity+=r.pilotPair.FullCapacity();
            if (r.initialAllocatedBytes>r.policy->AllocatedBytes())
                result.pilotActiveBindingReduction+=r.initialAllocatedBytes-r.policy->AllocatedBytes();
        } else if (r.initialAllocatedBytes>r.policy->AllocatedBytes())
                result.reducedAllocationBytes+=r.initialAllocatedBytes-r.policy->AllocatedBytes();
    }
    return result;
}
dtl::StreamingAccounting LedgerSnapshot() {return g_ledger.Snapshot();}
void LogSummary() {
    if (!g_installed.load() && !g_inspecting.load()) return;
    const auto s=Statistics();
    if (s.pilot) logger::info("[fo4-mesh-pilot] installed={} inactiveNativeCapacity={} retainedFullFallbackCapacity={} activeBindingReduction={} fullRestores={} reducedReuses={} "
        "duplicateFullCapacity={} detachSourceReferences={} detachTickets={} liveRecords={} netMemorySavingClaim=false performanceEligible=false vanillaActivation=false",
        s.installed,s.pilotInactiveCapacity,s.pilotFullFallbackCapacity,s.pilotActiveBindingReduction,s.pilotFullRestores,s.pilotReducedReuses,
        s.pilotDetachFullCapacity,s.pilotDetachSourceReferences,s.pilotDetachTickets,s.pilotLiveRecords);
    if (Lifetime::Enabled()) logger::info("[fo4-mesh-lifetime-coverage] candidateDiscardsBeforeObservation={} completeNativeBirthCoverage=false physicalDestroyedBytes=unknown",
        s.lifetimeUnobservedCandidateDiscards);
    logger::info("[fo4-mesh-pass-diagnostic] installed={} type5Passes={} lighting={} prePass={} utility={} effect={} unknown={} scope=native-pre-Setup-all-views resourceMutation={}",
        s.passObservationInstalled,s.diagnosticPasses,s.diagnosticNativePasses[0],s.diagnosticNativePasses[1],
        s.diagnosticNativePasses[2],s.diagnosticNativePasses[3],s.diagnosticUnknownPasses,s.installed);
    if (!s.installed) logger::info("[fo4-mesh-cache-diagnostic] bypasses={} maximum=1024 exactRigidType5Only=true bufferMutation=false performanceEligible=false",
        s.diagnosticCacheBypasses);
    if (!s.installed) {
        logger::info("[fo4-mesh-diagnostic] installed=false submittedType5Draws={} pooledDraws={} sharedShapeDraws={} owningReadyDraws={} effectDataDraws={} exclusiveWindowReadyDraws={} scope=main-view-gather-subset mutation=none",
            s.diagnosticDraws,s.diagnosticPooled,s.diagnosticSharedShapes,s.diagnosticOwningReady,s.diagnosticEffectData,s.diagnosticExclusiveReady);
        return;
    }
    logger::info("[fo4-mesh-tl] healthy={} type5Draws={} owningRejected={} cacheBypasses={} enrolled={} sourceVerified={} sourceRejected={} "
        "plannerAccepted={} plannerCompleted={} plannerCancelled={} plannerPending={} compactions={} restores={} compactedDraws={} fallbackDraws={} "
        "sourceBytes={} stagingBytes={} candidateBytes={} retainedBytes={} reducedAllocationBytes={} nativePairReleases={} completedRetirementBytes={} queryFailures={} "
        "candidateSubmitted={} candidateTimeouts={} creationThreadRejected={} nativeFramePumps={} initialPoolSources={} logicalPoolBytesReleaseQueued={} trackedAddRefs={} "
        "preSetupCalls={} preSetupFallbacks={} unsupportedConsumers={} fallbackFailures={} allocationRejected={} physicalDestroyedBytes=unknown",
        s.healthy,s.type5Draws,s.owningRejected,s.cacheBypasses,s.enrolled,s.verified,s.sourceRejected,
        s.accepted,s.completed,s.cancelled,s.pending,s.compactions,s.restores,s.compactedDraws,s.fallbackDraws,
        s.sourceBytes,s.stagingBytes,s.pendingBufferBytes,s.retainedBytes,s.reducedAllocationBytes,s.nativePairReleases,s.completedRetirementBytes,s.queryFailures,
        s.candidateSubmitted,s.candidateTimeouts,s.creationThreadRejected,s.nativeFramePumps,s.initialPoolSources,s.logicalPoolBytesReleased,s.trackedAddRefs,
        s.preSetupCalls,s.preSetupFallbacks,s.unsupportedConsumers,s.fallbackFailures,s.allocationRejected);
    const auto heaps=HeapHooks::GetStatistics();
    logger::info("[fo4-mesh-pool] detachedHeaps={} releasedReferences={} releasedReferenceBytes={} eligibleEmptyHeaps={} "
        "retiringSkipped={} untrackedSkipped={} unsafePhaseSkipped={} scope=mesh-touched-fully-empty-heaps physicalDestroyedBytes=unknown",
        heaps.detachedHeaps,heaps.releasedReferences,heaps.releasedReferenceBytes,heaps.eligibleEmptyHeaps,
        heaps.retiringSkipped,heaps.untrackedSkipped,heaps.unsafePhaseSkipped);
}
}
