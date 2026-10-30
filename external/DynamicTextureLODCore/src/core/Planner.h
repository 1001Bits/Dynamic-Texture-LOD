#pragma once

#include "TextureFormat.h"
#include "StarfieldPolicy.h"

#include <array>
#include <cstdint>
#include <span>

// Game-independent planning data. Pixels retains the original one-cycle
// policy; Starfield selects the separately resumable StarfieldPlanSession.
// Backends should use that session with generation-checked request receipts.
// Plan() remains a synchronous planning-only adapter, accepting hypothetical
// pending requests. Byte counts never establish measured physical VRAM frees.
namespace dtl
{
    // Starfield's budget modes.
    enum class Mode : std::uint8_t
    {
        Unlimited = 0,  // budget off/no reading: Starfield refresh-all with finite per-cycle caps
        Room = 1,       // under the target
        Tight = 2,      // between the target and the budget line
        Over = 3        // over the budget line: emergency
    };

    const char* ModeName(Mode a_mode);

    // Defaults are Starfield's, from its Textures and LOD settings.
    struct BudgetLines
    {
        // uiMinimumFreeBudget: kept free under the OS budget. The budget line.
        std::uint64_t reserve = 128ull << 20;
        // uiUnderBudgetThresholdV2: normal use stays this far under the line.
        std::uint64_t target = 512ull << 20;
        // fTextureLODScreenSizeUpgradeTargetPercent: upgrades stop this far
        // from the target towards the line.
        float upgradeCeiling = 0.65f;
        // uiMaximumIncreasePerIterationScreenSize: upgrades per cycle.
        std::uint64_t upgradePerCycle = 34ull << 20;
        std::uint64_t minimumDecreasePerCycle = 34ull << 20;
    };

    // One reading weighed against the lines.
    struct Budget
    {
        Mode          mode = Mode::Unlimited;
        std::uint64_t usage = 0;
        std::uint64_t line = 0;     // OS budget minus the reserve
        std::uint64_t target = 0;   // the line minus the target
        std::uint64_t ceiling = 0;  // upgrades stop above this
        std::uint64_t osBudget = 0; // observed/smoothed budget before reserve, including ignore mode
        // Memory the OS paged out of video memory recently, while still
        // paging. It makes the mode Over even under the line: the reading
        // has already missed what was pushed out.
        std::uint64_t overflowBytes = 0;
    };

    // An OS budget of zero means no reading: Unlimited.
    Budget Assess(const BudgetLines& a_lines, std::uint64_t a_usage, std::uint64_t a_osBudget);
    // Source ignore mode uses B=T=unreserved budget. Keep observed usage for
    // telemetry; the mode0 planner does not use it for pressure arithmetic.
    inline Budget IgnoreBudget(Budget budget)
    {
        budget.mode = Mode::Unlimited;
        budget.line = budget.target = budget.ceiling = budget.osBudget;
        return budget;
    }

    // What happens to textures nothing on screen is using.
    enum class Unseen : std::uint8_t
    {
        Keep = 0,           // stay where they are
        UnderPressure = 1,  // go to their lowest level once memory is tight
        Lowest = 2,         // always go to their lowest level, as in Starfield
        // Planned like visible textures, by their size at their distance:
        // view direction plays no part, so turning around reloads nothing.
        ByDistance = 3
    };

    inline constexpr std::uint32_t kMaxPlanLevel = 4;
    // An elapsed time that is unknown or longer than any delay.
    inline constexpr std::uint32_t kLongAgoMs = 0xFFFFFFFFu;

    struct PlanEntry
    {
        // Filled by the backend.
        std::uint64_t key = 0;
        float         priority = 0.0f;  // LodPolicy Priority, for seen textures
        bool          seen = false;     // drawn, or on screen, recently
        std::uint8_t  current = 0;      // level now
        std::uint8_t  previous = 0;     // level planned last cycle, or current if new
        std::uint8_t  wanted = 0;       // the screen-size level, for seen textures
        std::uint8_t  lowest = 0;       // the most it can drop; 0 pins it
        std::array<std::uint64_t, kMaxPlanLevel + 1> bytes{};  // resident at each level

        // Filled by Plan.
        std::uint8_t planned = 0;
        // Appended to preserve legacy aggregate initializers.
        std::uint64_t generation = 0;
        std::uint64_t resourceIdentity = 0;
        bool pending = false;
        bool noDegrade = false;

        // Quality guard, filled by the backend. A level below minLevel is
        // more detail than the game itself would show: never planned. While
        // seen, a level above seenCap could show on screen: only an emergency
        // goes past it. The defaults constrain nothing.
        std::uint8_t  minLevel = 0;
        std::uint8_t  seenCap = kMaxPlanLevel;
        // Since the texture was last seen, and since its level last changed.
        std::uint32_t unseenMs = kLongAgoMs;
        std::uint32_t sinceChangeMs = kLongAgoMs;
        // Since its level was last raised: lowering it again soon after
        // undoes a file read that was just paid for.
        std::uint32_t sinceRaiseMs = kLongAgoMs;
    };

    // Resident bytes at each level, for an entry.
    inline void FillBytes(PlanEntry& a_entry, std::uint8_t a_format, std::uint32_t a_width, std::uint32_t a_height,
        std::uint32_t a_mips)
    {
        for (std::uint32_t level = 0; level <= kMaxPlanLevel; ++level) {
            a_entry.bytes[level] = level < a_mips ?
                                       TextureFormat::ChainBytes(a_format, a_width, a_height, level, a_mips - level) :
                                       0;
        }
    }

    struct PlanSettings
    {
        Unseen unseen = Unseen::UnderPressure;
        DetailPolicy policy = DetailPolicy::Pixels; // legacy API default; INI defaults to Starfield
    };

    struct PlanResult
    {
        Mode          mode = Mode::Unlimited;
        std::uint64_t pendingFree = 0;   // lowerings planned before, not done yet
        std::uint64_t pendingGrow = 0;   // raisings planned before, not done yet
        std::uint64_t lowered = 0;       // freed going to wanted or unseen levels
        std::uint64_t raised = 0;        // spent on upgrades, including a swap's
        std::uint64_t traded = 0;        // freed to pay for a swap
        std::uint64_t emergency = 0;     // freed by the emergency walk
        std::uint32_t upgradesHeld = 0;  // wanted to come up but weren't paid for
        bool          swapped = false;
        std::uint64_t decreaseTarget = 0;
        std::uint64_t decreaseRemaining = 0;
        std::uint64_t releasedAwaitingDestruction = 0;
        std::uint64_t balanceLowered = 0;
        std::uint64_t balanceRaised = 0;
        std::uint32_t acceptedRequests = 0;
        std::uint32_t staleRequests = 0;
        bool usedBudgetFallback = false;
        std::uint64_t meshRaised = 0, meshLowered = 0, meshDecreaseCharged = 0, meshEmergency = 0;
        std::uint64_t meshSpendingCarryIn = 0;
        std::uint32_t meshAcceptedRequests = 0, meshStaleRequests = 0, meshUpgradesHeld = 0;
    };

    // Plans one cycle. Sorts a_entries, most important first, and fills each
    // one's planned level.
    PlanResult Plan(std::span<PlanEntry> a_entries, const Budget& a_budget, const BudgetLines& a_lines,
        const PlanSettings& a_settings);
}
