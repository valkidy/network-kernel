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
    std::uint32_t item_template_id = 0;
    std::uint64_t item_instance_id = 0;
    std::uint8_t world_item_mode = 0;
    NetId carrier_entity_id = 0;
};

struct WorldSnapshot {
    SnapshotHeader header;
    std::vector<EntitySnapshot> entities;
};

WorldSnapshot build_world_snapshot(
    const World& world,
    std::uint32_t server_tick,
    std::uint32_t server_time_ms,
    std::uint32_t last_processed_input_seq);

}  // namespace network_example

#endif  // SYNC_PUBLIC_SNAPSHOT_H_
