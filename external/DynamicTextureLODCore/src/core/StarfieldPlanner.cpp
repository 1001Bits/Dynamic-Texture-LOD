#include "StarfieldPlanner.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <vector>

namespace dtl
{
    namespace
    {
        constexpr auto kNone = std::numeric_limits<std::size_t>::max();
        constexpr auto kUnlimited = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t Minus(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : 0; }
        std::uint64_t Add(std::uint64_t a, std::uint64_t b) { return b > kUnlimited - a ? kUnlimited : a + b; }
        std::uint64_t Delta(const PlanEntry& entry, std::uint8_t target)
        {
            const auto from = entry.bytes[entry.current], to = entry.bytes[target];
            return from > to ? from - to : to - from;
        }
    }

    bool MatchesRequest(const PlanEntry& live, const PlanRequest& request)
    {
        return live.key == request.identity.key && live.generation == request.identity.generation &&
            live.resourceIdentity == request.resourceIdentity && live.current == request.current && !live.pending &&
            request.target <= kMaxPlanLevel &&
            (request.target <= request.current || (request.target <= live.lowest && !live.noDegrade));
    }

    struct StarfieldPlanSession::Impl
    {
        std::vector<PlanEntry> entries, meshes;
        Budget budget;
        StarfieldCycleInput input;
        PlanResult result;
        PlanStage stage = PlanStage::Idle;
        std::uint64_t meshAllowance = 0, textureAllowance = 0, combinedAllowance = 0;
        bool emergency = false, swap = false, refresh = false;
        std::size_t cursor = 0, swapIndex = kNone;
        std::uint64_t swapCost = 0, swapFreed = 0;
        std::size_t prefixEnd = 0, balanceIndex = kNone;
        bool balanceFrozen = false;
        std::uint64_t balanceEstimate = 0, balanceFreed = 0;
        std::array<std::uint64_t, kMaxPlanLevel + 1> balanceCosts{};
        std::uint8_t balanceTarget = 0;

        // Never more detail than the game's own level, never past lowest.
        static std::uint8_t Bound(const PlanEntry& entry, std::uint8_t level)
        {
            return std::min(std::max(level, entry.minLevel), entry.lowest);
        }
        static std::uint8_t Hold(const PlanEntry& entry) { return std::max(entry.current, entry.minLevel); }

        std::uint8_t Wanted(const PlanEntry& entry) const
        {
            if (refresh) return Bound(entry, 0);
            // Sized: planned by its size on screen. By distance, unseen
            // textures are sized too, with the rank of every visit.
            const bool byDistance = input.unseen == Unseen::ByDistance;
            const bool sized = entry.seen || byDistance;
            if (input.pressureOnly && budget.mode == Mode::Room) {
                // Nothing to save with room to spare. A sized texture held
                // past what the screen shows comes back at once; the rest
                // return to the game's level only when refill allows.
                if (sized && entry.current > entry.seenCap) return Bound(entry, entry.seenCap);
                return input.refillBytes ? Bound(entry, 0) : Hold(entry);
            }
            // Out of view only briefly: hold, so turning around costs nothing.
            const bool unseen = !sized && entry.unseenMs >= input.unseenDelayMs;
            if (!sized && !unseen) return Hold(entry);
            if (unseen && (input.unseen == Unseen::Keep ||
                (input.unseen == Unseen::UnderPressure && budget.mode == Mode::Room))) return Hold(entry);
            std::uint8_t level;
            if (unseen && input.unseen == Unseen::UnderPressure) level = entry.lowest;
            else if (!input.screenSize && sized) level = 0;
            else {
                // Rank includes the source's bias-zero exemption, which may keep
                // a texture important even when its current cycle was not seen.
                level = StarfieldWantedLevel(entry.priority, entry.lowest, input.textureThresholds);
                if (sized) {
                    if (level > entry.current && input.hysteresis > 0.0f) {
                        // A lowering needs the rank well under its threshold.
                        const float boosted = entry.priority / (1.0f - input.hysteresis);
                        level = std::max(StarfieldWantedLevel(boosted, entry.lowest, input.textureThresholds), entry.current);
                    }
                    // Never past what the screen can show at its size.
                    level = std::min(level, entry.seenCap);
                }
            }
            if (level > entry.current && !MayLowerNow(entry)) return Hold(entry);
            return Bound(entry, level);
        }

