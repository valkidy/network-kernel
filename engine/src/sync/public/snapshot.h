#ifndef SYNC_PUBLIC_SNAPSHOT_H_
#define SYNC_PUBLIC_SNAPSHOT_H_

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "world/public/world.h"

namespace network_example {

inline constexpr std::uint32_t kSnapshotStateFlagHpUnknown = 1u << 0;
inline constexpr std::uint32_t kSnapshotStateFlagProjectileHybridCorrection = 1u << 1;
// A beam projectile. Beams replicate a reach instead of a velocity: they do not
// move, so the compact projectile section -- which reconstructs a projectile's
// rotation from its velocity -- can only ever hand a beam the identity
// rotation, leaving the client no idea where it points or how far it reaches
// once something stops it. Their origin and aim are not replicated either; the
// client rebuilds both from the shooter, whose position and aim_direction are
// already in every snapshot's actor section.
inline constexpr std::uint32_t kSnapshotStateFlagProjectileBeam = 1u << 2;

// EntitySnapshot::weapon_state_flags.
inline constexpr std::uint8_t kSnapshotWeaponStateFlagReloading = 1u << 0;

struct SnapshotHeader {
    std::uint32_t server_tick = 0;
    std::uint32_t server_time_ms = 0;
    std::uint32_t last_processed_input_seq = 0;
};

struct EntitySnapshot {
    NetId net_id = 0;
    EntityType type = EntityType::kUnknown;
    ActorType actor_type = ActorType::kUnknown;
    PeerId owner_peer = 0;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
    std::uint16_t state = 0;
    std::uint32_t flags = 0;
    std::uint32_t state_flags = 0;
    std::uint32_t spawn_tick = 0;
    std::uint32_t action_instance_id = 0;
    glm::vec3 aim_direction{1.0f, 0.0f, 0.0f};
    // Beams only. How far the beam actually reached this tick -- its authored
    // length, or the distance to whatever stopped it first. The far end is
    // position + forward * this, where forward is the beam's rotation applied
    // to +Z; only the scalar travels on the wire, because origin and aim are
    // already replicated by the shooter. Meaningless unless
    // kSnapshotStateFlagProjectileBeam is set.
    float beam_effective_length = 0.0f;
    std::uint32_t action_template_id = 0;
    std::uint32_t action_start_tick = 0;
    std::uint32_t action_commit_count = 0;
    std::uint8_t action_phase = 0;
    bool has_authoritative_movement_state = false;
    std::uint16_t ground_state = 0;
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    NetId supporting_entity_net_id = 0;
    std::uint32_t supporting_collider_id = 0;
    // The actor's ImpulseLockout while one stands, for the one client that
    // predicts it. A knockback the client did not cause -- an enemy's swing --
    // is otherwise invisible to its prediction, which rebuilds horizontal
    // velocity from input over the top of the throw and is pulled back to the
    // authority on every snapshot until the actor lands. Filtered to the
    // receiving session's own player exactly like movement state.
    bool has_impulse_lockout = false;
    std::uint32_t impulse_lockout_until_tick = 0;
    std::uint32_t impulse_lockout_armed_tick = 0;
    // The lockout is the actor getting up after a knockback that landed:
    // rooted, not carrying velocity. Meaningless without has_impulse_lockout.
    bool impulse_lockout_recovering = false;
    // The status suspension holding the actor (schema 29), for its owner's
    // prediction alone, as the lockout is: the velocity the authority moves it
    // at and the tick the status ends -- when it drops.
    bool has_suspension = false;
    glm::vec3 suspension_velocity{0.0f};
    std::uint32_t suspension_until_tick = 0;
    // The building this actor is inside, or 0, and its seat there. Sent to
    // every session that sees the actor (schema 27; 26 sent it to the owner
    // alone): the owner's prediction needs it to stop pushing its player out
    // of the building's walls, and every client needs it to draw the
    // occupants -- hidden outside, seated inside.
    NetId shelter_net_id = 0;
    std::uint8_t shelter_seat = 0;
    // The weapon the player is holding, for the one client that holds it. Like
    // movement state, the builder fills it for every armed actor and
    // build_relevant_snapshot keeps it only on the receiving session's own
    // player: nobody else's HUD shows another player's magazine. Only the active
    // slot travels, because that is the only one a HUD reads.
    // Schema 28: the weapon a player holds, for every client that sees the
    // player (KERNEL_HELD_WEAPON_NONE when unarmed). Players only.
    bool has_held_weapon = false;
    std::uint8_t held_weapon_id = kHeldWeaponNone;
    bool has_owner_weapon_state = false;
    std::uint8_t active_weapon_slot = 0;
    std::uint8_t weapon_state_flags = 0;
    std::uint16_t active_weapon_ammo = 0;
    std::uint32_t item_template_id = 0;
    std::uint64_t item_instance_id = 0;
    std::uint8_t world_item_mode = 0;
    NetId carrier_entity_id = 0;
};

// A lock record that carries where it was taken from (snapshot schema 31): an
// action graph's lock_strike, whose source -- a bottle -- is gone, or was never
// a shooter at all. Its shooter may then be zero.
constexpr std::uint8_t kSnapshotLockRecordHasOrigin = 1u << 0;

// One lock-on missile (snapshot schema 30, 31). The server has already
// scheduled its hit; this is only what a client needs to draw a flight from
// the shooter -- or from `origin`, with kSnapshotLockRecordHasOrigin -- that
// reaches the target on its arrival tick. 13 B on the wire, 25 B with an
// origin, in an optional section of the first snapshot packet of an interval
// -- unreliable, like the snapshot: a lost one is a missile not drawn, and the
// damage lands anyway. Each is sent once, so a client tells them apart by
// shooter, launch tick, commit and target.
struct SnapshotLockMissile {
    NetId shooter_net_id = 0;
    NetId target_net_id = 0;
    // The commit it was fired on, counted from zero.
    std::uint16_t commit_index = 0;
    // Ticks before the snapshot's server_tick that it was launched: the
    // snapshot carries every missile since the last one.
    std::uint8_t launch_tick_delta = 0;
    // Ticks from launch to arrival, 1..255.
    std::uint8_t flight_ticks = 1;
    // kSnapshotLockRecord*.
    std::uint8_t flags = 0;
    // With kSnapshotLockRecordHasOrigin: where the missile leaves from.
    glm::vec3 origin{0.0f};
};

// One instant lock_strike of an action graph (snapshot schema 31): what it
// locked -- the targets it struck at once, nearest first -- and enough of the
// lock to draw the rest of its arcs: where it was taken, along which
// direction, in which volume, and how many arcs a full lock has. The unfilled
// arcs' ends a client works out itself (lock_targets::unfilled_arc_end), as for
// a weapon's. 31 B and 4 B a target, in its own optional section, sent once
// like a missile.
struct SnapshotLockStrike {
    glm::vec3 origin{0.0f};
    // Unit length, or zero for a sphere; 16 bits an axis on the wire.
    glm::vec3 direction{0.0f};
    std::uint32_t collider_template_id = 0;
    // The graph's instigator; zero for a bottle.
    NetId instigator_net_id = 0;
    // Which strike of a repeated lock_strike's run, from zero.
    std::uint16_t strike_index = 0;
    std::uint8_t launch_tick_delta = 0;
    // A full lock's arcs, 1..KERNEL_MAX_LOCK_ARCS (16).
    std::uint8_t arc_count = 1;
    // At most arc_count.
    std::vector<NetId> target_net_ids;
};

struct WorldSnapshot {
    SnapshotHeader header;
    std::vector<EntitySnapshot> entities;
    std::vector<SnapshotLockMissile> lock_missiles;
    std::vector<SnapshotLockStrike> lock_strikes;
};

// The visual flags a world entity's own components imply: moving, reloading,
// dead, and grounded / falling / landed from its movement state. Every render
// and snapshot path takes its flags from here, so a flag one path learns about
// reaches all of them. Flags an action writes -- aiming, firing, staggered --
// live on ReplicationState instead and are merged on top by the caller.
std::uint32_t derived_visual_flags(const World& world, entt::entity entity);

WorldSnapshot build_world_snapshot(
    const World& world,
    std::uint32_t server_tick,
    std::uint32_t server_time_ms,
    std::uint32_t last_processed_input_seq);

}  // namespace network_example

#endif  // SYNC_PUBLIC_SNAPSHOT_H_
