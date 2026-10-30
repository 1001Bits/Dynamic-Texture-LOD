#pragma once

#include "Planner.h"
#include "StreamingLedger.h"

#include <cstddef>
#include <memory>
#include <span>

namespace dtl
{
    enum class PlanStage : std::uint8_t { Idle, Upgrade, Degrade, BalanceScan, BalanceCosts, BalanceDegrade, Done, MeshUpgrade, MeshDegrade };
    enum class RequestReason : std::uint8_t { Upgrade, Degrade, Emergency, SwapDegrade, SwapUpgrade, BalanceDegrade, BalanceUpgrade,
        MeshUpgrade, MeshDegrade, MeshEmergency };
    enum class RequestStatus : std::uint8_t { Rejected, AcceptedPending, Completed, Stale };

    struct PlanRequest
    {
        TextureIdentity identity;
        std::uint64_t resourceIdentity = 0;
        std::uint8_t current = 0, target = 0;
        std::uint64_t estimatedBytes = 0;
        RequestReason reason = RequestReason::Degrade;
    };
    struct RequestReceipt
    {
        RequestStatus status = RequestStatus::Rejected;
        std::uint64_t bytes = 0;  // accepted request delta; not completed free bytes
    };
    using RequestSink = RequestReceipt (*)(void* context, const PlanRequest& request);

    // Optional already-accepted mesh work before this session's snapshots.
    // Do not also report requests handled by this session's mesh sink: those
    // are charged directly, once. Destruction accounting is texture-only.
    struct StarfieldCycleInput
    {
        StreamingAccounting accounting;
        bool noMeshesUpgradeLimits = true;
        std::uint64_t meshUpgradeBytes = 0;
        Unseen unseen = Unseen::Lowest;
        bool screenSize = true;
        std::array<float,4> textureThresholds = kStarfieldTextureThresholds;
        std::array<float,4> meshThresholds = kStarfieldMeshThresholds;

        // Departures for engines where every level change is a re-create and
        // raising one re-reads its file. The defaults are the source's.
        std::uint32_t unseenDelayMs = 0;      // out of view this long before it counts as unseen
        std::uint32_t degradeCooldownMs = 0;  // no lowering this soon after a change, except an emergency
        float hysteresis = 0.0f;              // a lowering needs the rank this fraction under its threshold
        bool creditPendingFrees = false;      // recently requested lowerings count against an overage
        bool headroomNetOfGrowth = false;     // upgrade headroom net of raisings still on their way
        bool fullUpgradeScan = false;         // look at every entry for upgrades, not just the high ranks

        // Pressure first, as Skyrim Performance Solution learned: with room to
        // spare nothing is lowered, a visible texture held past what the
        // screen shows comes back at once, and the rest come back to the
        // game's level only through refill.
        bool pressureOnly = false;
        // Bytes refill may still bring back this cycle; 0 while refill waits
        // for calm or room. Only read with pressureOnly.
        std::uint64_t refillBytes = 0;
        std::uint32_t raiseCooldownMs = 0;    // no lowering this soon after a raise, except an emergency
        // Each change is a re-create on the engine's one creation thread, which
        // the frame waits on: a lowering that frees less than this is skipped.
        std::uint64_t minLoweringBytes = 0;   // 0 off, as the source
    };

    // Callback accepts work only if key/generation/resource/current level are
    // still valid. Rejected/stale operations spend no byte allowance. The
    // session owns value snapshots and never dereferences engine objects.
    bool MatchesRequest(const PlanEntry& live, const PlanRequest& request);

    class StarfieldPlanSession
    {
    public:
        StarfieldPlanSession();
        ~StarfieldPlanSession();
        StarfieldPlanSession(StarfieldPlanSession&&) noexcept;
        StarfieldPlanSession& operator=(StarfieldPlanSession&&) noexcept;
        StarfieldPlanSession(const StarfieldPlanSession&) = delete;
        StarfieldPlanSession& operator=(const StarfieldPlanSession&) = delete;
        void Begin(std::span<const PlanEntry> entries, const Budget& budget, const BudgetLines& lines,
                   const StarfieldCycleInput& input = {}, std::span<const PlanEntry> meshes = {});
        // At most maxEntries visits. Source upgrade/degrade visits are 100;
        // Balance is also split here to keep a large prefix bounded.
        bool Step(RequestSink sink, void* context, std::size_t maxEntries = 100,
                  RequestSink meshSink = nullptr, void* meshContext = nullptr);
        // Source scheduler revisits 100-entry chunks until its deadline. The
        // source time-setting unit is inferred as ms; this port uses steady
        // clock milliseconds. A chunk may overshoot; zero runs one chunk.
        bool StepFor(RequestSink sink, void* context, double milliseconds,
                     RequestSink meshSink = nullptr, void* meshContext = nullptr);
        void Reset();
        bool Done() const;
        PlanStage Stage() const;
        const PlanResult& Result() const;
        std::span<const PlanEntry> Entries() const;
        // Mesh resourceIdentity is a backend's monotonic VB/IB pair revision.
        // Separate sink and Mesh* reasons prevent cross-kind key collisions.
        // Byte tables are actual retained VB+IB totals, never guessed halves.
        std::span<const PlanEntry> MeshEntries() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
