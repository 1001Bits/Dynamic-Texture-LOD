#pragma once

// Native allocation/zero-reference retirement, not resource replacement or GPU
// release. Install only during plugin startup, before game worker threads run.
namespace TextureLifecycle
{
    using Callback = void (*)(void* texture);

    // Birth runs after pool allocation, BEFORE the caller initializes the
    // texture header. Both callbacks may only invalidate identity bookkeeping;
    // neither may read the texture, call COM, or count physical memory freed.
    // Retirement is reported before native enqueue/free. Callback exceptions
    // are contained so the original engine operation always executes.
    // Requires at most 256 bytes of the existing F4SE trampoline.
    bool Install(Callback birth, Callback retire);
}
