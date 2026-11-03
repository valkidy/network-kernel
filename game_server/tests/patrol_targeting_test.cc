// Who an around_players patrol aims at: grouping, the threat check, and the
// per-player record a floor interval is measured from.
// docs/PLAYER_TARGETED_PATROL_PLAN.md §4.1-§4.3.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_server/src/patrol_targeting.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

using network_example::game_server::ActorStateView;
using network_example::game_server::group_players;
using network_example::game_server::PlayerGroup;
using network_example::game_server::PlayerThreatTracker;
using network_example::game_server::squads_near_group;
using network_example::game_server::target_players_from;
using network_example::game_server::TargetPlayer;

constexpr float kGroupRadius = 18.0f;
constexpr float kThreatRadius = 60.0f;

TargetPlayer player(std::uint32_t net_id, float x, float z) {
    return TargetPlayer{net_id, KernelVec3{x, 0.0f, z}};
}

KernelServerEntityState actor(
    std::uint32_t net_id, std::uint16_t actor_type, std::uint16_t hp) {
    KernelServerEntityState state{};
    state.net_id = net_id;
    state.actor_type = actor_type;
    state.hp = hp;
    state.max_hp = 100;
    state.valid = 1u;
    return state;
}

// The 3 + 1 of the plan's §4.5: a trio at the origin and a loner 100 m off.
std::vector<TargetPlayer> trio_and_loner() {
    return {
        player(7, 100.0f, 0.0f),
        player(2, 0.0f, 0.0f),
        player(5, 10.0f, 0.0f),
        player(3, 0.0f, 10.0f),
    };
}

void only_living_players_are_targets() {
    std::vector<KernelServerEntityState> states;
    states.push_back(actor(1, KernelActorType_Player, 100));
    states.push_back(actor(2, KernelActorType_Player, 0));
    KernelServerEntityState dead_flagged = actor(3, KernelActorType_Player, 100);
    dead_flagged.visual_flags = KERNEL_VISUAL_FLAG_DEAD;
    states.push_back(dead_flagged);
    KernelServerEntityState invalid = actor(4, KernelActorType_Player, 100);
    invalid.valid = 0u;
    states.push_back(invalid);
    states.push_back(actor(5, KernelActorType_Agent, 100));
    states[0].position = KernelVec3{3.0f, 1.0f, 4.0f};

    const std::vector<TargetPlayer> players = target_players_from(
        ActorStateView{states.data(), static_cast<std::uint32_t>(states.size())});
    require(players.size() == 1u);
    require(players[0].net_id == 1u);
    require(players[0].position.x == 3.0f);
    require(players[0].position.z == 4.0f);
}

void a_trio_and_a_loner_are_two_groups_trio_first() {
    const std::vector<PlayerGroup> groups = group_players(trio_and_loner(), kGroupRadius);
    require(groups.size() == 2u);
    require(groups[0].member_net_ids == (std::vector<std::uint32_t>{2, 3, 5}));
    require(groups[1].member_net_ids == (std::vector<std::uint32_t>{7}));
    require(groups[0].member_positions.size() == 3u);
    // Mean of (0,0), (0,10), (10,0).
    require(groups[0].centroid.x > 3.33f && groups[0].centroid.x < 3.34f);
    require(groups[0].centroid.z > 3.33f && groups[0].centroid.z < 3.34f);
    require(groups[1].centroid.x == 100.0f);

    // The order the view lists players in changes nothing.
    std::vector<TargetPlayer> reversed = trio_and_loner();
    std::reverse(reversed.begin(), reversed.end());
    const std::vector<PlayerGroup> again = group_players(reversed, kGroupRadius);
    require(again.size() == 2u);
    require(again[0].member_net_ids == groups[0].member_net_ids);
    require(again[1].member_net_ids == groups[1].member_net_ids);
}

// A-B and B-C are each inside the radius, A-C is not: one group by chaining.
// The control is the same three under a radius none of the pairs meet.
void groups_chain_through_their_members() {
    const std::vector<TargetPlayer> line = {
        player(1, 0.0f, 0.0f), player(2, 15.0f, 0.0f), player(3, 30.0f, 0.0f)};
    require(group_players(line, kGroupRadius).size() == 1u);
    require(group_players(line, 10.0f).size() == 3u);
}

void equal_groups_keep_net_id_order() {
    const std::vector<PlayerGroup> groups = group_players(
        {player(9, 0.0f, 0.0f), player(4, 50.0f, 0.0f), player(6, 100.0f, 0.0f)},
        kGroupRadius);
    require(groups.size() == 3u);
    require(groups[0].member_net_ids[0] == 4u);
    require(groups[1].member_net_ids[0] == 6u);
    require(groups[2].member_net_ids[0] == 9u);
}

void no_players_is_no_groups() {
    require(group_players({}, kGroupRadius).empty());
}

