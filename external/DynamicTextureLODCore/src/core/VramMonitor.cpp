#include "VramMonitor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace dtl::vram
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // Legacy/diagnostic polling. Starfield mode uses actual frame calls.
        constexpr auto kReadEvery = std::chrono::milliseconds(100);
        constexpr auto kDeviceRetry = std::chrono::milliseconds(500);

        std::atomic<bool>          g_running{ false };
        std::atomic<bool>          g_failed{ false };
        std::mutex g_lock;
        ComPtr<IDXGIAdapter3> g_adapter;
        Reading g_latest;
        bool g_frameSampling = false;
        bool g_haveFrame = false;
        std::uint64_t g_lastFrame = 0;
        auto g_lastFrameTime = std::chrono::steady_clock::time_point{};

        // Asynchronous frame sampling: the kernel call happens on a thread of its own, since it can wait while
        // Windows moves video memory, for seconds under heavy paging. Each frame
        // asks for a reading and takes the newest finished one, normally the previous frame's.
        std::atomic<bool> g_async{ false };
        std::mutex g_askLock;
        std::condition_variable g_ask;
        std::uint64_t g_askedFrame = 0, g_readFrame = 0;  // under g_askLock
        Reading g_asyncReading;                          // under g_lock
        bool g_haveAsync = false;                        // under g_lock

        bool Query(IDXGIAdapter3* a_adapter, Reading& a_reading)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            if (!a_adapter || FAILED(a_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
                local.Budget == 0)
                return false;
            a_reading.usage = local.CurrentUsage;
            a_reading.budget = local.Budget;
            return true;
        }

        void Sampler()
        {
            for (;;) {
                std::uint64_t frame;
                {
                    std::unique_lock lock(g_askLock);
                    g_ask.wait(lock, [] { return g_askedFrame != g_readFrame; });
                    frame = g_askedFrame;
                }
                IDXGIAdapter3* adapter;
                {
                    std::scoped_lock lock(g_lock);
                    adapter = g_adapter.Get();  // set once and held for the session
                }
                Reading reading{ 0, 0, frame, true };
                if (Query(adapter, reading)) {
                    std::scoped_lock lock(g_lock);
                    g_asyncReading = reading;
                    g_haveAsync = true;
                }
                std::scoped_lock lock(g_askLock);
                g_readFrame = frame;
            }
        }

        bool ReadLocked(Reading& reading)
        {
            if (!Query(g_adapter.Get(), reading)) return false;
            g_latest = reading;
            return true;
        }

        // The adapter is the DXGI parent of the device's DXGI side. Only an
        // IDXGIAdapter3 can report its memory budget (Windows 10 and later).
        ComPtr<IDXGIAdapter3> AdapterOf(IUnknown* a_device)
        {
            ComPtr<IDXGIDevice>   dxgiDevice;
            ComPtr<IDXGIAdapter>  adapter;
            ComPtr<IDXGIAdapter3> adapter3;

            if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) &&
                SUCCEEDED(dxgiDevice->GetParent(IID_PPV_ARGS(adapter.GetAddressOf())))) {
                adapter.As(&adapter3);
            }
            return adapter3;
        }

        void Monitor(DeviceGetter a_device, Listener a_listener)
        {
            // The game creates its device after plugins load.
            void* device = nullptr;
            while (!(device = a_device())) {
                std::this_thread::sleep_for(kDeviceRetry);
            }

            auto adapter = AdapterOf(static_cast<IUnknown*>(device));
            if (!adapter) {
                g_failed.store(true, std::memory_order_relaxed);
                spdlog::warn("This graphics adapter can't report its video memory budget");
                return;
            }
            {
                std::scoped_lock lock(g_lock);
                g_adapter = adapter;
            }
            const bool async = g_async.load(std::memory_order_relaxed);
            if (async) std::thread(Sampler).detach();

            // Held for good: this thread lives as long as the process.
            for (bool first = true;; std::this_thread::sleep_for(kReadEvery)) {
                Reading reading;
                if (async) {
                    // Not under the lock: the render thread takes it every frame.
                    {
                        std::scoped_lock lock(g_lock);
                        if (g_frameSampling && g_haveFrame &&
                            std::chrono::steady_clock::now() - g_lastFrameTime < kDeviceRetry) continue;
                    }
                    if (!Query(adapter.Get(), reading)) continue;
                    std::scoped_lock lock(g_lock);
                    g_latest = reading;
                } else {
                    std::scoped_lock lock(g_lock);
                    if (g_frameSampling && g_haveFrame &&
                        std::chrono::steady_clock::now() - g_lastFrameTime < kDeviceRetry) continue;
                    if (!ReadLocked(reading)) continue;
                }

                if (first) {
                    first = false;
                    spdlog::info("Video memory: {} MB budget, {} MB in use", reading.budget >> 20, reading.usage >> 20);
                }

                a_listener(reading);
            }
        }
    }

    void Start(DeviceGetter a_device, Listener a_listener)
    {
        if (!a_device || !a_listener || g_running.exchange(true)) {
            return;
        }

        // Never joined. A plugin's static destructors run under the loader
        // lock, where waiting for a thread can hang the exit, and these games
        // often exit without unloading plugins at all.
        std::thread(Monitor, a_device, a_listener).detach();
    }

    Reading Latest()
    {
        std::scoped_lock lock(g_lock);
        return g_latest;
    }

    void SetFrameSampling(bool a_enabled)
    {
        std::scoped_lock lock(g_lock);
        g_frameSampling = a_enabled;
        g_haveFrame = false;
    }

    void SetAsyncSampling(bool a_enabled)
    {
        g_async.store(a_enabled, std::memory_order_relaxed);
    }

    Reading SampleFrame(std::uint64_t a_frame)
    {
        if (g_async.load(std::memory_order_relaxed)) {
            {
                std::scoped_lock lock(g_askLock);
                g_askedFrame = a_frame;
            }
            g_ask.notify_one();
            std::scoped_lock lock(g_lock);
            if (!g_frameSampling) return g_latest;
            if (g_haveFrame && g_lastFrame == a_frame) return g_latest;
            g_haveFrame = true;
            g_lastFrame = a_frame;
            g_lastFrameTime = std::chrono::steady_clock::now();
            auto reading = g_haveAsync ? g_asyncReading : g_latest;
            reading.frame = a_frame;
            reading.frameSample = true;
            g_latest = reading;
            return reading;
        }
        std::scoped_lock lock(g_lock);
        if (!g_frameSampling) return g_latest;
        if (g_haveFrame && g_lastFrame == a_frame) return g_latest;
        g_haveFrame = true;
        g_lastFrame = a_frame;
        g_lastFrameTime = std::chrono::steady_clock::now();
        Reading reading{ 0, 0, a_frame, true };
        if (!ReadLocked(reading)) {
            // Mark a reused startup/failure sample with the actual frame ID;
            // budget smoothing still advances only on a real frame callback.
            reading = g_latest;
            reading.frame = a_frame;
            reading.frameSample = true;
            g_latest = reading;
        }
        return reading;
    }

    bool Failed() { return g_failed.load(std::memory_order_relaxed); }
}
