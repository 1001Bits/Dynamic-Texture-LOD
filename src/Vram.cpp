#include "Vram.h"

#include "Addresses.h"
#include "HostFloor.h"
#include "PluginIni.h"
#include "core/BudgetMode.h"
#include "core/VramMonitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")

namespace
{
    // Where BSGraphics::pDevice lives. Set before the monitor thread starts,
    // which is what makes it visible there.
    std::uintptr_t g_deviceSlot = 0;

    // [Budget] HostRamFloorMB (HostFloor.h), 0 (off) by default. At 4K with a
    // texture pack host RAM can run out before video memory. Starfield mode
    // samples per frame inside the core and is not covered.
    std::uint64_t g_hostFloor = 0;

    // The device itself, null until the renderer has created it.
    void* GetDevice() { return *reinterpret_cast<void* const volatile*>(g_deviceSlot); }

    void OnReading(const dtl::vram::Reading& a_reading)
    {
        auto reading = a_reading;
        if (g_hostFloor && reading.budget) {
            MEMORYSTATUSEX system{};
            system.dwLength = sizeof(system);
            if (::GlobalMemoryStatusEx(&system))
                reading.budget = HostFloor::Budget(reading.budget, system.ullAvailPhys, g_hostFloor);
        }
        dtl::budget::OnReading(reading);
    }
}

namespace Vram
{
    void Start()
    {
        g_deviceSlot = Addr::Resolve(Addr::D3DDevice);
        if (!g_deviceSlot) {
            logger::warn("No D3D device address for this runtime, budget mode can't read video memory");
            return;
        }
        g_hostFloor = static_cast<std::uint64_t>(PluginIni::Read("Budget", "HostRamFloorMB", 0)) << 20;
        if (g_hostFloor)
            logger::info("Budget mode keeps {} MB of host RAM available by lowering textures", g_hostFloor >> 20);
        // [Budget] AsyncSampling, 0 (off) until measured: the per-frame video memory reading on a thread of its
        // own, so the render thread never waits on the kernel call, which can take seconds under heavy paging.
        if (PluginIni::Read("Budget", "AsyncSampling", 0)) {
            dtl::vram::SetAsyncSampling(true);
            logger::info("Video memory readings on a thread of their own; each frame takes the newest one");
        }
        dtl::vram::Start(&GetDevice, &OnReading);
    }

    HostMemory ReadHostMemory()
    {
        HostMemory result;
        PROCESS_MEMORY_COUNTERS_EX process{};
        MEMORYSTATUSEX system{};
        system.dwLength = sizeof(system);
        if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
                sizeof(process)) || !::GlobalMemoryStatusEx(&system))
            return result;
        result.privateBytes = process.PrivateUsage;
        result.workingSet = process.WorkingSetSize;
        result.available = system.ullAvailPhys;
        result.valid = true;
        return result;
    }
}
