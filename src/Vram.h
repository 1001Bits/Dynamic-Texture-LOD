#pragma once

// The shared video memory monitor, fed with Fallout 4's D3D device. Every
// reading goes to budget mode.
namespace Vram
{
    // Starts the monitor. It waits for the renderer's device on its own
    // thread, so this is safe from F4SEPlugin_Load. Calling it again does
    // nothing.
    void Start();

    // This process's committed and resident memory and the system's available
    // physical memory. At 4K with a texture pack host RAM can run out before
    // video memory, so the timeline reports it beside texture bytes.
    struct HostMemory
    {
        std::uint64_t privateBytes = 0;
        std::uint64_t workingSet = 0;
        std::uint64_t available = 0;
        bool          valid = false;
    };
    HostMemory ReadHostMemory();
}
