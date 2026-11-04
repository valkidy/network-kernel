#ifndef GAME_SERVER_PATROL_ALLOCATION_H_
#define GAME_SERVER_PATROL_ALLOCATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "game_server/src/patrol_director.h"
#include "game_server/src/spawn_sampling.h"

namespace network_example::game_server {

// Which group an around_players definition aims its next squad at, and how big
// that squad is. docs/PLAYER_TARGETED_PATROL_PLAN.md §4.4 and §5.
//
// Pure, like patrol_targeting: the director hands it numbers it has already
// worked out, one candidate per player group.

// The squad a group of this size gets: its tier's band and mix, or the
// definition's own when it authors no tiers.
struct PatrolSquadShape {
    std::uint32_t count_min = 0;
    std::uint32_t count_max = 0;
    const std::vector<SpawnCompositionEntry>* composition = nullptr;
};

PatrolSquadShape squad_shape_for(
    const PatrolDefinitionConfig& definition,
    std::uint32_t members);

// ceil(members * groups_per_member): how many live squads one group may have
// near it.
std::uint32_t group_squad_cap(
    const PatrolAroundPlayersConfig& around,
    std::uint32_t members);

// ceil(players * groups_per_player): how many live squads the definition may
// have at all. Zero players is zero squads.
std::uint32_t definition_squad_cap(
    const PatrolAroundPlayersConfig& around,
    std::uint32_t players);

// One player group, as the allocator needs to see it.
struct PatrolTargetCandidate {
    std::uint32_t members = 0;
    // Squads near the group now (squads_near_group).
    std::uint32_t squads_near = 0;
    // PlayerThreatTracker::group_starved_ticks.
    std::uint64_t starved_ticks = 0;
};

struct PatrolAllocationState {
    // This definition's live squads.
    std::uint32_t live_groups = 0;
    std::uint32_t player_count = 0;
    // Whether the definition's interval has run down. Only the floor ignores
    // it.
    bool interval_elapsed = false;
    // Agents that may still be spawned under every ceiling that applies:
    // patrol_budget and agent_budget, whichever is tighter.
    std::uint32_t agent_room = 0;
};

enum class PatrolTargetReason : std::uint8_t {
    // Starved past floor_interval_ticks.
    kFloor = 0,
    // Nothing near it.
    kUnthreatened = 1,
    // Something near it, but fewer squads than its cap.
    kUnderCap = 2,
};

struct PatrolTargetChoice {
    // Into the candidates handed in.
    std::size_t candidate_index = 0;
    PatrolTargetReason reason = PatrolTargetReason::kUnthreatened;
    PatrolSquadShape shape;
};

// At most one squad per call, in this order:
//
//   1. the floor: groups starved for floor_interval_ticks or longer, longest
//      first. It runs every tick, not on the interval, and it is not held to
//      the definition-wide squad cap -- that cap filling up around a larger
//      group is exactly what the floor exists to get past.
//   2. groups with nothing near them, larger first, then longer starved.
//   3. groups under their own cap, larger first.
//
// Ties fall back to candidate order, which group_players makes deterministic.
// Every class is held to agent_room against its own tier's count_max: a group
// whose squad does not fit is passed over for the next one whose does, so a
// tight budget still puts out a small squad rather than nothing.
//
// Nullopt when nobody qualifies.
std::optional<PatrolTargetChoice> choose_patrol_target(
    const PatrolDefinitionConfig& definition,
    const std::vector<PatrolTargetCandidate>& candidates,
    const PatrolAllocationState& state);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_ALLOCATION_H_
