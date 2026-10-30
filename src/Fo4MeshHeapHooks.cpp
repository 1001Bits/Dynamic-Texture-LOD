#include "Fo4MeshHeapHooks.h"
#include "Fo4MeshHeap.h"
#include "Fo4MeshNative.h"
#include "Fo4MeshLifetime.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <span>

namespace Fo4Mesh::HeapHooks {
namespace {
    using Allocate=bool(*)(void*,NativeBuffer*);
    using Free=void(*)(void*,NativeBuffer*);
    using Constructor=void*(*)(void*);
    using Destructor=void(*)(void*);
    using FrameUpdate=void(*)(void*,ID3D11DeviceContext*);
    Allocate originalAllocate{};Free originalFree{};
    Constructor originalConstructor{};Destructor originalCreatorDestructor{};
    FrameUpdate originalFrameUpdate{};
    FrameCallback callback{};
    F4SE::Trampoline trampoline{"DynamicTextureLOD.Fo4MeshHeap"};
    std::mutex endpointLock;
    std::condition_variable callbacksDrained;
    std::atomic<bool> installed{},reclaimEnabled{};
    std::atomic<std::uint64_t> framePumps{},detachedHeaps{},releasedReferences{},releasedReferenceBytes{};
    std::atomic<std::uint64_t> eligibleEmptyHeaps{},retiringSkipped{},untrackedSkipped{},unsafePhaseSkipped{};
    std::atomic<std::uint64_t> lifetimeHeapObservations{},lifetimeHeapInterfaceFailures{};
    volatile std::uint32_t* presentBarrier{};
    ID3D11DeviceContext** nativeImmediate{};
    HeapReleaseQueue queue;
    struct Owner {
        void* allocator{};
        bool retiring{};
        std::uint64_t epoch{},activeCallbacks{};
        std::array<IUnknown*,8> lastBacking{};
        std::array<std::uint64_t,8> emptySince{};
        std::array<bool,8> meshTouched{};
        // Comparison-only identities; never AddRef/dereference these cached
        // addresses. The current native heap pointer is the owned source.
        std::array<IUnknown*,8> lifetimeBacking{};
    };
    // Native construction owns this bounded registration. No map allocation
    // can fail inside an engine resource-creator callback; excess owners are
    // simply not reclaimed.
    std::array<Owner,8> owners{};
    std::uint64_t nextOwnerEpoch{};
    template<class T> T Field(const void* object,std::size_t offset) {
        T result;std::memcpy(&result,static_cast<const std::byte*>(object)+offset,sizeof(result));return result;
    }
    Owner* Find(void* allocator) {
        for (auto& owner:owners) if (owner.allocator==allocator) return &owner;
        return nullptr;
    }
    void ObserveHeapBackings(Owner& owner) noexcept {
        // Caller holds endpointLock plus constructor/worker/callback lifetime.
        // An old owner epoch is never sampled after creator shutdown begins.
        // These are backing-pointer transition observations, not an exhaustive
        // birth hook: release/recreate/address reuse inside one original call
        // can remain invisible. Complete native birth coverage stays false.
        if (!Lifetime::Enabled() || owner.retiring) return;
        auto* heaps=static_cast<NativeHeap*>(owner.allocator);
        for (std::size_t i=0;i<8;++i) {
            auto* backing=heaps[i].backing;
            if (!backing) {owner.lifetimeBacking[i]=nullptr;continue;}
            if (backing==owner.lifetimeBacking[i]) continue;
            owner.lifetimeBacking[i]=backing;++lifetimeHeapObservations;
            // AllocateD3DBuffer writes a D3D buffer into heap+120. Query the
            // supported interface while that heap still owns its COM ref;
            // do not cast an asynchronously remembered address to a buffer.
            ID3D11Buffer* buffer{};
            const auto result=backing->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&buffer));
            if (FAILED(result) || !buffer) {++lifetimeHeapInterfaceFailures;continue;}
            (void)Lifetime::Attach(buffer,Lifetime::Role::NativeHeap,owner.epoch);
            buffer->Release();
        }
    }
    bool HookAllocate(void* allocator,NativeBuffer* buffer) {
        // This lock excludes both native endpoint operations and our backing
        // transfer. Present must never acquire it: native allocation may wait
        // for PresentBarrier while it holds the lock.
        std::scoped_lock lock(endpointLock);
        const auto result=originalAllocate(allocator,buffer);
        // Even an unsuccessful allocation can leave a newly created backing.
        if (auto* owner=Find(allocator)) ObserveHeapBackings(*owner);
        return result;
    }
    void HookFree(void* allocator,NativeBuffer* buffer) {
        std::scoped_lock lock(endpointLock);
        originalFree(allocator,buffer);
    }
    void* HookConstructor(void* allocator) {
        // Constructor eagerly creates its first backing and can wait for the
        // Present barrier. Do not hold endpointLock over that native wait.
        auto* result=originalConstructor(allocator);
        std::scoped_lock lock(endpointLock);
        auto* slot=Find(allocator);
        if (!slot) for (auto& owner:owners) if (!owner.allocator || (owner.retiring && !owner.activeCallbacks)) {slot=&owner;break;}
        if (slot && !slot->activeCallbacks) {
            *slot={};slot->allocator=allocator;slot->epoch=++nextOwnerEpoch;
            ObserveHeapBackings(*slot);
        }
        return result;
    }
    void HookCreatorDestructor(void* creator) {
        const auto allocator=Field<void*>(creator,0x99120);
        {
            std::unique_lock lock(endpointLock);
            if (auto* owner=Find(allocator)) {
                const auto epoch=owner->epoch;owner->retiring=true;
                // Existing plugin frame work must finish before native worker
                // shutdown starts. wait releases endpointLock, so the worker
                // can still service native requests made by that callback.
                callbacksDrained.wait(lock,[&] {return owner->epoch!=epoch || !owner->activeCallbacks;});
            }
        }
        // This destructor joins its worker BEFORE inlined allocator teardown.
        // The worker still enters HookAllocate/HookFree during that join, so
        // holding endpointLock while invoking it would deadlock shutdown.
        originalCreatorDestructor(creator);
    }
    void HookFrameUpdate(void* creator,ID3D11DeviceContext* context) {
        // Native Renderer::End is the only writer of PresentBarrier. This
        // Begin callsite is normally outside it; verify that invariant and
        // the exact native immediate context before acquiring endpointLock.
        // Acquiring the allocator lock during Present could deadlock against
        // the native worker's AllocateD3DBuffer wait for this same barrier.
        if (!creator || !context || *presentBarrier || context!=*nativeImmediate ||
            context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            ++unsafePhaseSkipped;originalFrameUpdate(creator,context);return;
        }
        struct CallbackLease {
            Owner* owner{};std::uint64_t epoch{};
            ~CallbackLease() {
                if (!owner) return;
                std::scoped_lock lock(endpointLock);
                if (owner->epoch==epoch && owner->activeCallbacks) --owner->activeCallbacks;
                callbacksDrained.notify_all();
            }
        } lease;
        {
            std::scoped_lock lock(endpointLock);
            auto* allocator=Field<void*>(creator,0x99120);
            auto* owner=allocator ? Find(allocator) : nullptr;
            if (!owner) ++untrackedSkipped;
            else if (owner->retiring) ++retiringSkipped;
            else {lease.owner=owner;lease.epoch=owner->epoch;++owner->activeCallbacks;}
        }
        // Acquire the lease at entry, before native FrameUpdate returns: an
        // added post-call gap must not let creator shutdown pass us before we
        // reserve the right to execute plugin queue work. The native callback
        // is always forwarded, including untracked/retiring observation cases.
        originalFrameUpdate(creator,context);
        if (!lease.owner || *presentBarrier) return;
        const auto frame=framePumps.fetch_add(1,std::memory_order_relaxed)+1;
        {
            std::scoped_lock lock(endpointLock);
            auto* owner=lease.owner;
            if (owner->retiring) return;
            // Also covers first observation after a late diagnostic enable.
            // Such an observation is not labelled the resource birth time.
            ObserveHeapBackings(*owner);
            if (reclaimEnabled.load(std::memory_order_acquire)) {
                auto* heaps=static_cast<NativeHeap*>(owner->allocator);
                for (std::size_t i=0;i<8;++i) {
                    auto& heap=heaps[i];
                    if (!owner->meshTouched[i]) continue;
                    if (!CompletelyEmpty(heap)) {owner->emptySince[i]=0;owner->lastBacking[i]=heap.backing;continue;}
                    ++eligibleEmptyHeaps;
                    if (!owner->emptySince[i] || owner->lastBacking[i]!=heap.backing) {
                        owner->emptySince[i]=frame;owner->lastBacking[i]=heap.backing;continue;
                    }
                    // Avoid destroying a briefly empty scratch heap between
                    // native operations. This is measured in rendered Begin
                    // callbacks, not wall time, and is independent of a claim
                    // about actual post-release DXGI residency.
                    if (frame-owner->emptySince[i]>=120 && queue.Detach(heap)) {
                        owner->emptySince[i]=0;owner->lastBacking[i]=nullptr;owner->meshTouched[i]=false;
                        owner->lifetimeBacking[i]=nullptr;
                    }
                }
                queue.Drain();
                const auto stats=queue.Stats();
                detachedHeaps.store(stats.detached,std::memory_order_relaxed);
                releasedReferences.store(stats.releasedReferences,std::memory_order_relaxed);
                releasedReferenceBytes.store(stats.releasedReferenceBytes,std::memory_order_relaxed);
            }
        }
        // No allocator lock is held across candidate submission: native queue
        // request allocation may need the same resource-creator worker.
        if (callback) callback(creator,context);
    }
    std::uintptr_t Replay(std::uintptr_t address,std::span<const std::uint8_t> prefix) {
        auto* code=static_cast<std::byte*>(trampoline.allocate(prefix.size()+14));
        std::memcpy(code,prefix.data(),prefix.size());
        constexpr std::array<std::uint8_t,6> jump{0xff,0x25,0,0,0,0};
        std::memcpy(code+prefix.size(),jump.data(),jump.size());
        const auto resume=address+prefix.size();std::memcpy(code+prefix.size()+6,&resume,sizeof(resume));
        FlushInstructionCache(GetCurrentProcess(),code,prefix.size()+14);
        return reinterpret_cast<std::uintptr_t>(code);
    }
}
bool Install(FrameCallback onFrame) {
    if (installed.load(std::memory_order_acquire)) return callback==onFrame;
    if (REL::Module::get().version()!=REL::Version{1,10,163,0}) return false;
    const auto base=REL::Module::get().base();
    const auto allocate=base+0x1d4dd30,free=base+0x1d4e470,construct=base+0x1d4cf80;
    const auto destruct=base+0x1d48dd0,frameCall=base+0x1d0b450,frameTarget=base+0x1d49e90;
    constexpr std::array<std::uint8_t,5> allocatePrefix{0x48,0x89,0x5c,0x24,0x18};
    constexpr std::array<std::uint8_t,5> freePrefix{0x48,0x89,0x5c,0x24,0x08};
    constexpr std::array<std::uint8_t,7> constructorPrefix{0x4c,0x8b,0xdc,0x49,0x89,0x4b,0x08};
    constexpr std::array<std::uint8_t,5> destructorPrefix{0x48,0x89,0x4c,0x24,0x08};
    constexpr std::array<std::uint8_t,3> contextLoad{0x48,0x8b,0x15};
    constexpr std::array<std::uint8_t,4> creatorLoad{0x49,0x8b,0x4e,0x18};
    if (std::memcmp(reinterpret_cast<void*>(allocate),allocatePrefix.data(),allocatePrefix.size()) ||
        std::memcmp(reinterpret_cast<void*>(free),freePrefix.data(),freePrefix.size()) ||
        std::memcmp(reinterpret_cast<void*>(construct),constructorPrefix.data(),constructorPrefix.size()) ||
        std::memcmp(reinterpret_cast<void*>(destruct),destructorPrefix.data(),destructorPrefix.size()) ||
        std::memcmp(reinterpret_cast<void*>(frameCall-11),contextLoad.data(),contextLoad.size()) ||
        frameCall-4+Field<std::int32_t>(reinterpret_cast<void*>(frameCall-11),3)!=base+0x61ddc60 ||
        std::memcmp(reinterpret_cast<void*>(frameCall-4),creatorLoad.data(),creatorLoad.size()) ||
        Field<std::uint8_t>(reinterpret_cast<void*>(frameCall),0)!=0xe8 ||
        frameCall+5+Field<std::int32_t>(reinterpret_cast<void*>(frameCall),1)!=frameTarget) return false;
    presentBarrier=reinterpret_cast<volatile std::uint32_t*>(base+0x609bfa0);
    nativeImmediate=reinterpret_cast<ID3D11DeviceContext**>(base+0x61ddc60);
    trampoline.create(512);
    originalAllocate=reinterpret_cast<Allocate>(Replay(allocate,allocatePrefix));
    originalFree=reinterpret_cast<Free>(Replay(free,freePrefix));
    originalConstructor=reinterpret_cast<Constructor>(Replay(construct,constructorPrefix));
    originalCreatorDestructor=reinterpret_cast<Destructor>(Replay(destruct,destructorPrefix));
    originalFrameUpdate=reinterpret_cast<FrameUpdate>(frameTarget);callback=onFrame;
    // Install at plugin load before creator construction. Existing unknown
    // allocator lifetimes remain untracked and cannot be reclaimed.
    trampoline.write_branch<5>(allocate,HookAllocate);
    trampoline.write_branch<5>(free,HookFree);
    trampoline.write_branch<5>(construct,HookConstructor);
    constexpr std::array<std::uint8_t,2> nops{0x90,0x90};REL::safe_write(construct+5,nops.data(),nops.size());
    trampoline.write_branch<5>(destruct,HookCreatorDestructor);
    trampoline.write_call<5>(frameCall,HookFrameUpdate);
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(construct),constructorPrefix.size());
    installed.store(true,std::memory_order_release);
    return true;
}
void SetReclaimEnabled(bool value) noexcept {reclaimEnabled.store(value,std::memory_order_release);}
void MarkMeshPoolRelease(ID3D11Buffer* backing) {
    if (!backing || !installed.load(std::memory_order_acquire) || *presentBarrier) return;
    std::scoped_lock lock(endpointLock);
    for (auto& owner:owners) if (owner.allocator && !owner.retiring) {
        auto* heaps=static_cast<NativeHeap*>(owner.allocator);
        for (std::size_t i=0;i<8;++i) if (heaps[i].backing==backing) owner.meshTouched[i]=true;
    }
}
Statistics GetStatistics() noexcept {
    return {installed.load(),framePumps.load(),detachedHeaps.load(),releasedReferences.load(),releasedReferenceBytes.load(),
        eligibleEmptyHeaps.load(),retiringSkipped.load(),untrackedSkipped.load(),unsafePhaseSkipped.load(),
        lifetimeHeapObservations.load(),lifetimeHeapInterfaceFailures.load()};
}
}
