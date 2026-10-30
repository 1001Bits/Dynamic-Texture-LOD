#include "BudgetMode.h"

#include "Settings.h"
#include "FrameBudget.h"
#include "GpuProcessMemory.h"
#include "ReserveMeter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

namespace dtl::budget
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kTimelinePeriod = std::chrono::seconds(1);

        constexpr std::uint64_t MB(std::uint64_t a_bytes) { return a_bytes >> 20; }

        // Settings and the latest assessment change together under one lock:
        // the monitor thread feeds readings ten times a second, the backend
        // plans from another thread.
        std::mutex        g_lock;
        Settings          g_settings;
        Listener          g_listener = nullptr;
        Budget            g_current;
        FrameBudgetSmoother g_smoother;
        Clock::time_point g_modeSince{};
        Clock::time_point g_nextTimeline{};

        // Only the backend's planning thread touches this.
        Clock::time_point g_nextPlanLog{};

        // Read every frame without the lock.
        std::atomic<bool>          g_starfield{ false };
        std::atomic<bool>          g_adaptive{ false };
        std::atomic<std::uint32_t> g_roomCycleMs{ 0 };
        std::atomic<std::uint8_t>  g_mode{ static_cast<std::uint8_t>(Mode::Unlimited) };
        std::atomic<std::uint64_t> g_headroom{ 0 };  // usage to the unsmoothed target, bytes
        std::atomic<std::uint64_t> g_lastSampleFrame{ 0 };
        std::uint32_t              g_overStreak = 0;  // lock held

        // With this much headroom a reading every so many frames is enough:
        // a cell load does not fill a gigabyte in a quarter second.
        constexpr std::uint64_t kMaxSampleInterval = 16;
        std::uint64_t SampleInterval(std::uint64_t a_headroom)
        {
            return a_headroom >= (1ull << 30) ? kMaxSampleInterval : a_headroom >= (512ull << 20) ? 4 : 1;
        }

        std::uint64_t Minus(std::uint64_t a_left, std::uint64_t a_right) { return a_left > a_right ? a_left - a_right : 0; }

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        }

        // Pressure first. Paging out counts once it rose this much within the
        // window (SPS 0.23.0: its own upload buffers alone add 150-200 MB, so
        // the level must also stay above OverflowSharedMB). Counter samples
        // older than kProcessFreshMs are not used.
        constexpr std::uint64_t kOverflowRise = 32ull << 20;
        constexpr std::int64_t  kOverflowWindowMs = 10000;
        constexpr std::int64_t  kRefillSharedWindowMs = 30000;
        constexpr std::int64_t  kProcessFreshMs = 3000;
        std::atomic<std::int64_t> g_lastPressureMs{ 0 };
        std::atomic<std::int64_t> g_lastLoadMs{ 0 };
        std::atomic<std::uint64_t> g_syntheticUsage{ 0 };  // test arms only
        bool g_refilling = false;  // lock held
        RefillState g_lastRefill;  // lock held

        // The reserve from measured jumps. Refill's own rises are not demand:
        // none is measured while it runs or for a few seconds after.
        constexpr std::int64_t kAfterRefillMs = 6000;
        // A load noted this recently is still going on (fast travel notes one
        // every native round). A loading screen whose end never arrives
        // stops counting after two minutes, so refill cannot wait for good.
        constexpr std::int64_t kLoadGoingMs = 1000;
        constexpr std::int64_t kLoadingAtMostMs = 120000;
        std::atomic<std::int64_t> g_loadingSinceMs{ 0 };  // 0: no loading screen
        std::atomic<std::int64_t> g_lastRefillMs{ 0 };
        ReserveMeter  g_meter;          // lock held
        std::uint64_t g_keepFree = 0;   // lock held; 0 until the meter is ready
        BudgetLines   g_lines;          // lock held; the settings' lines with the measured target

        // The settings' lines, with the target lowered to keep the measured
        // jump free, at most a quarter of the OS budget. Lock held.
        BudgetLines EffectiveLines(std::uint64_t a_osBudget)
        {
            auto lines = g_settings.lines;
            if (g_settings.autoTarget && g_keepFree) {
                const auto keep = a_osBudget ? std::min(g_keepFree, a_osBudget / 4) : g_keepFree;
                if (keep > lines.reserve) lines.target = std::max(lines.target, keep - lines.reserve);
            }
            return lines;
        }

        // Summary figures.
        std::atomic<std::uint64_t> g_peakUsage{ 0 };
        std::atomic<std::uint64_t> g_lastLine{ 0 };
        std::atomic<std::uint64_t> g_timesOver{ 0 };
        std::atomic<std::uint64_t> g_tightMs{ 0 };
        std::atomic<std::uint64_t> g_overMs{ 0 };
        std::atomic<std::uint64_t> g_emergencyBytes{ 0 };
        std::atomic<std::uint64_t> g_tradedBytes{ 0 };
        std::atomic<std::uint64_t> g_swaps{ 0 };
        std::atomic<std::uint64_t> g_heldCycles{ 0 };

        template <class T>
        void StoreMax(std::atomic<T>& a_target, T a_value)
        {
            if (a_value > a_target.load(std::memory_order_relaxed)) {
                a_target.store(a_value, std::memory_order_relaxed);
            }
        }

        // Time spent in the mode being left. Lock held.
        void CloseMode(Mode a_mode, Clock::time_point a_now)
        {
            const auto lasted = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(a_now - g_modeSince).count());
            if (a_mode == Mode::Tight) {
                g_tightMs.fetch_add(lasted, std::memory_order_relaxed);
            } else if (a_mode == Mode::Over) {
                g_overMs.fetch_add(lasted, std::memory_order_relaxed);
            }
            g_modeSince = a_now;
        }

        void LogTimeline(const Budget& a_budget)
        {
            spdlog::info("[tl] used {} MB, target {} MB, ceiling {} MB, line {} MB: {}", MB(a_budget.usage),
                MB(a_budget.target), MB(a_budget.ceiling), MB(a_budget.line), ModeName(a_budget.mode));
        }
    }

    Settings FromIni()
    {
        const auto& ini = GetSettings();

        Settings settings;
        settings.enabled = ini.budget;
        settings.lines.reserve = static_cast<std::uint64_t>(ini.reserveMB) << 20;
        settings.lines.target = static_cast<std::uint64_t>(ini.targetMB) << 20;
        settings.lines.upgradeCeiling = static_cast<float>(ini.upgradeCeiling) / 100.0f;
        settings.lines.upgradePerCycle = static_cast<std::uint64_t>(ini.sourceMaximumIncreaseMB) << 20;
        settings.lines.minimumDecreasePerCycle = static_cast<std::uint64_t>(ini.sourceMinimumDecreaseMB) << 20;
        settings.textureThresholds = ini.sourceTextureThresholds;
        settings.meshThresholds = StarfieldMeshThresholds(ini.sourceMeshScreenSizes,ini.sourceMeshFadePercent,ini.sourceMeshStreamingBias);
        settings.noMeshesUpgradeLimits = ini.sourceNoMeshesUpgradeLimits;
        settings.timeSliceMs = ini.sourceTimeSliceMs;
        settings.plan.unseen = static_cast<Unseen>(ini.unseen);
        settings.plan.policy = static_cast<DetailPolicy>(ini.policy);
        settings.screenSize = ini.screenSize;
        settings.dryRun = ini.dryRun;
        settings.timeline = ini.timeline;
        // StarfieldExact has already zeroed these in the INI settings.
        settings.unseenDelayMs = ini.unseenDelayMs;
        settings.degradeCooldownMs = ini.degradeCooldownMs;
        settings.hysteresis = static_cast<float>(ini.hysteresisPercent) / 100.0f;
        settings.creditPendingFrees = ini.creditPendingFrees;
        settings.headroomNetOfGrowth = ini.QualityGuard();
        settings.fullUpgradeScan = ini.QualityGuard();
        settings.overDebounceFrames = ini.overDebounceFrames;
        settings.adaptiveSampling = ini.adaptiveSampling;
        settings.roomCycleMs = ini.roomCycleMs;
        settings.pressureOnly = ini.pressureOnly;
        settings.raiseCooldownMs = ini.raiseCooldownMs;
        settings.minLoweringBytes = static_cast<std::uint64_t>(ini.minLoweringKB) << 10;
        settings.refillGap = static_cast<std::uint64_t>(ini.refillGapMB) << 20;
        settings.refillCalmMs = ini.refillCalmMs;
        settings.processCounters = ini.processCounters;
        settings.overflowShared = static_cast<std::uint64_t>(ini.overflowSharedMB) << 20;
        settings.autoTarget = ini.autoTarget;
        return settings;
    }

    void Configure(const Settings& a_settings, Listener a_listener)
    {
        vram::SetFrameSampling(a_settings.plan.policy == DetailPolicy::Starfield);
        if (a_settings.processCounters) gpumem::Start();
        g_starfield.store(a_settings.plan.policy == DetailPolicy::Starfield, std::memory_order_relaxed);
        g_adaptive.store(a_settings.adaptiveSampling, std::memory_order_relaxed);
        g_roomCycleMs.store(a_settings.roomCycleMs, std::memory_order_relaxed);
        std::scoped_lock lock(g_lock);
        g_settings = a_settings;
        g_lines = EffectiveLines(g_current.osBudget);
        g_smoother.Reset();
        g_listener = a_listener;
        g_overStreak = 0;
        if (!a_settings.enabled) {
            CloseMode(g_current.mode, Clock::now());
            g_current = a_settings.plan.policy == DetailPolicy::Starfield ? IgnoreBudget(g_current) : Budget{};
        }
        g_mode.store(static_cast<std::uint8_t>(g_current.mode), std::memory_order_relaxed);
    }

    void OnReading(const vram::Reading& a_reading)
    {
        Listener listener = nullptr;
        bool changed = false, timeline = false, keepFreeChanged = false, autoTarget = false;
        Budget logged;
        std::uint64_t largestJump = 0, keepFree = 0;
        {
            std::scoped_lock lock(g_lock);
            if (!g_settings.enabled && g_settings.plan.policy != DetailPolicy::Starfield) {
                return;
            }

            const auto now = Clock::now();
            const auto before = g_current.mode;
            std::uint64_t budget = a_reading.budget;
            if (g_settings.plan.policy == DetailPolicy::Starfield) {
                // Read every few frames, the budget still moves 10 MB a frame.
                if (a_reading.frameSample)
                    budget = g_smoother.Observe(a_reading.frame, budget, g_settings.adaptiveSampling ? kMaxSampleInterval : 1);
                else if (g_smoother.Current() != 0) budget = g_smoother.Current();
                // Poll callbacks are diagnostic/startup fallback, not fake
                // simulation frames and not planner scheduling events.
                if (a_reading.frameSample) listener = g_listener;
            } else listener = g_listener;
            // DXGI can read low; Windows' own counters for this process see
            // every allocation. Paged-out memory that keeps rising means the
            // game is already past its budget.
            std::uint64_t usage = a_reading.usage, overflow = 0;
            if (g_settings.processCounters) {
                const auto process = gpumem::Latest();
                if (process.valid && NowMs() - process.atMs <= kProcessFreshMs) {
                    usage = std::max(usage, process.dedicated);
                    if (g_settings.overflowShared && process.shared >= g_settings.overflowShared) {
                        const auto rise = gpumem::SharedRise(kOverflowWindowMs);
                        if (rise >= kOverflowRise) overflow = rise;
                    }
                }
            }
            // The largest jump in play: none across a loading screen, refill's
            // own rises or a test's pretend usage.
            const auto nowMs = NowMs();
            const auto synthetic = g_syntheticUsage.load(std::memory_order_relaxed);
            const auto loadingSince = g_loadingSinceMs.load(std::memory_order_relaxed);
            if (loadingSince && nowMs - loadingSince < kLoadingAtMostMs) g_lastLoadMs.store(nowMs, std::memory_order_relaxed);
            const bool loading = nowMs - g_lastLoadMs.load(std::memory_order_relaxed) < kLoadGoingMs;
            if (loading || synthetic || g_refilling ||
                nowMs - g_lastRefillMs.load(std::memory_order_relaxed) < kAfterRefillMs || !a_reading.budget) {
                g_meter.Break();
            } else {
                g_meter.Add(nowMs, usage);
                if (g_meter.Ready()) {
                    const auto keep = ReserveMeter::KeepFree(g_meter.Largest(), 0);
                    if (keep > g_keepFree) {
                        g_keepFree = keep;
                        keepFreeChanged = true;
                    }
                }
            }
            usage += synthetic;
            g_lines = EffectiveLines(budget);
            g_current = Assess(g_lines, usage, budget);
            // One reading over the line is often a load spike that passes by
            // itself. The emergency waits until it has held a few readings.
            if (g_current.mode != Mode::Over) g_overStreak = 0;
            else if (++g_overStreak < g_settings.overDebounceFrames) g_current.mode = Mode::Tight;
            // Paging out near the target is an emergency at once: the rise
            // already held for seconds.
            if (overflow && g_current.mode != Mode::Unlimited && usage + g_settings.refillGap / 2 >= g_current.target) {
                g_current.mode = Mode::Over;
                g_current.overflowBytes = overflow;
            }
            if (!g_settings.enabled) g_current = IgnoreBudget(g_current);
            if (g_current.mode == Mode::Tight || g_current.mode == Mode::Over)
                g_lastPressureMs.store(NowMs(), std::memory_order_relaxed);
            g_mode.store(static_cast<std::uint8_t>(g_current.mode), std::memory_order_relaxed);

            // Against the unsmoothed budget: a sudden drop must shorten the
            // sampling interval at once.
            const auto rawTarget = Minus(Minus(a_reading.budget, g_lines.reserve), g_lines.target);
            g_headroom.store(a_reading.budget && !overflow ? Minus(rawTarget, usage) : 0, std::memory_order_relaxed);

            StoreMax(g_peakUsage, usage);
            g_lastLine.store(g_current.line, std::memory_order_relaxed);

            if (g_current.mode != before) {
                CloseMode(before, now);
                if (g_current.mode == Mode::Over) {
                    g_timesOver.fetch_add(1, std::memory_order_relaxed);
                }
                changed = true;
            }
            if (g_settings.timeline && now >= g_nextTimeline) {
                g_nextTimeline = now + kTimelinePeriod;
                timeline = true;
            }
            logged = g_current;
            largestJump = g_meter.Largest();
            keepFree = g_keepFree;
            autoTarget = g_settings.autoTarget;
        }

        // Logged after the lock: this can run on the render thread.
        if (keepFreeChanged) {
            const auto keep = logged.osBudget ? std::min(keepFree, logged.osBudget / 4) : keepFree;
            spdlog::info("Largest video memory jump in play: {} MB within 3 s; {} MB to keep free{}", MB(largestJump),
                MB(keep), autoTarget ? ", the target keeps at least that" : " (AutoTarget is off)");
        }
        if (changed) {
            spdlog::info("Video memory {}: {} MB used, target {} MB, line {} MB{}", ModeName(logged.mode),
                MB(logged.usage), MB(logged.target), MB(logged.line),
                logged.overflowBytes ? std::format(", {} MB paged out in 10 s", MB(logged.overflowBytes)) : "");
        }
        if (timeline) {
            LogTimeline(logged);
        }
        if (listener) {
            listener();
        }
    }

    Budget Current()
    {
        std::scoped_lock lock(g_lock);
        return g_current;
    }

    Mode CurrentMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

    bool CycleDue(Clock::time_point a_lastBegin)
    {
        const auto every = g_roomCycleMs.load(std::memory_order_relaxed);
        return every == 0 || CurrentMode() != Mode::Room ||
               Clock::now() - a_lastBegin >= std::chrono::milliseconds(every);
    }

    void NoteLoad() { g_lastLoadMs.store(NowMs(), std::memory_order_relaxed); }

    void SetLoading(bool a_loading)
    {
        const auto now = NowMs();
        g_loadingSinceMs.store(a_loading ? now : 0, std::memory_order_relaxed);
        g_lastLoadMs.store(now, std::memory_order_relaxed);
    }

    std::uint64_t MeasuredKeepFree()
    {
        std::scoped_lock lock(g_lock);
        return g_keepFree;
    }

    void SetSyntheticUsage(std::uint64_t a_bytes) { g_syntheticUsage.store(a_bytes, std::memory_order_relaxed); }

    RefillState LastRefill()
    {
        std::scoped_lock lock(g_lock);
        return g_lastRefill;
    }

    void OnFrame(std::uint64_t a_frame)
    {
        if (!g_starfield.load(std::memory_order_relaxed)) return;
        if (g_adaptive.load(std::memory_order_relaxed)) {
            // Far below the target, a kernel call every frame buys nothing.
            const auto interval = SampleInterval(g_headroom.load(std::memory_order_relaxed));
            const auto last = g_lastSampleFrame.load(std::memory_order_relaxed);
            if (last && a_frame > last && a_frame - last < interval) return;
        }
        g_lastSampleFrame.store(a_frame, std::memory_order_relaxed);
        OnReading(vram::SampleFrame(a_frame));
    }

    void BeginPlan(StarfieldPlanSession& a_session, std::span<const PlanEntry> a_entries,
        const StreamingAccounting& a_accounting, std::uint64_t a_meshSpent, std::span<const PlanEntry> a_meshEntries)
    {
        Settings settings;
        Budget budget;
        std::uint64_t refill = 0;
        {
            std::scoped_lock lock(g_lock);
            settings = g_settings;
            settings.lines = g_lines;
            budget = settings.enabled || settings.plan.policy == DetailPolicy::Starfield ? g_current : Budget{};
            if (settings.pressureOnly && budget.mode == Mode::Room) {
                // Refill: back to the game's own level only well clear of
                // pressure. It starts with the whole gap under the target free
                // and stops at three quarters of it, after calm, never while
                // memory is still being paged out (SPS 0.20.5, 0.23.0, 1.0.48).
                const auto now = NowMs();
                const auto calm = static_cast<std::int64_t>(settings.refillCalmMs);
                const auto spent = budget.usage + a_accounting.requestedIncreaseBytes;
                const bool quiet = now - g_lastPressureMs.load(std::memory_order_relaxed) >= calm &&
                    now - g_lastLoadMs.load(std::memory_order_relaxed) >= calm &&
                    !(settings.processCounters && settings.overflowShared &&
                      gpumem::SharedRising(kOverflowRise, kRefillSharedWindowMs));
                if (!quiet || (!g_refilling && spent + settings.refillGap > budget.target)) g_refilling = false;
                else {
                    refill = Minus(Minus(budget.target, settings.refillGap / 4 * 3), spent);
                    g_refilling = refill > 0;
                }
                if (g_refilling) g_lastRefillMs.store(now, std::memory_order_relaxed);
                if (settings.timeline) {
                    const auto since = [now](std::int64_t a_then) { return a_then ? now - a_then : -1; };
                    g_lastRefill = RefillState{ true, quiet, g_refilling, refill,
                        since(g_lastLoadMs.load(std::memory_order_relaxed)),
                        since(g_lastPressureMs.load(std::memory_order_relaxed)),
                        settings.processCounters ? gpumem::SharedRise(kRefillSharedWindowMs) : 0, spent, budget.target };
                }
            } else {
                g_refilling = false;
                if (settings.timeline) g_lastRefill = RefillState{};
            }
        }
        StarfieldCycleInput input;
        input.pressureOnly = settings.pressureOnly;
        input.refillBytes = refill;
        input.raiseCooldownMs = settings.raiseCooldownMs;
        input.minLoweringBytes = settings.minLoweringBytes;
        input.accounting = a_accounting;
        input.meshUpgradeBytes = a_meshSpent;
        input.unseen = settings.plan.unseen;
        input.screenSize = settings.screenSize;
        input.textureThresholds = settings.textureThresholds;
        input.meshThresholds = settings.meshThresholds;
        input.noMeshesUpgradeLimits = settings.noMeshesUpgradeLimits;
        input.unseenDelayMs = settings.unseenDelayMs;
        input.degradeCooldownMs = settings.degradeCooldownMs;
        input.hysteresis = settings.hysteresis;
        input.creditPendingFrees = settings.creditPendingFrees;
        input.headroomNetOfGrowth = settings.headroomNetOfGrowth;
        input.fullUpgradeScan = settings.fullUpgradeScan;
        if (settings.dryRun && budget.mode != Mode::Unlimited) {
            // Explicit diagnostic mode only. No backend callback, no resource
            // state changes and no additions to the real request ledger.
            StarfieldPlanSession hypothetical;
            hypothetical.Begin(a_entries, budget, settings.lines, input, a_meshEntries);
            const RequestSink hypotheticalSink = [](void*, const PlanRequest& request) {
                return RequestReceipt{ RequestStatus::AcceptedPending, request.estimatedBytes };
            };
            while (!hypothetical.Step(hypotheticalSink,nullptr,100,hypotheticalSink,nullptr)) {}
            const auto now = Clock::now();
            if (now >= g_nextPlanLog) {
                g_nextPlanLog = now + kTimelinePeriod;
                const auto& result = hypothetical.Result();
                spdlog::info("Starfield dry run ({}): hypothetical down {} MB, up {} MB, emergency {} MB, "
                    "balance down/up {}/{} MB, mesh down/up {}/{} MB (charged down {} MB), "
                    "decrease remaining {} MB; actual requests use full detail",
                    ModeName(result.mode), MB(result.lowered), MB(result.raised), MB(result.emergency),
                    MB(result.balanceLowered), MB(result.balanceRaised), MB(result.meshLowered), MB(result.meshRaised),
                    MB(result.meshDecreaseCharged), MB(result.decreaseRemaining));
            }
            budget = IgnoreBudget(budget);
        }
        a_session.Begin(a_entries, budget, settings.lines, input, a_meshEntries);
    }

    void RecordPlan(const PlanResult& a_result)
    {
        static std::atomic_bool warnedBudgetFallback{};
        if (a_result.usedBudgetFallback && !warnedBudgetFallback.exchange(true))
            spdlog::warn("Starfield refresh-all has no observed GPU budget; using configured finite increase slice "
                "as a port fallback, not source-equivalent budget evidence");
        g_emergencyBytes.fetch_add(a_result.emergency, std::memory_order_relaxed);
        g_tradedBytes.fetch_add(a_result.traded, std::memory_order_relaxed);
        if (a_result.swapped) g_swaps.fetch_add(1, std::memory_order_relaxed);
        if (a_result.upgradesHeld) g_heldCycles.fetch_add(1, std::memory_order_relaxed);
        spdlog::debug("Starfield plan ({}): accepted {} requests, stale {}, down {} MB, up {} MB, "
            "decrease target {} MB, remaining {} MB, engine releases awaiting destruction {} MB, "
            "balance down/up {}/{} MB (request bytes, not physical VRAM frees)",
            ModeName(a_result.mode), a_result.acceptedRequests, a_result.staleRequests, MB(a_result.lowered),
            MB(a_result.raised), MB(a_result.decreaseTarget), MB(a_result.decreaseRemaining),
            MB(a_result.releasedAwaitingDestruction), MB(a_result.balanceLowered), MB(a_result.balanceRaised));
        spdlog::debug("Starfield meshes: accepted {}, stale {}, up/down {}/{} MB, decrease charged {} MB, "
            "emergency {} MB, held {}, carry-in {} MB (accepted logical work, not physical frees)",
            a_result.meshAcceptedRequests,a_result.meshStaleRequests,MB(a_result.meshRaised),MB(a_result.meshLowered),
            MB(a_result.meshDecreaseCharged),MB(a_result.meshEmergency),a_result.meshUpgradesHeld,MB(a_result.meshSpendingCarryIn));
    }

    bool StepPlan(StarfieldPlanSession& a_session, RequestSink a_sink, void* a_context,
        RequestSink a_meshSink, void* a_meshContext)
    {
        double timeSlice;
        {
            std::scoped_lock lock(g_lock);
            timeSlice = g_settings.timeSliceMs;
        }
        return a_session.StepFor(a_sink,a_context,timeSlice,a_meshSink,a_meshContext);
    }

    PlanResult PlanCycle(std::span<PlanEntry> a_entries)
    {
        Settings settings;
        Budget   budget;
        {
            std::scoped_lock lock(g_lock);
            settings = g_settings;
            settings.lines = g_lines;
            budget = g_settings.enabled || settings.plan.policy == DetailPolicy::Starfield ? g_current : Budget{};
        }

        // A cycle runs up to ten times a second: one info line a second at most.
        const auto now = Clock::now();
        const bool talk = now >= g_nextPlanLog;

        PlanResult result;
        if (settings.dryRun && budget.mode != Mode::Unlimited) {
            // What the limits would have done, on a copy, then no limits.
            std::vector<PlanEntry> copy(a_entries.begin(), a_entries.end());
            result = Plan(copy, budget, settings.lines, settings.plan);
            if (talk && (result.emergency || result.swapped || result.upgradesHeld)) {
                g_nextPlanLog = now + kTimelinePeriod;
                spdlog::info("Dry run ({}): would free {} MB in an emergency, trade {} MB for a swap, hold {} upgrades",
                    ModeName(result.mode), MB(result.emergency), MB(result.traded), result.upgradesHeld);
            }
            Plan(a_entries, IgnoreBudget(budget), settings.lines, settings.plan);
        } else {
            result = Plan(a_entries, budget, settings.lines, settings.plan);
        }

        g_emergencyBytes.fetch_add(result.emergency, std::memory_order_relaxed);
        g_tradedBytes.fetch_add(result.traded, std::memory_order_relaxed);
        if (result.swapped) {
            g_swaps.fetch_add(1, std::memory_order_relaxed);
        }
        if (result.upgradesHeld) {
            g_heldCycles.fetch_add(1, std::memory_order_relaxed);
        }
        if (talk && result.emergency && !settings.dryRun) {
            g_nextPlanLog = now + kTimelinePeriod;
            spdlog::info("Emergency: {} MB over the line, lowering {} MB, smallest on screen first",
                MB(budget.usage > budget.line ? budget.usage - budget.line : 0), MB(result.emergency));
        }
        spdlog::debug("Plan ({}): lowered {} MB, raised {} MB, traded {} MB, emergency {} MB, {} upgrades held, "
                      "{} MB still to free, {} MB still to load",
            ModeName(result.mode), MB(result.lowered), MB(result.raised), MB(result.traded), MB(result.emergency),
            result.upgradesHeld, MB(result.pendingFree), MB(result.pendingGrow));
        return result;
    }

    void LogSummary()
    {
        const auto line = g_lastLine.load(std::memory_order_relaxed);
        if (line == 0) {
            return;
        }

        std::uint64_t largestJump = 0, keepFree = 0;
        std::int64_t playMs = 0;
        {
            std::scoped_lock lock(g_lock);
            CloseMode(g_current.mode, Clock::now());
            largestJump = g_meter.Largest();
            playMs = g_meter.PlayMs();
            keepFree = g_keepFree;
        }
        spdlog::info("Largest video memory jump in {:.1f} min of play: {} MB within 3 s; {}",
            static_cast<double>(playMs) / 60000.0, MB(largestJump),
            keepFree ? std::format("{} MB to keep free", MB(keepFree)) : std::string("measuring for 3 min first"));

        spdlog::info(
            "Video memory peaked at {} MB against a line of {} MB; tight for {:.1f} s, over the line {} times for "
            "{:.1f} s; {} MB planned in emergencies, {} swaps planned with {} MB, upgrades held in {} cycles",
            MB(g_peakUsage.load(std::memory_order_relaxed)), MB(line),
            static_cast<double>(g_tightMs.load(std::memory_order_relaxed)) / 1000.0,
            g_timesOver.load(std::memory_order_relaxed),
            static_cast<double>(g_overMs.load(std::memory_order_relaxed)) / 1000.0,
            MB(g_emergencyBytes.load(std::memory_order_relaxed)), g_swaps.load(std::memory_order_relaxed),
            MB(g_tradedBytes.load(std::memory_order_relaxed)), g_heldCycles.load(std::memory_order_relaxed));
    }
}
