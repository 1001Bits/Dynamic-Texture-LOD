#pragma once

// Texture LOD through Fallout 4's native streaming, with a shared planner
// inspired by Starfield's memory priorities. Level means top mips omitted.
//
// Paired object-level and ForEachTexture call sites scope a thread-local
// observation to the synchronous texture walk. That walk supplies material
// UV scale; the texture functor supplies dimensions and the current level.
// Policy0 uses the legacy pixel estimate. Policy1 uses angular thresholds,
// staged Starfield budget decisions and optional conservative GPU feedback.
//
// Observations retain the largest use in the active and preceding native
// traversal rounds. StartOver advances their age, so a slow, time-sliced
// traversal cannot expire measurements merely because seconds have passed.
// Desktop uses a sphere/frustum visibility test; VR conservatively retains
// visited objects because WorldRootCamera's stereo coverage is unverified.
//
// Policy1 runs frame-scheduled slices with a gather barrier; Policy0 retains
// its100ms worker. The engine performs the actual streaming.
// Fixed-level passes and the engine's forced fast-travel level stay native.
namespace Lod
{
    // Installs the hooks and starts planning. Returns false, changing
    // nothing, if this runtime's code isn't what the plugin expects.
    bool Install();

    void LogSummary();

    // Live on/off (MCM, F11). Off, every hook hands the game its own answers, planning stops and the game gets
    // its own upgrade limit back, as without the plugin; on, as configured at load. Never resumes a planner the
    // fail-safe stopped. Returns whether the mod is now active.
    bool SetActive(bool a_active);
    bool Active();

    // The planner's current step, for the frame hook's stall report.
    const char* PlanStep();

    // MCM "4/6 GB budget mode", live: every texture at most 1024 pixels, all the time, for 4 to 6 GB cards.
    void SetBudgetMode(bool a_on);
}
