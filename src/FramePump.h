#pragma once
#include <cstdint>

struct IDXGISwapChain;

namespace FramePump
{
    using Callback = void (*)(std::uint64_t frame);
    // A real renderer frame, including occluded frames; never a timer tick or
    // a DXGI_PRESENT_TEST probe. Chains existing Present hooks. Starting again
    // only adds a callback; nullptr starts it without one.
    bool Start(Callback callback);

    // Called on the same frames, after the callback and just before the game's
    // Present, with its swap chain: drawing here lands on the presented image.
    using Draw = void (*)(IDXGISwapChain* swap);
    void SetDraw(Draw draw);

    // What the frame hook is doing now. A step that lasts over three seconds is
    // logged with its thread and the planner's step, once per stall.
    void Step(const char* what);
}
