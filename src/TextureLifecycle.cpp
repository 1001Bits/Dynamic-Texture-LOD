#include "TextureLifecycle.h"
#include "Addresses.h"

#include <cstring>

namespace
{
    // OG/VR share this allocator with structured buffers. Only the seven
    // independently verified texture E8 callers below are patched.
    constexpr Addr::Location kAllocate{ 562495, 2277076, 0x1DA4020 };
    constexpr Addr::Location kRetire{ 639849, 2277429, 0x1DD3790 };
    constexpr std::array<std::uintptr_t, 7> kOgBirth{
        0x1D0EBA2, 0x1D0ED7F, 0x1D0EF3C, 0x1D0F159,
        0x1D1F6F8, 0x1D36F3C, 0x1D37AA9
    };
    constexpr std::array<std::uintptr_t, 7> kVrBirth{
        0x1D91432, 0x1D9160F, 0x1D917CC, 0x1D919E9,
        0x1DA5718, 0x1DC0BBC, 0x1DC16E9
    };
    constexpr std::array<std::uint8_t, 5> kRetirePrologue{ 0x48, 0x89, 0x5C, 0x24, 0x08 };
    using Allocate = void* (*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    using Retire = void (*)(void*, void*);
    Allocate g_allocate{};
    Retire g_retire{};
    TextureLifecycle::Callback g_birthCallback{}, g_retireCallback{};
    bool g_installed = false;
    std::atomic<std::uint64_t> g_callbackFailures{ 0 };

    void Notify(TextureLifecycle::Callback callback, void* texture) noexcept
    {
        try { callback(texture); }
        catch (...) { g_callbackFailures.fetch_add(1, std::memory_order_relaxed); }
    }

    void* OnAllocate(void* pool, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4)
    {
        // Preserve all four argument registers: the generic allocator's
        // external-allocation slow path forwards its incoming registers.
        void* texture = g_allocate(pool, a2, a3, a4);
        if (texture) Notify(g_birthCallback, texture);
        return texture;
    }

    void OnRetire(void* creator, void* texture)
    {
        Notify(g_retireCallback, texture);
        g_retire(creator, texture);
    }

    std::vector<std::uintptr_t> CallsTo(std::uintptr_t target)
    {
        const auto text = REL::Module::get().segment(REL::Segment::text);
        std::vector<std::uintptr_t> result;
        for (auto at = text.address(); at + 5 <= text.address() + text.size(); ++at) {
            if (*reinterpret_cast<const std::uint8_t*>(at) != 0xE8) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, reinterpret_cast<const void*>(at + 1), sizeof(displacement));
            if (static_cast<std::uintptr_t>(static_cast<std::intptr_t>(at + 5) + displacement) == target)
                result.push_back(at);
        }
        return result;
    }
}

namespace TextureLifecycle
{
    bool Install(Callback birth, Callback retire)
    {
        if (g_installed) return birth == g_birthCallback && retire == g_retireCallback;
        if (!birth || !retire) return false;
        const auto allocator = Addr::Resolve(kAllocate);
        const auto retirement = Addr::Resolve(kRetire);
        if (!allocator || !retirement) return false;
        const auto text = REL::Module::get().segment(REL::Segment::text);
        if (retirement < text.address() || retirement + kRetirePrologue.size() > text.address() + text.size() ||
            std::memcmp(reinterpret_cast<const void*>(retirement), kRetirePrologue.data(), kRetirePrologue.size()) != 0) {
            logger::error("Native texture retirement prologue mismatch; lifecycle hooks disabled");
            return false;
        }
        const auto all = CallsTo(allocator);
        std::vector<std::uintptr_t> selected;
        if (REL::Module::IsNG()) {
            // NG's wrapper initializes exactly one BSGraphics::Texture;
            // unlike the OG/VR generic pool it has seven texture-only users.
            if (all.size() == 7) selected = all;
        } else if (all.size() == 9) {
            const auto& offsets = REL::Module::IsVR() ? kVrBirth : kOgBirth;
            for (auto offset : offsets) {
                const auto site = Addr::ImageAddress(offset);
                if (std::find(all.begin(), all.end(), site) == all.end()) {
                    selected.clear();
                    break;
                }
                selected.push_back(site);
            }
        }
        auto& trampoline = F4SE::GetTrampoline();
        if (selected.size() != 7 || trampoline.free_size() < 256) {
            logger::error("Native texture lifecycle validation failed: {} allocator calls, {} texture calls, {} trampoline bytes",
                all.size(), selected.size(), trampoline.free_size());
            return false;
        }

        // All engine code/addresses are validated before any write. Replay the
        // single 5-byte, RIP-independent MOV then JMP [RIP+0] to entry+5. No
        // scratch register, stack adjustment or relocated branch is needed.
        auto* resume = static_cast<std::uint8_t*>(trampoline.allocate(19));
        std::memcpy(resume, kRetirePrologue.data(), kRetirePrologue.size());
        constexpr std::uint8_t jump[]{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(resume + 5, jump, sizeof(jump));
        const auto next = retirement + 5;
        std::memcpy(resume + 11, &next, sizeof(next));
        if (!REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), resume, 19)) {
            logger::error("Cannot flush native texture lifecycle trampoline; hooks disabled");
            return false;
        }
        g_allocate = reinterpret_cast<Allocate>(allocator);
        g_retire = reinterpret_cast<Retire>(resume);
        g_birthCallback = birth;
        g_retireCallback = retire;
        trampoline.write_branch<5>(retirement, OnRetire);
        for (auto site : selected) trampoline.write_call<5>(site, OnAllocate);
        g_installed = true;
        logger::info("Native texture lifecycle installed: 7 birth calls and zero-reference retirement; no physical-free credit");
        return true;
    }
}