        // A recent change, or a recent raise, holds off another lowering,
        // except over the line.
        bool MayLowerNow(const PlanEntry& entry) const
        {
            return (entry.sinceChangeMs >= input.degradeCooldownMs && entry.sinceRaiseMs >= input.raiseCooldownMs) ||
                   budget.mode == Mode::Over;
        }
        // Swap and balance donors trade detail; only where it can't be seen.
        bool MayDonateTo(const PlanEntry& entry, std::uint8_t level) const
        {
            const bool sized = entry.seen || input.unseen == Unseen::ByDistance;
            return (!sized || level <= entry.seenCap) && MayLowerNow(entry);
        }

        std::uint8_t MeshWanted(const PlanEntry& entry) const
        {
            return refresh ? 0 : StarfieldWantedLevel(entry.priority, entry.lowest, input.meshThresholds);
        }

        void RequestMesh(std::size_t index, std::uint8_t target, RequestReason reason,
            RequestSink sink, void* context)
        {
            auto& entry = meshes[index];
            if (!sink || entry.pending || target == entry.current || target > kMaxPlanLevel ||
                (target > entry.current && (target > entry.lowest || entry.noDegrade))) return;
            const bool down = target > entry.current;
            // Distinct mesh levels may retain the same VB/IB capacities. A
            // zero-byte logical change is valid; reversed byte tables are not.
            if ((down && entry.bytes[target] > entry.bytes[entry.current]) ||
                (!down && entry.bytes[target] < entry.bytes[entry.current])) return;
            const auto wanted = MeshWanted(entry);
            const PlanRequest request{ { entry.key, entry.generation }, entry.resourceIdentity,
                entry.current, target, Delta(entry, target), reason };
            const auto receipt = sink(context, request);
            if (receipt.status == RequestStatus::Stale) {
                ++result.meshStaleRequests;
                entry.pending = true;
                return;
            }
            if (receipt.status != RequestStatus::AcceptedPending && receipt.status != RequestStatus::Completed) return;
            ++result.meshAcceptedRequests;
            entry.planned = target;
            if (receipt.status == RequestStatus::AcceptedPending) entry.pending = true;
            else entry.current = target;
            if (down) {
                result.meshLowered = Add(result.meshLowered, receipt.bytes);
                if (!input.noMeshesUpgradeLimits) {
                    result.meshDecreaseCharged = Add(result.meshDecreaseCharged, receipt.bytes);
                    result.decreaseRemaining = Minus(result.decreaseRemaining, receipt.bytes);
                }
                if (reason == RequestReason::MeshEmergency) result.meshEmergency = Add(result.meshEmergency, receipt.bytes);
            } else {
                if (request.current > wanted && target <= wanted && result.meshUpgradesHeld > 0)
                    --result.meshUpgradesHeld;
                result.meshRaised = Add(result.meshRaised, receipt.bytes);
                combinedAllowance = Minus(combinedAllowance, receipt.bytes);
                if (!input.noMeshesUpgradeLimits) meshAllowance = Minus(meshAllowance, receipt.bytes);
            }
        }

        void StartMeshDegrade()
        {
            cursor = meshes.size();
            stage = result.decreaseRemaining == 0 || meshes.empty() ? PlanStage::Done : PlanStage::MeshDegrade;
        }

