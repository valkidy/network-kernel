#ifndef PROTOCOL_PUBLIC_NETWORK_PACKETS_H_
#define PROTOCOL_PUBLIC_NETWORK_PACKETS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "protocol/public/packet_header.h"
#include "sync/public/snapshot.h"
#include "world/public/components.h"

namespace network_example {

struct EntitySpawnPacket {
    NetId net_id = 0;
    EntityType entity_type = EntityType::kUnknown;
    PeerId owner_peer = 0;
    std::uint32_t server_tick = 0;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct EntityDespawnPacket {
    NetId net_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t reason = 0;
};

struct ProjectileSpawnRecord {
    NetId projectile_net_id = 0;
    NetId owner_net_id = 0;
    PeerId owner_peer = 0;
    std::uint32_t client_action_id = 0;
    glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
    glm::vec3 initial_velocity{0.0f, 0.0f, 0.0f};
};

struct ProjectileSpawnGroup {
    std::uint32_t projectile_template_id = 0;
    std::vector<ProjectileSpawnRecord> records;
};

struct ProjectileSpawnBatchPacket {
    std::uint32_t server_tick = 0;
    std::uint64_t server_time_us = 0;
    std::uint64_t catalog_hash = 0;
    std::vector<ProjectileSpawnGroup> groups;
};

std::vector<std::uint8_t> encode_input_packet(
    PeerId player_id,
    const PlayerInput& input,
    std::uint32_t sequence = 0);

bool decode_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    PlayerInput* out_input);

std::vector<std::uint8_t> encode_snapshot_packet(
    const WorldSnapshot& snapshot,
    std::uint32_t sequence = 0);

bool decode_snapshot_packet(
    const std::uint8_t* data,
    std::size_t size,
    WorldSnapshot* out_snapshot);

std::size_t estimate_snapshot_base_packet_size();
std::size_t estimate_snapshot_entity_size(EntityType type);
std::size_t estimate_snapshot_entity_size(const EntitySnapshot& entity);
std::size_t estimate_snapshot_packet_size(const WorldSnapshot& snapshot);

std::vector<std::uint8_t> encode_reliable_event_packet(
    const KernelEvent& event,
    std::uint32_t sequence = 0);

bool decode_reliable_event_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelEvent* out_event);

std::vector<std::uint8_t> encode_entity_spawn_packet(
    const EntitySpawnPacket& packet,
    std::uint32_t sequence = 0);

bool decode_entity_spawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntitySpawnPacket* out_packet);

std::vector<std::uint8_t> encode_entity_despawn_packet(
    const EntityDespawnPacket& packet,
    std::uint32_t sequence = 0);

bool decode_entity_despawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntityDespawnPacket* out_packet);

std::vector<std::uint8_t> encode_projectile_spawn_batch_packet(
    const ProjectileSpawnBatchPacket& packet,
    std::uint32_t sequence = 0);

bool decode_projectile_spawn_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    ProjectileSpawnBatchPacket* out_packet);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_NETWORK_PACKETS_H_
