#include "Planner.h"
#include "StarfieldPlanner.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace dtl
{
    namespace
    {
        constexpr std::uint64_t kNoLimit = std::numeric_limits<std::uint64_t>::max();

        std::uint64_t Minus(std::uint64_t a_left, std::uint64_t a_right) { return a_left > a_right ? a_left - a_right : 0; }

        // Bytes freed going from one level down to another, or spent going up.
        std::uint64_t Between(const PlanEntry& a_entry, std::uint8_t a_from, std::uint8_t a_to)
        {
            const auto from = a_entry.bytes[a_from];
            const auto to = a_entry.bytes[a_to];
            return from > to ? from - to : to - from;
        }

        std::uint8_t Clamp(std::uint8_t a_level) { return std::min<std::uint8_t>(a_level, kMaxPlanLevel); }

        // Unseen textures rank below every seen one.
        float Rank(const PlanEntry& a_entry) { return a_entry.seen ? a_entry.priority : -1.0f; }

        // Where a texture should be before any budget is applied.
        std::uint8_t Base(const PlanEntry& a_entry, std::uint8_t a_hold, Mode a_mode, Unseen a_unseen)
        {
            if (a_entry.seen) {
                return std::min(a_entry.wanted, a_entry.lowest);
            }
            switch (a_unseen) {
            case Unseen::Lowest:
                return a_entry.lowest;
            case Unseen::UnderPressure:
                return a_mode == Mode::Tight || a_mode == Mode::Over ? std::max(a_hold, a_entry.lowest) : a_hold;
            // ByDistance needs a rank for unseen textures, which only the
            // Starfield policy's backends supply; here it holds like Keep.
            default:
                return a_hold;
            }
        }
    }

    const char* ModeName(Mode a_mode)
    {
        switch (a_mode) {
        case Mode::Room:
            return "room";
        case Mode::Tight:
            return "tight";
        case Mode::Over:
            return "over budget";
        default:
            return "unlimited";
        }
    }

    Budget Assess(const BudgetLines& a_lines, std::uint64_t a_usage, std::uint64_t a_osBudget)
    {
        Budget budget;
        budget.usage = a_usage;
        budget.osBudget = a_osBudget;
        if (a_osBudget == 0) {
            return budget;
        }

        budget.line = Minus(a_osBudget, a_lines.reserve);
        budget.target = Minus(budget.line, a_lines.target);
        const auto span = static_cast<double>(budget.line - budget.target);
        budget.ceiling = budget.target + static_cast<std::uint64_t>(span * std::clamp(a_lines.upgradeCeiling, 0.0f, 1.0f));

        // As CheckBudget does it, with no hysteresis: the ceiling between the
        // target and the line is what keeps upgrades from running into it.
        budget.mode = a_usage > budget.line ? Mode::Over : a_usage >= budget.target ? Mode::Tight : Mode::Room;
        return budget;
    }

    PlanResult Plan(std::span<PlanEntry> a_entries, const Budget& a_budget, const BudgetLines& a_lines,
        const PlanSettings& a_settings)
    {
        if (a_settings.policy == DetailPolicy::Starfield) {
            StarfieldPlanSession session;
            StarfieldCycleInput input;
            input.unseen = a_settings.unseen;
            session.Begin(a_entries, a_budget, a_lines, input);
            while (!session.Step([](void*, const PlanRequest& request) {
                return RequestReceipt{ RequestStatus::AcceptedPending, request.estimatedBytes };
            }, nullptr)) {}
            std::ranges::copy(session.Entries(), a_entries.begin());
            return session.Result();
        }
        PlanResult result;
        result.mode = a_budget.mode;

        // One list, most important first, as SortLODs leaves it.
        std::ranges::sort(a_entries, [](const PlanEntry& a_left, const PlanEntry& a_right) {
            const float left = Rank(a_left);
            const float right = Rank(a_right);
            return left != right ? left > right : a_left.key < a_right.key;
        });

        // What earlier plans still have to do. Lowerings already asked for
        // count against an overage, raisings against the room under the ceiling.
        for (auto& entry : a_entries) {
            entry.current = Clamp(entry.current);
            entry.lowest = Clamp(entry.lowest);
            entry.previous = std::min(Clamp(entry.previous), std::max(entry.current, entry.lowest));
            if (entry.previous > entry.current) {
                result.pendingFree += Between(entry, entry.current, entry.previous);
            } else if (entry.previous < entry.current) {
                result.pendingGrow += Between(entry, entry.previous, entry.current);
            }
        }

        // Down to where size on screen puts each texture. What earlier cycles
        // asked for stands, since it is already counted above, except a
        // raising over the line, which is called off.
        std::vector<std::uint8_t> base(a_entries.size());
        for (std::size_t i = 0; i < a_entries.size(); ++i) {
            auto&      entry = a_entries[i];
            const bool cancel = entry.previous < entry.current && a_budget.mode == Mode::Over;
            const auto hold = cancel ? entry.current : entry.previous;

            base[i] = Base(entry, hold, a_budget.mode, a_settings.unseen);
            if (base[i] > hold) {
                // Only what goes below the current level is freed memory.
                const auto from = std::max(hold, entry.current);
                if (base[i] > from) {
                    result.lowered += Between(entry, from, base[i]);
                }
                entry.planned = base[i];
            } else {
                entry.planned = hold;
            }
        }

        // UpgradeTextures: from the front, while the slice and the room under
        // the ceiling last. A single upgrade may overshoot what is left, as in
        // Starfield; the next cycle then has less.
        std::uint64_t allowance = 0;
        switch (a_budget.mode) {
        case Mode::Unlimited:
            allowance = kNoLimit;
            break;
        case Mode::Room:
        case Mode::Tight:
            allowance = std::min(a_lines.upgradePerCycle,
                Minus(a_budget.ceiling, a_budget.usage + result.pendingGrow));
            break;
        default:
            break;
        }
        const bool swapMode = allowance == 0 && (a_budget.mode == Mode::Room || a_budget.mode == Mode::Tight);

        std::size_t candidate = a_entries.size();
        for (std::size_t i = 0; i < a_entries.size(); ++i) {
            auto& entry = a_entries[i];
            if (base[i] >= entry.planned) {
                continue;
            }
            if (allowance > 0) {
                const auto cost = Between(entry, base[i], entry.planned);
                entry.planned = base[i];
                result.raised += cost;
                allowance = allowance == kNoLimit ? kNoLimit : Minus(allowance, cost);
            } else {
                ++result.upgradesHeld;
                if (candidate == a_entries.size()) {
                    candidate = i;
                }
            }
        }

        // Swap mode: from the back, one level past where size on screen puts
        // each texture, until the most important upgrade is paid for. If the
        // list can't pay for it, nothing changes.
        if (swapMode && candidate < a_entries.size()) {
            auto&      wanted = a_entries[candidate];
            const auto cost = Between(wanted, base[candidate], wanted.planned);

            std::vector<std::pair<std::size_t, std::uint8_t>> trades;
            std::uint64_t                                     freed = 0;
            for (std::size_t i = a_entries.size(); i-- > candidate + 1 && freed < cost;) {
                auto&      entry = a_entries[i];
                const auto next = std::min<std::uint8_t>(std::max<std::uint8_t>(entry.planned, base[i] + 1), entry.lowest);
                if (next <= entry.planned) {
                    continue;
                }
                freed += Between(entry, entry.planned, next);
                trades.emplace_back(i, next);
            }

            if (freed >= cost) {
                for (const auto& [index, level] : trades) {
                    a_entries[index].planned = level;
                }
                wanted.planned = base[candidate];
                --result.upgradesHeld;
                result.raised += cost;
                result.traded = freed;
                result.swapped = true;
            }
        }

        // Over the line: the emergency walk, from the back, everything to its
        // lowest level until the overage is freed, net of what is already on
        // its way out.
        if (a_budget.mode == Mode::Over) {
            const auto need = Minus(Minus(a_budget.usage, a_budget.line), result.pendingFree + result.lowered);
            for (std::size_t i = a_entries.size(); i-- > 0 && result.emergency < need;) {
                auto& entry = a_entries[i];
                if (entry.planned >= entry.lowest) {
                    continue;
                }
                result.emergency += Between(entry, entry.planned, entry.lowest);
                entry.planned = entry.lowest;
            }
        }

        return result;
    }
}