        std::uint64_t Request(std::size_t index, std::uint8_t target, RequestReason reason,
            RequestSink sink, void* context)
        {
            auto& entry = entries[index];
            if (!sink || entry.pending || target == entry.current || target > kMaxPlanLevel ||
                (target > entry.current && (target > entry.lowest || entry.noDegrade))) return 0;
            // Invalid byte descriptions cannot be charged as a change in the
            // opposite direction. Retain the resource instead.
            const bool down = target > entry.current;
            if ((down && entry.bytes[target] >= entry.bytes[entry.current]) ||
                (!down && entry.bytes[target] <= entry.bytes[entry.current])) return 0;
            if (!down && target < entry.minLevel) return 0;  // more detail than the game's own
            if (down && Delta(entry, target) < input.minLoweringBytes) return 0;  // not worth its re-create
            const PlanRequest request{ { entry.key, entry.generation }, entry.resourceIdentity,
                entry.current, target, Delta(entry, target), reason };
            const auto receipt = sink(context, request);
            if (receipt.status == RequestStatus::Stale) {
                ++result.staleRequests;
                entry.pending = true; // invalidate this snapshot for every later stage
                return 0;
            }
            if (receipt.status != RequestStatus::AcceptedPending && receipt.status != RequestStatus::Completed) return 0;
            ++result.acceptedRequests;
            entry.planned = target;
            if (receipt.status == RequestStatus::AcceptedPending) entry.pending = true;
            else entry.current = target;
            // This is accepted logical work, not completed physical release.
            if (down) {
                result.lowered = Add(result.lowered, receipt.bytes);
                result.decreaseRemaining = Minus(result.decreaseRemaining, receipt.bytes);
                if (reason == RequestReason::Emergency) result.emergency = Add(result.emergency, receipt.bytes);
                if (reason == RequestReason::SwapDegrade) result.traded = Add(result.traded, receipt.bytes);
                if (reason == RequestReason::BalanceDegrade) result.balanceLowered = Add(result.balanceLowered, receipt.bytes);
            } else {
                if (request.current > Wanted(entry) && target <= Wanted(entry) && result.upgradesHeld > 0)
                    --result.upgradesHeld;
                result.raised = Add(result.raised, receipt.bytes);
                textureAllowance = Minus(textureAllowance, receipt.bytes);
                combinedAllowance = Minus(combinedAllowance, receipt.bytes);
                if (reason == RequestReason::BalanceUpgrade) result.balanceRaised = Add(result.balanceRaised, receipt.bytes);
            }
            return receipt.bytes;
        }

        void StartDegrade()
        {
            if (result.decreaseRemaining == 0) { stage = PlanStage::Done; return; }
            if (entries.empty()) { StartBalance(); return; }
            cursor = entries.size();
            stage = PlanStage::Degrade;
        }

        void StartBalance()
        {
            if (!swap || result.decreaseRemaining == 0) { StartMeshDegrade(); return; }
            stage = PlanStage::BalanceScan;
            cursor = 0;
        }

        void FinishBalance(RequestSink sink, void* context)
        {
            auto& candidate = entries[balanceIndex];
            // Failed/no-degrade donor requests may make the estimated best
            // level unaffordable. Source falls back to the first level paid
            // for by accepted work; unlike swap mode, equality is enough.
            for (std::uint8_t level = std::max(balanceTarget, candidate.minLevel); level < candidate.current; ++level) {
                if (balanceCosts[level] <= balanceFreed) {
                    Request(balanceIndex, level, RequestReason::BalanceUpgrade, sink, context);
                    break;
                }
            }
            StartMeshDegrade();
        }
    };

    StarfieldPlanSession::StarfieldPlanSession() : impl_(std::make_unique<Impl>()) {}
    StarfieldPlanSession::~StarfieldPlanSession() = default;
    StarfieldPlanSession::StarfieldPlanSession(StarfieldPlanSession&&) noexcept = default;
    StarfieldPlanSession& StarfieldPlanSession::operator=(StarfieldPlanSession&&) noexcept = default;

