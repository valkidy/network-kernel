// Which group an around_players patrol serves next, and with what squad.
// docs/PLAYER_TARGETED_PATROL_PLAN.md §4.4-§4.5 and §5.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "game_server/src/patrol_allocation.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

using network_example::game_server::choose_patrol_target;
using network_example::game_server::definition_squad_cap;
using network_example::game_server::group_squad_cap;
using network_example::game_server::PatrolAllocationState;
using network_example::game_server::PatrolAroundPlayersConfig;
using network_example::game_server::PatrolDefinitionConfig;
using network_example::game_server::PatrolPlacementMode;
using network_example::game_server::PatrolSquadTier;
using network_example::game_server::PatrolTargetCandidate;
using network_example::game_server::PatrolTargetChoice;
using network_example::game_server::PatrolTargetReason;
using network_example::game_server::SpawnCompositionEntry;
using network_example::game_server::squad_shape_for;

constexpr std::uint32_t kPlenty = 1000;

PatrolSquadTier tier(
    std::uint32_t members_min, std::uint32_t members_max,
    std::uint32_t count_min, std::uint32_t count_max, std::uint32_t template_id) {
    PatrolSquadTier result;
    result.members_min = members_min;
    result.members_max = members_max;
    result.count_min = count_min;
    result.count_max = count_max;
    result.composition = {SpawnCompositionEntry{"unit", template_id, count_min, count_max}};
    return result;
}

// The plan's numbers: tiers 4-6 / 6-9 / 8-12, k = 1, 1.5 per player, a 1800
// tick floor.
PatrolDefinitionConfig targeted() {
    PatrolDefinitionConfig definition;
    definition.id = 3;
    definition.name = "targeted";
    definition.placement = PatrolPlacementMode::kAroundPlayers;
    definition.squad_tiers = {
        tier(1, 1, 4, 6, 101),
        tier(2, 2, 6, 9, 102),
        tier(3, 0, 8, 12, 103),
    };
    definition.count_min = 4;
    definition.count_max = 12;
    return definition;
}

PatrolAllocationState ready(std::uint32_t live_groups, std::uint32_t players) {
    PatrolAllocationState state;
    state.live_groups = live_groups;
    state.player_count = players;
    state.interval_elapsed = true;
    state.agent_room = kPlenty;
    return state;
}

void caps_round_up() {
    PatrolAroundPlayersConfig around;
    require(group_squad_cap(around, 1) == 1u);
    require(group_squad_cap(around, 3) == 3u);
    require(definition_squad_cap(around, 4) == 6u);
    require(definition_squad_cap(around, 3) == 5u);
    require(definition_squad_cap(around, 0) == 0u);
    around.groups_per_member = 0.5f;
    require(group_squad_cap(around, 1) == 1u);
    require(group_squad_cap(around, 3) == 2u);
    // 10 * 0.3 is a hair over 3 in float; it means 3.
    around.groups_per_player = 0.3f;
    require(definition_squad_cap(around, 10) == 3u);
}

void the_squad_follows_the_group_size() {
    const PatrolDefinitionConfig definition = targeted();
    require(squad_shape_for(definition, 1).count_max == 6u);
    require(squad_shape_for(definition, 1).composition->at(0).entity_template_id == 101u);
    require(squad_shape_for(definition, 2).count_min == 6u);
    require(squad_shape_for(definition, 2).count_max == 9u);
    require(squad_shape_for(definition, 3).count_max == 12u);
    // The last tier is unbounded.
    require(squad_shape_for(definition, 7).composition->at(0).entity_template_id == 103u);

    // Untiered: the definition's own band, whatever the group.
    PatrolDefinitionConfig untiered = targeted();
    untiered.squad_tiers.clear();
    untiered.count_min = 6;
    untiered.count_max = 12;
    untiered.composition = {SpawnCompositionEntry{"unit", 200, 6, 12}};
    require(squad_shape_for(untiered, 1).count_min == 6u);
    require(squad_shape_for(untiered, 1).composition == &untiered.composition);
}

// §4.5: a trio and a loner under a 6-squad ceiling.
void a_trio_and_a_loner_both_get_served() {
    const PatrolDefinitionConfig definition = targeted();

    // Nobody threatened: the trio first, and it gets the 8-12 squad.
    std::vector<PatrolTargetCandidate> candidates = {{3, 0, 0}, {1, 0, 0}};
    std::optional<PatrolTargetChoice> choice =
        choose_patrol_target(definition, candidates, ready(0, 4));
    require(choice.has_value());
    require(choice->candidate_index == 0u);
    require(choice->reason == PatrolTargetReason::kUnthreatened);
    require(choice->shape.count_max == 12u);

    // The trio holds its whole share; the loner is next, with its own 4-6.
    candidates = {{3, 3, 0}, {1, 0, 0}};
    choice = choose_patrol_target(definition, candidates, ready(3, 4));
    require(choice.has_value());
    require(choice->candidate_index == 1u);
    require(choice->reason == PatrolTargetReason::kUnthreatened);
    require(choice->shape.count_max == 6u);

    // Both at their caps, with room left under the definition's 6: nothing.
    candidates = {{3, 3, 0}, {1, 1, 0}};
    require(!choose_patrol_target(definition, candidates, ready(4, 4)).has_value());

    // The control: one under its cap is topped up.
    candidates = {{3, 2, 0}, {1, 1, 0}};
    choice = choose_patrol_target(definition, candidates, ready(3, 4));
    require(choice.has_value());
    require(choice->candidate_index == 0u);
    require(choice->reason == PatrolTargetReason::kUnderCap);
}

