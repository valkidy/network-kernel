#ifndef SIMULATION_PUBLIC_LOCK_STRIKE_H_
#define SIMULATION_PUBLIC_LOCK_STRIKE_H_

#include <cstdint>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/lock_target_selection.h"
#include "simulation/public/simulation.h"

namespace network_example {

// One lock, whoever asks for it: a lock-on weapon's commit, or an action
// graph's lock_strike. Everything the authority needs is here -- the volume
// (where it starts, where it aims, how far, cone or sphere, how many), the
// shot whose damage and side it deals -- so nothing reads a weapon definition.
// docs/LOCK_STRIKE_ACTION_PLAN.md.
struct LockStrikeSpec {
    lock_targets::Cone volume;
    std::uint32_t projectile_template_id = 0;
    std::uint32_t collision_mask = 0;
    // The weapon the damage is credited to; zero for a graph's strike that no
    // weapon fired.
    std::uint8_t weapon_id = 0;
    std::uint16_t damage = 0;
};

// What a lock locked, nearest first, with each target's hit zone beside it.
struct LockSelection {
    std::vector<lock_targets::Candidate> locked;
    std::vector<std::pair<NetId, std::uint16_t>> hit_zones;

    std::uint16_t hit_zone_of(NetId net_id) const;
};

// The shared rule (lock_targets::select) over who may be locked: living
// actors on a side the shot hits, other than `shooter_net_id` and anything
// `shooter_peer_id` controls, a static wall between the volume's origin and a
// target hiding it. From the rewound frame when there is one, the live world
// otherwise. A shooter of zero excludes nobody by id: a bottle has no shooter.
LockSelection select_lock_targets(
    World& world,
    const LockStrikeSpec& spec,
    const HistoryFrame* rewind_frame,
    NetId shooter_net_id,
    PeerId shooter_peer_id);

// Every locked target damaged once, at once. Nothing is spawned.
void apply_lock_instant_damage(
    World& world,
    const LockStrikeSpec& spec,
    const LockSelection& selection,
    std::uint32_t current_tick,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint64_t hit_time_us,
    DamagePipeline* damage_pipeline);

// One missile from the volume's origin at `target`. Its hit is certain and
// decided now -- scheduled in the pipeline for its arrival -- so the missile
// itself is never simulated, only reported (into `launches`) for clients to
// draw. It flies at the speed that covers the volume's full reach in the shot
// template's lifetime_ticks, so a target at the edge is hit at the end of
// that lifetime and a nearer one proportionally sooner, never in fewer than
// one tick. False when nothing was scheduled. `report_origin` records the
// volume's origin as where it leaves from (a graph's lock_strike).
bool launch_lock_missile(
    World& world,
    const LockStrikeSpec& spec,
    const lock_targets::Candidate& target,
    std::uint16_t hit_zone,
    std::uint16_t commit_index,
    std::uint32_t current_tick,
    float fixed_delta_seconds,
    std::uint64_t action_time_us,
    NetId shooter_net_id,
    PeerId shooter_peer_id,
    std::uint32_t action_instance_id,
    DamagePipeline* damage_pipeline,
    std::vector<LockMissileLaunch>* launches,
    bool report_origin = false);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_LOCK_STRIKE_H_
