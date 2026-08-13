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
    ActorType actor_type = ActorType::kUnknown;
    PeerId owner_peer = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t actor_template_id = 0;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t entity_template_id = 0;
    std::uint32_t collider_template_id = 0;
    std::uint32_t item_template_id = 0;
    KernelItemInstanceId item_instance_id = 0;
    std::uint8_t world_item_mode = KernelWorldItemMode_Placed;
    std::uint32_t carrier_entity_id = 0;
};

struct EntityDespawnPacket {
    NetId net_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t reason = 0;
};

struct EntityTemplateUpdatePacket {
    NetId net_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t actor_template_id = 0;
};

struct ProjectileSpawnRecord {
    NetId projectile_net_id = 0;
    NetId owner_net_id = 0;
    PeerId owner_peer = 0;
    std::uint32_t action_instance_id = 0;
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

struct LocalActionResultBatchPacket {
    std::uint32_t server_tick = 0;
    std::vector<KernelLocalActionResult> records;
};

struct RemoteActionPresentationBatchPacket {
    std::uint32_t server_tick = 0;
    std::vector<KernelRemoteActionPresentationEvent> records;
};

struct StatusEffectStateRecord {
    std::uint32_t status_effect_id = 0;
    std::uint32_t status_instance_id = 0;
    NetId instigator_net_id = 0;
    std::uint32_t applied_tick = 0;
    std::uint32_t expire_tick = 0;
    std::uint16_t stack_count = 1u;
};

struct StatusEffectStatePacket {
    std::uint32_t server_tick = 0;
    NetId target_net_id = 0;
    std::uint32_t revision = 0;
    std::vector<StatusEffectStateRecord> records;
};

inline constexpr std::uint16_t kInventoryChangeQuantity = 1u << 0;
inline constexpr std::uint16_t kInventoryChangeCooldown = 1u << 1;
inline constexpr std::uint16_t kInventoryChangePortableState = 1u << 2;
inline constexpr std::uint16_t kInventoryChangeAll =
    kInventoryChangeQuantity |
    kInventoryChangeCooldown |
    kInventoryChangePortableState;

struct InventoryWireItem {
    KernelItemInstanceId item_instance_id = 0;
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    std::uint32_t next_use_tick = 0;
    std::vector<std::uint32_t> portable_values;
};

struct InventoryDeltaRecord {
    KernelInventoryDeltaType type = KernelInventoryDeltaType_Add;
    std::uint16_t slot = 0;
    std::uint16_t previous_slot = 0;
    std::uint16_t changed_fields = kInventoryChangeAll;
    InventoryWireItem item;
};

struct InventoryDeltaBatchPacket {
    KernelInventoryContainerId inventory_container_id = 0;
    std::uint64_t first_revision = 0;
    std::vector<InventoryDeltaRecord> records;
};

struct InventorySnapshotRequestPacket {
    KernelInventoryContainerId inventory_container_id = 0;
    std::uint64_t client_revision = 0;
};

struct InventorySnapshotEntry {
    std::uint16_t slot = 0;
    InventoryWireItem item;
};

struct InventorySnapshotPagePacket {
    KernelInventoryContainerId inventory_container_id = 0;
    std::uint32_t owner_entity_id = 0;
    std::uint64_t revision = 0;
    std::uint32_t slot_capacity = 0;
    std::uint16_t page_index = 0;
    std::uint16_t page_count = 0;
    std::vector<InventorySnapshotEntry> entries;
};

inline constexpr std::uint8_t kPropStateChangeMode = 1u << 0;
inline constexpr std::uint8_t kPropStateChangeTransform = 1u << 1;
inline constexpr std::uint8_t kPropStateChangeVelocity = 1u << 2;
inline constexpr std::uint8_t kPropStateChangeHealth = 1u << 3;

struct PropStateChangeRecord {
    NetId net_id = 0;
    std::uint8_t changed_fields = 0;
    KernelWorldItemMode world_mode = KernelWorldItemMode_Placed;
    NetId carrier_entity_id = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PropStateChangeBatchPacket {
    std::uint32_t server_tick = 0;
    std::vector<PropStateChangeRecord> records;
};

// One procedural step an authoritative actor's leg took, addressed to the
// entity that took it. Both swing endpoints are frozen at lift-off, so this is
// the whole of a step: a receiver reproduces the swing from it without the
// terrain, the gait, or the actor's bones. The lift-off position is absent on
// purpose -- a receiver in sync already has it as the foot's planted position.
//
// Deliberately unreliable. The landing position is absolute rather than a
// delta, so a lost step leaves that leg wrong only until its next step, and a
// baseline is sent when an entity first becomes relevant. Reliable ordered
// delivery would instead risk head-of-line blocking freezing every leg for a
// round trip, which is the worse failure.
struct LocomotionStepRecord {
    NetId net_id = 0;
    std::uint8_t leg_index = 0;
    // Ticks before the batch's server_tick that the swing began. Steps are
    // collected as they are committed and flushed with the next snapshot, so
    // this spans a snapshot interval, not a session.
    std::uint8_t start_tick_delta = 0;
    glm::vec3 landing_target_world{0.0f};
};

struct LocomotionStepBatchPacket {
    std::uint32_t server_tick = 0;
    std::vector<LocomotionStepRecord> records;
};

std::vector<std::uint8_t> encode_locomotion_step_batch_packet(
    const LocomotionStepBatchPacket& packet,
    std::uint32_t sequence = 0);

bool decode_locomotion_step_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    LocomotionStepBatchPacket* out_packet);

std::vector<std::uint8_t> encode_player_input_packet(
    PeerId player_id,
    const KernelPlayerInput& input,
    std::uint32_t sequence = 0);

bool decode_player_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    KernelPlayerInput* out_input);

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

