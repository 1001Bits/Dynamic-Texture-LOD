#include "FramePump.h"
#include "Addresses.h"
#include "Lod.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <dxgi.h>

namespace
{
    // The process's first thread runs the game loop and renders the world. Not the thread loading plugins:
    // f4se_loader loads F4SE from a thread of its own.
    DWORD PrimaryThread()
    {
        DWORD primary = 0;
        ULARGE_INTEGER earliest{ .QuadPart = ~0ull };
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 entry{ .dwSize = sizeof(THREADENTRY32) };
        for (BOOL more = ::Thread32First(snapshot, &entry); more; more = ::Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != ::GetCurrentProcessId()) continue;
            if (const HANDLE thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (::GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                    const ULARGE_INTEGER at{ .LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime };
                    if (at.QuadPart < earliest.QuadPart) {
                        earliest = at;
                        primary = entry.th32ThreadID;
                    }
                }
                ::CloseHandle(thread);
            }
        }
        ::CloseHandle(snapshot);
        return primary;
    }

    using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    std::atomic<Present> g_next{};
    std::uintptr_t g_rendererSlot = 0;
    std::atomic<FramePump::Callback> g_callback{};
    std::atomic<FramePump::Draw> g_draw{};
    std::uint64_t g_frame = 0;
    std::atomic_flag g_inFrame = ATOMIC_FLAG_INIT;
    std::atomic<bool> g_started{ false };
    std::atomic<DWORD> g_mainThread{ 0 };
    // For the stall report: what the hook is doing, since when, on which thread.
    std::atomic<const char*> g_step{ nullptr };
    std::atomic<std::uint64_t> g_stepSince{ 0 };
    std::atomic<DWORD> g_stepThread{ 0 };

    IDXGISwapChain* FindSwapChain() noexcept
    {
        // Renderer initialization precedes use; the SEH boundary contains
        // only borrowed reads, no locks, ownership changes or engine calls.
        __try {
            const auto* data = *reinterpret_cast<RE::BSGraphics::RendererData**>(g_rendererSlot);
            return data ? reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain) : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* swap, UINT interval, UINT flags)
    {
        const auto next = g_next.load(std::memory_order_acquire);
        if (!(flags & DXGI_PRESENT_TEST) && swap == FindSwapChain() &&
            !g_inFrame.test_and_set(std::memory_order_acquire)) {
            g_stepThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
            g_stepSince.store(::GetTickCount64(), std::memory_order_relaxed);
            g_step.store("texture planner frame", std::memory_order_release);
            ++g_frame;
            if (const auto callback = g_callback.load(std::memory_order_acquire)) callback(g_frame);
            if (const auto draw = g_draw.load(std::memory_order_acquire)) draw(swap);
            g_step.store(nullptr, std::memory_order_release);
            g_inFrame.clear(std::memory_order_release);
        }
        return next(swap, interval, flags);
    }

    bool Install(IDXGISwapChain* swap)
    {
        auto** table = *reinterpret_cast<void***>(swap);
        const auto existing = reinterpret_cast<Present>(table[8]);
        if (!existing || existing == OnPresent) return false;
        DWORD protection{};
        if (!VirtualProtect(table + 8, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
        g_next.store(existing, std::memory_order_release);
        const auto previous = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(table + 8), reinterpret_cast<void*>(&OnPresent),
            reinterpret_cast<void*>(existing));
        DWORD ignored{};
        VirtualProtect(table + 8, sizeof(void*), protection, &ignored);
        return previous == reinterpret_cast<void*>(existing);
    }

    // A frame hook that never returns stops the game, or the thread presenting
    // menus and loading screens: name the step, so the log shows the cause.
    void Watch()
    {
        std::uint64_t reported = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto* step = g_step.load(std::memory_order_acquire);
            const auto since = g_stepSince.load(std::memory_order_relaxed);
            const auto now = ::GetTickCount64();
            if (reported && (!step || since != reported)) {
                logger::info("Frame hook free again after {} s", (now - reported) / 1000);
                reported = 0;
            }
            if (step && !reported && now - since >= 3000) {
                reported = since;
                const auto thread = g_stepThread.load(std::memory_order_relaxed);
                const bool main = thread == g_mainThread.load(std::memory_order_relaxed);
                logger::warn("Frame hook stuck for {} s at '{}' on thread {} ({}), texture planner at '{}'",
                    (now - since) / 1000, step, thread, main ? "main thread" : "not the main thread",
                    Lod::PlanStep());
            }
        }
    }
}

namespace FramePump
{
    bool Start(Callback callback)
    {
        if (callback) g_callback.store(callback, std::memory_order_release);
        if (g_started.exchange(true, std::memory_order_acq_rel)) return true;
        g_mainThread.store(PrimaryThread(), std::memory_order_relaxed);
        g_rendererSlot = Addr::RendererDataSlot();
        if (!g_rendererSlot) {
            logger::error("No verified renderer frame source for this runtime");
            return false;
        }
        std::thread([] {
            for (;;) {
                if (auto* swap = FindSwapChain(); swap && Install(swap)) {
                    if (g_callback.load(std::memory_order_acquire))
                        logger::info("Renderer frame pump installed; configured worker slice, 100-entry visits, gather frame barrier");
                    else
                        logger::info("Renderer frame pump installed without a planner callback");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
        std::thread(Watch).detach();
        return true;
    }

    void SetDraw(Draw draw)
    {
        g_draw.store(draw, std::memory_order_release);
    }

    void Step(const char* what)
    {
        g_step.store(what, std::memory_order_release);
    }
}