    void StarfieldPlanSession::Begin(std::span<const PlanEntry> entries, const Budget& budget,
        const BudgetLines& lines, const StarfieldCycleInput& input, std::span<const PlanEntry> meshes)
    {
        Reset();
        auto& s = *impl_;
        s.entries.assign(entries.begin(), entries.end());
        s.meshes.assign(meshes.begin(), meshes.end());
        s.budget = budget;
        s.input = input;
        if (!ValidStarfieldThresholds(s.input.textureThresholds)) s.input.textureThresholds = kStarfieldTextureThresholds;
        if (!ValidStarfieldThresholds(s.input.meshThresholds)) s.input.meshThresholds = kStarfieldMeshThresholds;
        s.result.mode = budget.mode;
        s.result.pendingFree = input.accounting.requestedDecreaseBytes;
        s.result.pendingGrow = input.accounting.requestedIncreaseBytes;
        s.result.releasedAwaitingDestruction = input.accounting.releasedAwaitingDestructionBytes;
        s.result.meshSpendingCarryIn = input.meshUpgradeBytes;
        const auto prepare = [](std::vector<PlanEntry>& list) {
        for (auto& entry : list) {
            entry.current = std::min<std::uint8_t>(entry.current, kMaxPlanLevel);
            entry.lowest = std::min<std::uint8_t>(entry.lowest, kMaxPlanLevel);
            entry.minLevel = std::min(entry.minLevel, entry.lowest);
            entry.seenCap = std::min<std::uint8_t>(entry.seenCap, kMaxPlanLevel);
            entry.planned = entry.current;
            // A malformed rank must never put a resource into the low-priority
            // release tail; pin it for this snapshot instead.
            if (!std::isfinite(entry.priority)) { entry.priority = 3.0f; entry.noDegrade = true; }
        }
        std::ranges::sort(list, [](const PlanEntry& a, const PlanEntry& b) {
            return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
        });
        };
        prepare(s.entries);
        prepare(s.meshes);
        if (budget.mode == Mode::Unlimited) {
            s.refresh = true;
            // CheckBudget FUN_142a4bfa0 mode0: B for each resource class,
            // 2B shared. Ignore-budget changes wanted to full detail; it does
            // not remove the per-cycle workload cap. Accepted meshes spend
            // from the shared allowance before texture upgrades run.
            // Here B is tracker+8: the unreserved GPU budget under the ignore
            // flag, NOT uiMaximumIncreasePerIterationScreenSize. No valid
            // observation uses a bounded port fallback, explicitly reported.
            s.result.usedBudgetFallback = budget.osBudget == 0;
            const auto allowance = budget.osBudget ? budget.osBudget : lines.upgradePerCycle;
            s.meshAllowance = input.noMeshesUpgradeLimits ? allowance : Minus(allowance,input.meshUpgradeBytes);
            s.textureAllowance = allowance;
            s.combinedAllowance = Minus(Add(allowance,allowance),input.meshUpgradeBytes);
        } else if (budget.mode == Mode::Over) {
            s.meshAllowance = input.noMeshesUpgradeLimits ? lines.upgradePerCycle : 0;
            // Paging out can start under the line; then free what was pushed out.
            auto outstanding = std::max(Minus(Minus(budget.usage, budget.line),
                input.accounting.releasedAwaitingDestructionBytes), budget.overflowBytes);
            // Lowerings asked for moments ago are not in the reading yet.
            // Asking again for the whole overage every cycle overshoots.
            if (input.creditPendingFrees) outstanding = Minus(outstanding, input.accounting.recentRequestedDecreaseBytes);
            s.emergency = outstanding > 0;
            s.result.decreaseTarget = outstanding;
        } else {
            const auto usage = input.headroomNetOfGrowth ?
                Add(budget.usage, input.accounting.requestedIncreaseBytes) : budget.usage;
            const auto headroom = Minus(budget.ceiling, usage);
            const auto meshAllowance = std::min(input.noMeshesUpgradeLimits ? budget.line : headroom, lines.upgradePerCycle);
            s.meshAllowance = input.noMeshesUpgradeLimits ? meshAllowance : Minus(meshAllowance,input.meshUpgradeBytes);
            s.textureAllowance = std::min(input.noMeshesUpgradeLimits ? headroom : Minus(headroom, meshAllowance), lines.upgradePerCycle);
            s.combinedAllowance = Minus(std::min(Add(meshAllowance, s.textureAllowance), headroom), input.meshUpgradeBytes);
            if (input.pressureOnly && budget.mode == Mode::Room && input.refillBytes) {
                // Refill fills only part of the room, so it never walks back into pressure.
                s.textureAllowance = std::min(s.textureAllowance, input.refillBytes);
                s.combinedAllowance = std::min(s.combinedAllowance, input.refillBytes);
            }
            auto overTarget = Minus(budget.usage, budget.target);
            if (input.creditPendingFrees) overTarget = Minus(overTarget, input.accounting.recentRequestedDecreaseBytes);
            s.result.decreaseTarget = std::max(overTarget, lines.minimumDecreasePerCycle);
        }
        s.result.decreaseRemaining = s.result.decreaseTarget;
        s.swap = !s.refresh && !s.emergency && s.textureAllowance == 0;
        for (const auto& entry : s.entries)
            if (!entry.pending && entry.current > s.Wanted(entry)) ++s.result.upgradesHeld;
        for (const auto& entry : s.meshes)
            if (!entry.pending && entry.current > s.MeshWanted(entry)) ++s.result.meshUpgradesHeld;
        s.stage = !s.meshes.empty() && (input.noMeshesUpgradeLimits || s.meshAllowance != 0) ?
            PlanStage::MeshUpgrade : PlanStage::Upgrade;
        if (s.entries.empty() && s.meshes.empty()) s.stage = PlanStage::Done;
    }

