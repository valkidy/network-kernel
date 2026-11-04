#include "game_server/src/patrol_allocation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>

namespace network_example::game_server {

namespace {

// Rounded up, less a hair, so 3 * 1.0 is 3 and not 4 when the product lands a
// rounding error above the integer it means.
std::uint32_t ceil_product(std::uint32_t count, float per) {
    const float product = static_cast<float>(count) * per;
    if (!(product > 0.0f)) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::ceil(product - 1e-4f));
}

}  // namespace

PatrolSquadShape squad_shape_for(
    const PatrolDefinitionConfig& definition,
    std::uint32_t members) {
    for (const PatrolSquadTier& tier : definition.squad_tiers) {
        if (members >= tier.members_min &&
            (tier.members_max == 0 || members <= tier.members_max)) {
            return PatrolSquadShape{tier.count_min, tier.count_max, &tier.composition};
        }
    }
    // No tiers, or a size the validator would never have let through.
    return PatrolSquadShape{
        definition.count_min, definition.count_max, &definition.composition};
}

std::uint32_t group_squad_cap(
    const PatrolAroundPlayersConfig& around,
    std::uint32_t members) {
    return ceil_product(members, around.groups_per_member);
}

std::uint32_t definition_squad_cap(
    const PatrolAroundPlayersConfig& around,
    std::uint32_t players) {
    return ceil_product(players, around.groups_per_player);
}

std::optional<PatrolTargetChoice> choose_patrol_target(
    const PatrolDefinitionConfig& definition,
    const std::vector<PatrolTargetCandidate>& candidates,
    const PatrolAllocationState& state) {
    const PatrolAroundPlayersConfig& around = definition.around_players;
    const std::uint64_t floor_ticks = around.floor_interval_ticks;

    const auto is_floor = [&](const PatrolTargetCandidate& candidate) {
        return floor_ticks != 0 && candidate.starved_ticks >= floor_ticks;
    };
    const auto under_group_cap = [&](const PatrolTargetCandidate& candidate) {
        return candidate.squads_near < group_squad_cap(around, candidate.members);
    };
    // The first candidate in `order` that fits the budget.
    const auto first_that_fits =
        [&](const std::vector<std::size_t>& order,
            PatrolTargetReason reason) -> std::optional<PatrolTargetChoice> {
        for (const std::size_t index : order) {
            const PatrolSquadShape shape =
                squad_shape_for(definition, candidates[index].members);
            if (shape.count_max <= state.agent_room) {
                return PatrolTargetChoice{index, reason, shape};
            }
        }
        return std::nullopt;
    };
    std::vector<std::size_t> all(candidates.size());
    std::iota(all.begin(), all.end(), 0u);

    std::vector<std::size_t> floor;
    std::copy_if(all.begin(), all.end(), std::back_inserter(floor), [&](std::size_t index) {
        return is_floor(candidates[index]) && under_group_cap(candidates[index]);
    });
    std::stable_sort(floor.begin(), floor.end(), [&](std::size_t lhs, std::size_t rhs) {
        return candidates[lhs].starved_ticks > candidates[rhs].starved_ticks;
    });
    if (auto choice = first_that_fits(floor, PatrolTargetReason::kFloor)) {
        return choice;
    }

    if (!state.interval_elapsed ||
        state.live_groups >= definition_squad_cap(around, state.player_count)) {
        return std::nullopt;
    }

    std::vector<std::size_t> unthreatened;
    std::copy_if(
        all.begin(), all.end(), std::back_inserter(unthreatened), [&](std::size_t index) {
            return candidates[index].squads_near == 0 && under_group_cap(candidates[index]);
        });
    std::stable_sort(
        unthreatened.begin(), unthreatened.end(), [&](std::size_t lhs, std::size_t rhs) {
            if (candidates[lhs].members != candidates[rhs].members) {
                return candidates[lhs].members > candidates[rhs].members;
            }
            return candidates[lhs].starved_ticks > candidates[rhs].starved_ticks;
        });
    if (auto choice = first_that_fits(unthreatened, PatrolTargetReason::kUnthreatened)) {
        return choice;
    }

    std::vector<std::size_t> under_cap;
    std::copy_if(
        all.begin(), all.end(), std::back_inserter(under_cap), [&](std::size_t index) {
            return candidates[index].squads_near > 0 && under_group_cap(candidates[index]);
        });
    std::stable_sort(under_cap.begin(), under_cap.end(), [&](std::size_t lhs, std::size_t rhs) {
        return candidates[lhs].members > candidates[rhs].members;
    });
    return first_that_fits(under_cap, PatrolTargetReason::kUnderCap);
}

}  // namespace network_example::game_server
