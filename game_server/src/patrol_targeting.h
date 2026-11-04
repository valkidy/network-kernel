#ifndef GAME_SERVER_PATROL_TARGETING_H_
#define GAME_SERVER_PATROL_TARGETING_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "game_server/src/agent_runtime.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// Who an around_players patrol may be aimed at, and who is already being
// threatened. docs/PLAYER_TARGETED_PATROL_PLAN.md §4.1-§4.3.
//
// Pure: nothing here reaches the kernel, so the director feeds it what it has
// already read and a test can feed it whatever it likes.

struct TargetPlayer {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
};

// Players close enough together to be one target. Members are in net_id order;
// the centroid is their mean position, not yet snapped to anything.
struct PlayerGroup {
    std::vector<std::uint32_t> member_net_ids;
    std::vector<KernelVec3> member_positions;
    KernelVec3 centroid{0.0f, 0.0f, 0.0f};
};

// Living players, in the order the view holds them. A dead player is no
// target: a squad walking up to a corpse is a squad wasted, and the respawn
// puts the player somewhere else anyway.
std::vector<TargetPlayer> target_players_from(const ActorStateView& actors);

// Single-linkage groups: two players within `radius` of each other (horizontal)
// share a group, and so does anyone linked through a chain of such pairs. Larger
// groups first, ties broken by the lowest member net_id, so the same players
// always come out in the same order whatever order the view lists them in.
//
// Pairwise on purpose: a server holds a handful of players, and an index would
// cost more to build than the comparisons it saves.
std::vector<PlayerGroup> group_players(
    const std::vector<TargetPlayer>& players,
    float radius);

// Squads whose cursor is within `radius` (horizontal) of any member. Measured
// now, not by who a squad was spawned for: a loner who walks up to someone
// else's squad is threatened, and a group that walked away from its own is
// not.
std::uint32_t squads_near_group(
    const PlayerGroup& group,
    const std::vector<KernelVec3>& squad_cursors,
    float radius);

// When each player last had a squad near them, kept per player because groups
// are recomputed every time and have no identity to hang a cooldown on.
class PlayerThreatTracker {
public:
    // Players near any squad are threatened as of `tick`. A player seen for the
    // first time starts as threatened on this tick, so a player who has just
    // joined waits out a floor interval like everyone else rather than jumping
    // the queue. Players no longer listed are forgotten; one who comes back
    // starts again.
    void observe(
        std::uint64_t tick,
        const std::vector<TargetPlayer>& players,
        const std::vector<KernelVec3>& squad_cursors,
        float radius);

    // Ticks since the group was last threatened, judged by whichever member was
    // threatened most recently. That is what keeps a trio that splits up from
    // reading as three fresh, starving targets the moment it splits.
    std::uint64_t group_starved_ticks(
        const PlayerGroup& group,
        std::uint64_t tick) const;

    // Zero for a player never observed.
    std::uint64_t last_threatened_tick(std::uint32_t net_id) const;

    std::size_t tracked_player_count() const { return last_threatened_.size(); }

private:
    std::unordered_map<std::uint32_t, std::uint64_t> last_threatened_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_TARGETING_H_