    bool StarfieldPlanSession::Step(RequestSink sink, void* context, std::size_t maxEntries,
        RequestSink meshSink, void* meshContext)
    {
        auto& s = *impl_;
        if (maxEntries == 0) return Done();
        std::size_t visits = 0;
        while (visits < maxEntries && s.stage != PlanStage::Done && s.stage != PlanStage::Idle) {
            switch (s.stage) {
            case PlanStage::MeshUpgrade: {
                // FUN_142a4c5a0: the source flag bypasses all three stops,
                // including a spent shared allowance. Spending still reduces
                // the allowance used by the later texture upgrade stage.
                if (s.cursor >= s.meshes.size() || (!s.input.noMeshesUpgradeLimits &&
                    (s.emergency || s.meshAllowance == 0 || s.combinedAllowance == 0))) {
                    s.cursor = 0;
                    s.stage = PlanStage::Upgrade;
                    break;
                }
                const auto index = s.cursor++;
                ++visits;
                auto& entry = s.meshes[index];
                if (entry.pending) break;
                const auto wanted = s.MeshWanted(entry);
                if (entry.current > wanted)
                    s.RequestMesh(index,wanted,RequestReason::MeshUpgrade,meshSink,meshContext);
                else if (wanted >= 3) { s.cursor = 0; s.stage = PlanStage::Upgrade; }
                break;
            }
            case PlanStage::Upgrade: {
                if (s.cursor >= s.entries.size() || s.emergency ||
                    (!s.swap && (s.textureAllowance == 0 || s.combinedAllowance == 0))) { s.StartDegrade(); break; }
                const auto index = s.cursor++;
                ++visits;
                auto& entry = s.entries[index];
                if (entry.pending) break;
                const auto wanted = s.Wanted(entry);
                if (entry.current > wanted) {
                    if (s.swap) {
                        s.swapCost = Delta(entry, wanted);
                        if (s.swapCost > 0) { s.swapIndex = index; s.StartDegrade(); }
                    } else s.Request(index, wanted, RequestReason::Upgrade, sink, context);
                } else if (wanted > 2 && !s.input.fullUpgradeScan) s.StartDegrade();
                break;
            }
            case PlanStage::Degrade: {
                if (s.cursor == 0 || s.result.decreaseRemaining == 0) { s.StartBalance(); break; }
                const auto index = --s.cursor;
                ++visits;
                if (index == s.swapIndex) { s.swapIndex = kNone; s.swapCost = s.swapFreed = 0; }
                auto& entry = s.entries[index];
                if (entry.pending) break;
                auto wanted = s.emergency ? entry.lowest : s.Wanted(entry);
                if (s.swapIndex != kNone && wanted < entry.lowest &&
                    s.MayDonateTo(entry, static_cast<std::uint8_t>(wanted + 1))) ++wanted;
                const auto reason = s.emergency ? RequestReason::Emergency :
                    s.swapIndex != kNone ? RequestReason::SwapDegrade : RequestReason::Degrade;
                if (entry.current < wanted) {
                    const auto accepted = s.Request(index, wanted, reason, sink, context);
                    if (s.swapIndex != kNone) {
                        s.swapFreed = Add(s.swapFreed, accepted);
                        // The source uses strictly greater here, not >=.
                        if (s.swapFreed > s.swapCost) {
                            const auto raised = s.Request(s.swapIndex, s.Wanted(s.entries[s.swapIndex]), RequestReason::SwapUpgrade, sink, context);
                            s.result.swapped = raised > 0;
                            s.swapIndex = kNone;
                            s.swapCost = s.swapFreed = 0;
                        }
                    }
                }
                break;
            }
            case PlanStage::BalanceScan: {
                if (s.cursor >= s.entries.size() || s.entries[s.cursor].priority < s.input.textureThresholds[0]) {
                    s.prefixEnd = s.cursor;
                    if (s.balanceIndex == kNone || s.balanceIndex + 1 >= s.prefixEnd) { s.StartMeshDegrade(); break; }
                    auto& candidate = s.entries[s.balanceIndex];
                    for (std::uint8_t level = 0; level < candidate.current; ++level) s.balanceCosts[level] = Delta(candidate, level);
                    s.cursor = s.balanceIndex + 1;
                    s.stage = PlanStage::BalanceCosts;
                    break;
                }
                const auto index = s.cursor++;
                ++visits;
                const auto& entry = s.entries[index];
                if (entry.pending) break;
                const auto best = s.balanceIndex == kNone ? 0 : s.entries[s.balanceIndex].current;
                // FUN_142a4d2b0: keep the earliest largest current level until
                // a later less-degraded non-max entry freezes that candidate.
                if (entry.current != entry.lowest && entry.current < best) s.balanceFrozen = true;
                if (!s.balanceFrozen && entry.current > best) s.balanceIndex = index;
                break;
            }
            case PlanStage::BalanceCosts: {
                if (s.cursor >= s.prefixEnd) {
                    const auto current = s.entries[s.balanceIndex].current;
                    s.balanceTarget = current;
                    for (std::uint8_t level = s.entries[s.balanceIndex].minLevel; level < current; ++level) {
                        if (s.balanceCosts[level] <= s.balanceEstimate) { s.balanceTarget = level; break; }
                    }
                    if (s.balanceTarget == current) { s.StartMeshDegrade(); break; }
                    s.cursor = s.prefixEnd;
                    s.stage = PlanStage::BalanceDegrade;
                    break;
                }
                const auto& entry = s.entries[s.cursor++];
                ++visits;
                if (!entry.pending && entry.current != entry.lowest && entry.current < 2 && entry.lowest > 1 &&
                    s.MayDonateTo(entry, 2))
                    s.balanceEstimate = Add(s.balanceEstimate, Delta(entry, 2));
                break;
            }
            case PlanStage::BalanceDegrade: {
                if (s.cursor <= s.balanceIndex + 1 || s.balanceFreed >= s.balanceCosts[s.balanceTarget]) {
                    s.FinishBalance(sink, context);
                    break;
                }
                const auto index = --s.cursor;
                ++visits;
                const auto& entry = s.entries[index];
                if (!entry.pending && entry.current != entry.lowest && entry.current < 2 && entry.lowest > 1 &&
                    s.MayDonateTo(entry, 2))
                    s.balanceFreed = Add(s.balanceFreed, s.Request(index, 2, RequestReason::BalanceDegrade, sink, context));
                break;
            }
            case PlanStage::MeshDegrade: {
                // FUN_142a4d900 stops on begin or zero remaining, even when
                // the bypass flag prevents mesh demotions paying that target.
                if (s.cursor == 0 || s.result.decreaseRemaining == 0) { s.stage = PlanStage::Done; break; }
                const auto index = --s.cursor;
                ++visits;
                const auto& entry = s.meshes[index];
                if (entry.pending) break;
                const bool forceLowest = s.emergency && !s.input.noMeshesUpgradeLimits;
                const auto wanted = forceLowest ? entry.lowest : s.MeshWanted(entry);
                if (entry.current < wanted)
                    s.RequestMesh(index,wanted,forceLowest ? RequestReason::MeshEmergency : RequestReason::MeshDegrade,
                        meshSink,meshContext);
                break;
            }
            default: s.stage = PlanStage::Done; break;
            }
        }
        return Done();
    }

