#pragma once

#include "Planner.h"
#include "VramMonitor.h"
#include "StarfieldPlanner.h"

#include <chrono>
#include <span>

// Budget mode as every plugin runs it: each video memory reading weighed
// against the budget lines, with the log lines, the timeline and the
// summary, and one planning cycle that every backend runs the same way.
namespace dtl::budget
{
    struct Settings
    {
        bool         enabled = true;
        BudgetLines  lines;
        PlanSettings plan;
        bool         dryRun = false;    // plan with no limits, log what the limits would have done
        bool         timeline = false;  // log every second, for bug reports
        bool         screenSize = true;
        std::array<float,4> textureThresholds = kStarfieldTextureThresholds;
        std::array<float,4> meshThresholds = kStarfieldMeshThresholds;
        bool         noMeshesUpgradeLimits = true;
        double       timeSliceMs = 10.0;

        // Departures from the source (dtl::Settings [Smoothing] and [Budget]).
        // The defaults here are the source's; FromIni fills the INI's.
        std::uint32_t unseenDelayMs = 0;
        std::uint32_t degradeCooldownMs = 0;
        float         hysteresis = 0.0f;
        bool          creditPendingFrees = false;
        bool          headroomNetOfGrowth = false;
        bool          fullUpgradeScan = false;
        std::uint32_t overDebounceFrames = 1;
        bool          adaptiveSampling = false;
        std::uint32_t roomCycleMs = 0;

        // Pressure first, from Skyrim Performance Solution's history. The
        // defaults here are the source's (all off); FromIni fills the INI's.
        bool          pressureOnly = false;
        std::uint32_t raiseCooldownMs = 0;
        std::uint64_t minLoweringBytes = 0; // lowerings freeing less are not worth their re-create; 0 off
        std::uint64_t refillGap = 0;        // refill starts this far under the target, fills to 3/4 of it
        std::uint32_t refillCalmMs = 0;     // and only this long after pressure or a load
        bool          processCounters = false;  // usage is the larger of DXGI and the process counters
        std::uint64_t overflowShared = 0;   // paged-out memory that, while rising, means over budget; 0 off
        // After 3 minutes of play the target keeps the largest jump seen free
        // (ReserveMeter), when that is more than reserve plus target.
        bool          autoTarget = false;
    };

    // The settings as the ini has them.
    Settings FromIni();

    // Called on the monitor thread after every reading, for a backend that
    // plans on each one. May be null.
    using Listener = void (*)();

    // Sets or changes the settings. Safe at any time from any thread.
    void Configure(const Settings& a_settings, Listener a_listener);

    // Feeds one reading. Call from the video memory monitor's listener.
    void OnReading(const vram::Reading& a_reading);
    // Call once for each actual game frame. The adapter currently uses
    // Present, not Starfield's frame-begin hook; that phase is an adaptation.
    void OnFrame(std::uint64_t a_frame);

    // The latest reading weighed against the lines. Unlimited while budget
    // mode is off or before the first reading. Safe from any thread.
    Budget Current();
    // Just its mode, without a lock, for per-frame callers.
    Mode CurrentMode();

    // Whether a planning cycle that last began at a_lastBegin may begin
    // another now: always when tight or over, at most every RoomCycleMs with
    // room to spare.
    bool CycleDue(std::chrono::steady_clock::time_point a_lastBegin);

    // A load is under way. Video memory looks empty while a scene streams
    // in, so refill waits RefillCalmMs after the last one. Any thread.
    void NoteLoad();
    // A load began or ended (a saved game). While one goes on, at most two
    // minutes, every reading counts as a load, and no jump is measured across
    // it. Any thread.
    void SetLoading(bool a_loading);

    // Free memory the target keeps from the largest jump measured so far; 0
    // until 3 minutes of play have been measured. Any thread.
    std::uint64_t MeasuredKeepFree();

    // Test arms only: added to every usage reading until set back to 0, so a
    // test can put the planner over its line without real memory pressure.
    void SetSyntheticUsage(std::uint64_t a_bytes);

    // Diagnostics, timeline only: the refill gate as the last planning cycle
    // began it.
    struct RefillState
    {
        bool          considered = false;  // pressure-only, with room
        bool          quiet = false;
        bool          refilling = false;
        std::uint64_t refillBytes = 0;
        std::int64_t  sinceLoadMs = -1;      // -1: never
        std::int64_t  sincePressureMs = -1;  // -1: never
        std::uint64_t sharedRise = 0;
        std::uint64_t spent = 0;
        std::uint64_t target = 0;
    };
    RefillState LastRefill();

    // Plans one cycle against the current budget and settings, and keeps its
    // figures for the log. In a dry run the entries get the unlimited plan
    // and the real one is only logged.
    PlanResult PlanCycle(std::span<PlanEntry> a_entries);
    void BeginPlan(StarfieldPlanSession& a_session, std::span<const PlanEntry> a_entries,
        const StreamingAccounting& a_accounting = {}, std::uint64_t a_meshSpent = 0,
        std::span<const PlanEntry> a_meshEntries = {});
    void RecordPlan(const PlanResult& a_result);
    bool StepPlan(StarfieldPlanSession& a_session, RequestSink a_sink, void* a_context,
                  RequestSink a_meshSink = nullptr, void* a_meshContext = nullptr);

    // Peak usage, time tight and over budget, and what planning did, as one
    // log line.
    void LogSummary();
}