std::vector<std::uint8_t> encode_entity_template_update_packet(
    const EntityTemplateUpdatePacket& packet,
    std::uint32_t sequence = 0);

bool decode_entity_template_update_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntityTemplateUpdatePacket* out_packet);

std::vector<std::uint8_t> encode_projectile_spawn_batch_packet(
    const ProjectileSpawnBatchPacket& packet,
    std::uint32_t sequence = 0);

bool decode_projectile_spawn_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    ProjectileSpawnBatchPacket* out_packet);

std::vector<std::uint8_t> encode_local_action_result_batch_packet(
    const LocalActionResultBatchPacket& packet,
    std::uint32_t sequence = 0);

bool decode_local_action_result_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    LocalActionResultBatchPacket* out_packet);

std::vector<std::uint8_t> encode_remote_action_presentation_batch_packet(
    const RemoteActionPresentationBatchPacket& packet,
    std::uint32_t sequence = 0);

bool decode_remote_action_presentation_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    RemoteActionPresentationBatchPacket* out_packet);

std::vector<std::uint8_t> encode_status_effect_state_packet(
    const StatusEffectStatePacket& packet,
    std::uint32_t sequence = 0);

bool decode_status_effect_state_packet(
    const std::uint8_t* data,
    std::size_t size,
    StatusEffectStatePacket* out_packet);

std::vector<std::uint8_t> encode_gameplay_request_packet(
    const KernelGameplayRequest& request,
    std::uint32_t sequence = 0);

bool decode_gameplay_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelGameplayRequest* out_request);

std::vector<std::uint8_t> encode_gameplay_request_outcome_packet(
    const KernelGameplayRequestOutcome& outcome,
    std::uint32_t sequence = 0);

bool decode_gameplay_request_outcome_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelGameplayRequestOutcome* out_outcome);

std::vector<std::uint8_t> encode_inventory_delta_batch_packet(
    const InventoryDeltaBatchPacket& packet,
    std::uint32_t sequence = 0);
bool decode_inventory_delta_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventoryDeltaBatchPacket* out_packet);

std::vector<std::uint8_t> encode_inventory_snapshot_request_packet(
    const InventorySnapshotRequestPacket& packet,
    std::uint32_t sequence = 0);
bool decode_inventory_snapshot_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventorySnapshotRequestPacket* out_packet);

std::vector<std::uint8_t> encode_inventory_snapshot_page_packet(
    const InventorySnapshotPagePacket& packet,
    std::uint32_t sequence = 0);
bool decode_inventory_snapshot_page_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventorySnapshotPagePacket* out_packet);

std::vector<std::uint8_t> encode_prop_state_change_batch_packet(
    const PropStateChangeBatchPacket& packet,
    std::uint32_t sequence = 0);
bool decode_prop_state_change_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    PropStateChangeBatchPacket* out_packet);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_NETWORK_PACKETS_H_