    bool StarfieldPlanSession::StepFor(RequestSink sink, void* context, double milliseconds,
        RequestSink meshSink, void* meshContext)
    {
        // Clamp malformed API input rather than allowing an unbounded frame.
        if (!std::isfinite(milliseconds)) milliseconds = 10.0;
        milliseconds = std::clamp(milliseconds,0.0,100.0);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::duration<double,std::milli>(milliseconds);
        do {
            if (Step(sink,context,100,meshSink,meshContext)) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    void StarfieldPlanSession::Reset()
    {
        if (!impl_) { impl_ = std::make_unique<Impl>(); return; }
        // A cycle every few frames: keep the snapshot buffers' capacity.
        auto entries = std::move(impl_->entries);
        auto meshes = std::move(impl_->meshes);
        entries.clear();
        meshes.clear();
        *impl_ = Impl{};
        impl_->entries = std::move(entries);
        impl_->meshes = std::move(meshes);
    }
    bool StarfieldPlanSession::Done() const { return impl_->stage == PlanStage::Done || impl_->stage == PlanStage::Idle; }
    PlanStage StarfieldPlanSession::Stage() const { return impl_->stage; }
    const PlanResult& StarfieldPlanSession::Result() const { return impl_->result; }
    std::span<const PlanEntry> StarfieldPlanSession::Entries() const { return impl_->entries; }
    std::span<const PlanEntry> StarfieldPlanSession::MeshEntries() const { return impl_->meshes; }
}