// A loner nobody has threatened for the floor interval gets a squad even with
// the definition's ceiling full and the interval still running.
void the_floor_gets_past_a_full_ceiling() {
    const PatrolDefinitionConfig definition = targeted();
    PatrolAllocationState full = ready(6, 4);
    full.interval_elapsed = false;

    std::vector<PatrolTargetCandidate> candidates = {{3, 3, 0}, {1, 0, 1800}};
    std::optional<PatrolTargetChoice> choice =
        choose_patrol_target(definition, candidates, full);
    require(choice.has_value());
    require(choice->candidate_index == 1u);
    require(choice->reason == PatrolTargetReason::kFloor);

    // The control: a tick short of the floor is nothing.
    candidates[1].starved_ticks = 1799;
    require(!choose_patrol_target(definition, candidates, full).has_value());

    // Zero is no floor at all.
    PatrolDefinitionConfig no_floor = targeted();
    no_floor.around_players.floor_interval_ticks = 0;
    candidates[1].starved_ticks = 1000000;
    require(!choose_patrol_target(no_floor, candidates, full).has_value());
}

// The floor ignores the ceiling but not the agent budgets.
void the_floor_still_needs_agent_room() {
    const PatrolDefinitionConfig definition = targeted();
    PatrolAllocationState state = ready(6, 4);
    state.agent_room = 5;
    const std::vector<PatrolTargetCandidate> candidates = {{1, 0, 5000}};
    require(!choose_patrol_target(definition, candidates, state).has_value());
    state.agent_room = 6;
    require(choose_patrol_target(definition, candidates, state).has_value());
}

void the_longest_starved_floor_goes_first() {
    const PatrolDefinitionConfig definition = targeted();
    // Larger group first in the list, but less starved: starvation wins.
    const std::vector<PatrolTargetCandidate> candidates = {
        {2, 0, 2000}, {1, 0, 3000}, {1, 0, 2500}};
    const std::optional<PatrolTargetChoice> choice =
        choose_patrol_target(definition, candidates, ready(0, 4));
    require(choice.has_value());
    require(choice->candidate_index == 1u);
    require(choice->reason == PatrolTargetReason::kFloor);
}

// A trio's 12 does not fit, a loner's 6 does: the loner is served rather than
// nobody. The control is the same with room for the trio.
void a_tight_budget_serves_a_smaller_squad() {
    const PatrolDefinitionConfig definition = targeted();
    const std::vector<PatrolTargetCandidate> candidates = {{3, 0, 0}, {1, 0, 0}};
    PatrolAllocationState state = ready(0, 4);

    state.agent_room = 12;
    require(choose_patrol_target(definition, candidates, state)->candidate_index == 0u);
    state.agent_room = 7;
    require(choose_patrol_target(definition, candidates, state)->candidate_index == 1u);
    state.agent_room = 5;
    require(!choose_patrol_target(definition, candidates, state).has_value());
}

void equal_sizes_go_to_the_longer_starved() {
    const PatrolDefinitionConfig definition = targeted();
    const std::vector<PatrolTargetCandidate> candidates = {{2, 0, 100}, {2, 0, 900}};
    require(choose_patrol_target(definition, candidates, ready(0, 4))->candidate_index == 1u);
}

void only_the_floor_runs_between_intervals() {
    const PatrolDefinitionConfig definition = targeted();
    const std::vector<PatrolTargetCandidate> candidates = {{3, 0, 0}};
    PatrolAllocationState state = ready(0, 3);
    state.interval_elapsed = false;
    require(!choose_patrol_target(definition, candidates, state).has_value());
    state.interval_elapsed = true;
    require(choose_patrol_target(definition, candidates, state).has_value());
}

void nobody_is_nothing() {
    require(!choose_patrol_target(targeted(), {}, ready(0, 0)).has_value());
}

}  // namespace

int main() {
    caps_round_up();
    the_squad_follows_the_group_size();
    a_trio_and_a_loner_both_get_served();
    the_floor_gets_past_a_full_ceiling();
    the_floor_still_needs_agent_room();
    the_longest_starved_floor_goes_first();
    a_tight_budget_serves_a_smaller_squad();
    equal_sizes_go_to_the_longer_starved();
    only_the_floor_runs_between_intervals();
    nobody_is_nothing();
    std::printf("patrol_allocation_test passed\n");
    return 0;
}