void squads_are_counted_once_per_group_by_distance() {
    const std::vector<PlayerGroup> groups = group_players(trio_and_loner(), kGroupRadius);
    const PlayerGroup& trio = groups[0];
    const PlayerGroup& loner = groups[1];
    // One squad between two trio members counts once; one just inside the
    // radius of the loner counts for the loner only; one just outside counts
    // for nobody.
    const std::vector<KernelVec3> cursors = {
        KernelVec3{5.0f, 0.0f, 5.0f},
        KernelVec3{100.0f + kThreatRadius - 0.5f, 0.0f, 0.0f},
        KernelVec3{100.0f, 0.0f, -(kThreatRadius + 0.5f)},
    };
    require(squads_near_group(trio, cursors, kThreatRadius) == 1u);
    require(squads_near_group(loner, cursors, kThreatRadius) == 1u);
    require(squads_near_group(loner, {cursors[2]}, kThreatRadius) == 0u);
    require(squads_near_group(trio, {}, kThreatRadius) == 0u);
}

void starvation_counts_from_the_last_threat() {
    PlayerThreatTracker tracker;
    const std::vector<TargetPlayer> players = {player(1, 0.0f, 0.0f)};
    // First seen counts as threatened, so a new player waits like everyone.
    tracker.observe(10, players, {}, kThreatRadius);
    require(tracker.last_threatened_tick(1) == 10u);
    const std::vector<PlayerGroup> groups = group_players(players, kGroupRadius);
    require(tracker.group_starved_ticks(groups[0], 10) == 0u);

    // Nothing near: the record stays put and starvation grows.
    tracker.observe(500, players, {KernelVec3{200.0f, 0.0f, 0.0f}}, kThreatRadius);
    require(tracker.last_threatened_tick(1) == 10u);
    require(tracker.group_starved_ticks(groups[0], 500) == 490u);

    // A squad near: reset.
    tracker.observe(600, players, {KernelVec3{20.0f, 0.0f, 0.0f}}, kThreatRadius);
    require(tracker.last_threatened_tick(1) == 600u);
    require(tracker.group_starved_ticks(groups[0], 700) == 100u);
}

// The trio is threatened together, then splits. Each piece is judged by its
// own members' history, so none reads as a fresh target; the loner who was
// never near a squad is the control that starvation is being measured at all.
void a_split_group_is_not_a_fresh_target() {
    PlayerThreatTracker tracker;
    tracker.observe(0, trio_and_loner(), {}, kThreatRadius);
    tracker.observe(
        100, trio_and_loner(), {KernelVec3{0.0f, 0.0f, 0.0f}}, kThreatRadius);

    // At 150 the trio has walked apart; the loner has not moved.
    const std::vector<TargetPlayer> split = {
        player(2, -200.0f, 0.0f),
        player(3, 0.0f, 200.0f),
        player(5, 10.0f, 0.0f),
        player(7, 100.0f, 0.0f),
    };
    tracker.observe(150, split, {}, kThreatRadius);
    const std::vector<PlayerGroup> groups = group_players(split, kGroupRadius);
    require(groups.size() == 4u);
    for (const PlayerGroup& group : groups) {
        const std::uint64_t starved = tracker.group_starved_ticks(group, 150);
        if (group.member_net_ids[0] == 7u) {
            require(starved == 150u);
        } else {
            require(starved == 50u);
        }
    }
}

// A group's starvation is its most recently threatened member's.
void a_group_is_as_fed_as_its_best_fed_member() {
    PlayerThreatTracker tracker;
    const std::vector<TargetPlayer> apart = {player(1, 0.0f, 0.0f), player(2, 300.0f, 0.0f)};
    tracker.observe(0, apart, {}, kThreatRadius);
    tracker.observe(200, apart, {KernelVec3{300.0f, 0.0f, 0.0f}}, kThreatRadius);
    require(tracker.last_threatened_tick(1) == 0u);
    require(tracker.last_threatened_tick(2) == 200u);

    // Now together.
    const std::vector<TargetPlayer> together = {player(1, 0.0f, 0.0f), player(2, 5.0f, 0.0f)};
    tracker.observe(250, together, {}, kThreatRadius);
    const std::vector<PlayerGroup> groups = group_players(together, kGroupRadius);
    require(groups.size() == 1u);
    require(tracker.group_starved_ticks(groups[0], 250) == 50u);
}

void a_player_who_leaves_is_forgotten() {
    PlayerThreatTracker tracker;
    tracker.observe(0, {player(1, 0.0f, 0.0f), player(2, 50.0f, 0.0f)}, {}, kThreatRadius);
    require(tracker.tracked_player_count() == 2u);
    tracker.observe(10, {player(1, 0.0f, 0.0f)}, {}, kThreatRadius);
    require(tracker.tracked_player_count() == 1u);
    require(tracker.last_threatened_tick(2) == 0u);
    // Back again: starts over rather than carrying a stale record.
    tracker.observe(900, {player(1, 0.0f, 0.0f), player(2, 50.0f, 0.0f)}, {}, kThreatRadius);
    require(tracker.last_threatened_tick(2) == 900u);
    require(tracker.last_threatened_tick(1) == 0u);
}

}  // namespace

int main() {
    only_living_players_are_targets();
    a_trio_and_a_loner_are_two_groups_trio_first();
    groups_chain_through_their_members();
    equal_groups_keep_net_id_order();
    no_players_is_no_groups();
    squads_are_counted_once_per_group_by_distance();
    starvation_counts_from_the_last_threat();
    a_split_group_is_not_a_fresh_target();
    a_group_is_as_fed_as_its_best_fed_member();
    a_player_who_leaves_is_forgotten();
    std::printf("patrol_targeting_test passed\n");
    return 0;
}
