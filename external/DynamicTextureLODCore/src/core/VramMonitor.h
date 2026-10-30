#pragma once

#include <cstdint>

// Video memory use, read from DXGI on a thread of its own ten times a
// second. Engine-independent: the game supplies its D3D11 device and gets a
// callback with every reading.
namespace dtl::vram
{
    struct Reading
    {
        std::uint64_t usage = 0;   // bytes the process has in local video memory
        std::uint64_t budget = 0;  // bytes the OS lets it use, zero until known
        std::uint64_t frame = 0;
        bool frameSample = false;
    };

    // The game's ID3D11Device as an IUnknown*, or null until it has one.
    using DeviceGetter = void* (*)();

    // Called on the monitor thread after every reading.
    using Listener = void (*)(const Reading&);

    // Starts the monitor. It waits for the device, then polls for the rest of
    // the session. Calling it again does nothing.
    void Start(DeviceGetter a_device, Listener a_listener);

    // The latest reading. budget is zero before the first one.
    Reading Latest();
    // Frame mode queries once per distinct real frame; the background thread
    // still discovers the adapter and supplies a startup/fallback reading.
    void SetFrameSampling(bool a_enabled);
    Reading SampleFrame(std::uint64_t a_frame);
    // Before Start: frame mode reads on a thread of its own, and each frame
    // takes the newest finished reading (normally the previous frame's), so the
    // caller never waits on the kernel call. Off: the caller reads itself.
    void SetAsyncSampling(bool a_enabled);

    // True once it is clear the device can't report its memory.
    bool Failed();
}
