#pragma once

#include <string_view>

namespace Fo4Mesh::Lifetime::Diagnostics {
    constexpr bool IsExplicitOptIn(std::wstring_view value) noexcept {return value==L"1";}

    // Load-time diagnostic only, default off. Exact environment value
    // DTL_FO4_MESH_LIFETIME=1 opts in and permanently pins the observer module.
    // Call before native heap construction/hooks or mesh buffer creation.
    // A late call observes existing resources; it cannot reconstruct births.
    bool InitializeFromEnvironment() noexcept;

    // Call from the planner's telemetry worker OUTSIDE renderer, mesh-registry
    // and allocator locks. Disabled calls are atomic-only. Enabled calls drain
    // at most once per second, emit bounded events and honest coverage counters,
    // and never access a resource/native pointer. No background thread is added.
    // Successful logger calls do not themselves attest durable sink output.
    // Diagnostic work perturbs performance and must not be used in timing cases.
    void PollLog() noexcept;
}
