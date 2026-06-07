#include "protocol/public/network_packets.h"

#include <cstddef>
#include <utility>

#include "protocol/src/packet_codec.h"

namespace network_example {
namespace {

constexpr std::size_t kInputPayloadSize = 45;
constexpr std::size_t kSnapshotHeaderPayloadSize = 16;
constexpr std::size_t kEntitySnapshotPayloadSize = 68;
constexpr std::size_t kReliableEventPayloadSize = 34;
constexpr std::size_t kEntitySpawnPayloadSize = 42;
constexpr std::size_t kEntityDespawnPayloadSize = 12;
constexpr std::size_t kProjectileSpawnBatchHeaderPayloadSize = 24;
constexpr std::size_t kProjectileSpawnGroupHeaderPayloadSize = 8;
constexpr std::size_t kProjectileSpawnRecordPayloadSize = 40;

}  // namespace

std::vector<std::uint8_t> encode_input_packet(
    PeerId player_id,
    const PlayerInput& input,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kInputPayloadSize);
    payload.write_u32(player_id);
    payload.write_u32(input.input_seq);
    payload.write_u64(input.client_action_time_us);
    payload.write_u32(input.client_action_id);
    payload.write_float(input.move.x);
    payload.write_float(input.move.y);
    payload.write_float(input.aim_dir.x);
    payload.write_float(input.aim_dir.y);
    payload.write_float(input.aim_dir.z);
    payload.write_u32(input.buttons);
    payload.write_u8(input.selected_weapon);
    return protocol_internal::wrap_packet(
        MessageType::kInputPacket,
        payload.bytes(),
        sequence);
}

bool decode_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    PlayerInput* out_input) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_player_id == nullptr || out_input == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kInputPacket,
            &payload,
            &payload_size) ||
        payload_size != kInputPayloadSize) {
        return false;
    }

    PlayerInput input{};
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(out_player_id) ||
        !reader.read_u32(&input.input_seq) ||
        !reader.read_u64(&input.client_action_time_us) ||
        !reader.read_u32(&input.client_action_id) ||
        !reader.read_float(&input.move.x) ||
        !reader.read_float(&input.move.y) ||
        !reader.read_float(&input.aim_dir.x) ||
        !reader.read_float(&input.aim_dir.y) ||
        !reader.read_float(&input.aim_dir.z) ||
        !reader.read_u32(&input.buttons) ||
        !reader.read_u8(&input.selected_weapon) ||
        !reader.done()) {
        return false;
    }
    *out_input = input;
    return true;
}

std::vector<std::uint8_t> encode_snapshot_packet(
    const WorldSnapshot& snapshot,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(
        kSnapshotHeaderPayloadSize + snapshot.entities.size() * kEntitySnapshotPayloadSize);
    payload.write_u32(snapshot.header.server_tick);
    payload.write_u32(snapshot.header.server_time_ms);
    payload.write_u32(snapshot.header.last_processed_input_seq);
    payload.write_u32(static_cast<std::uint32_t>(snapshot.entities.size()));

    for (const EntitySnapshot& entity : snapshot.entities) {
        payload.write_u32(entity.net_id);
        payload.write_u16(static_cast<std::uint16_t>(entity.type));
        payload.write_u32(entity.owner_peer);
        payload.write_vec3(entity.position);
        payload.write_quat(entity.rotation);
        payload.write_vec3(entity.velocity);
        payload.write_u16(entity.hp);
        payload.write_u16(entity.max_hp);
        payload.write_u16(entity.state);
        payload.write_u32(entity.flags);
        payload.write_u32(entity.spawn_tick);
        payload.write_u32(entity.client_action_id);
    }

    return protocol_internal::wrap_packet(
        MessageType::kSnapshotPacket,
        payload.bytes(),
        sequence);
}

bool decode_snapshot_packet(
    const std::uint8_t* data,
    std::size_t size,
    WorldSnapshot* out_snapshot) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_snapshot == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kSnapshotPacket,
            &payload,
            &payload_size) ||
        payload_size < kSnapshotHeaderPayloadSize) {
        return false;
    }

    WorldSnapshot snapshot;
    protocol_internal::PacketReader reader(payload, payload_size);
    std::uint32_t entity_count = 0;
    if (!reader.read_u32(&snapshot.header.server_tick) ||
        !reader.read_u32(&snapshot.header.server_time_ms) ||
        !reader.read_u32(&snapshot.header.last_processed_input_seq) ||
        !reader.read_u32(&entity_count)) {
        return false;
    }

    const std::size_t expected_size =
        kSnapshotHeaderPayloadSize + entity_count * kEntitySnapshotPayloadSize;
    if (payload_size != expected_size) {
        return false;
    }

    snapshot.entities.reserve(entity_count);
    for (std::uint32_t index = 0; index < entity_count; ++index) {
        EntitySnapshot entity;
        std::uint16_t entity_type = 0;
        if (!reader.read_u32(&entity.net_id) ||
            !reader.read_u16(&entity_type) ||
            !reader.read_u32(&entity.owner_peer) ||
            !reader.read_vec3(&entity.position) ||
            !reader.read_quat(&entity.rotation) ||
            !reader.read_vec3(&entity.velocity) ||
            !reader.read_u16(&entity.hp) ||
            !reader.read_u16(&entity.max_hp) ||
            !reader.read_u16(&entity.state) ||
            !reader.read_u32(&entity.flags) ||
            !reader.read_u32(&entity.spawn_tick) ||
            !reader.read_u32(&entity.client_action_id)) {
            return false;
        }
        entity.type = static_cast<EntityType>(entity_type);
        snapshot.entities.push_back(entity);
    }

    if (!reader.done()) {
        return false;
    }

    *out_snapshot = std::move(snapshot);
    return true;
}

std::vector<std::uint8_t> encode_reliable_event_packet(
    const KernelEvent& event,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kReliableEventPayloadSize);
    payload.write_u16(static_cast<std::uint16_t>(event.type));
    payload.write_u32(event.tick);
    payload.write_u32(event.net_id);
    payload.write_u32(event.peer_id);
    payload.write_u32(event.code);
    payload.write_u64(event.event_time_us);
    payload.write_u64(event.presentation_time_us);
    return protocol_internal::wrap_packet(
        MessageType::kReliableEventPacket,
        payload.bytes(),
        sequence);
}

bool decode_reliable_event_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelEvent* out_event) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_event == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kReliableEventPacket,
            &payload,
            &payload_size) ||
        payload_size != kReliableEventPayloadSize) {
        return false;
    }

    KernelEvent event{};
    std::uint16_t event_type = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u16(&event_type) ||
        !reader.read_u32(&event.tick) ||
        !reader.read_u32(&event.net_id) ||
        !reader.read_u32(&event.peer_id) ||
        !reader.read_u32(&event.code) ||
        !reader.read_u64(&event.event_time_us) ||
        !reader.read_u64(&event.presentation_time_us) ||
        !reader.done()) {
        return false;
    }

    event.type = static_cast<KernelEventType>(event_type);
    *out_event = event;
    return true;
}

std::vector<std::uint8_t> encode_entity_spawn_packet(
    const EntitySpawnPacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kEntitySpawnPayloadSize);
    payload.write_u32(packet.net_id);
    payload.write_u16(static_cast<std::uint16_t>(packet.entity_type));
    payload.write_u32(packet.owner_peer);
    payload.write_u32(packet.server_tick);
    payload.write_vec3(packet.position);
    payload.write_quat(packet.rotation);
    return protocol_internal::wrap_packet(
        MessageType::kEntitySpawn,
        payload.bytes(),
        sequence);
}

bool decode_entity_spawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntitySpawnPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kEntitySpawn,
            &payload,
            &payload_size) ||
        payload_size != kEntitySpawnPayloadSize) {
        return false;
    }

    EntitySpawnPacket packet{};
    std::uint16_t entity_type = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.net_id) ||
        !reader.read_u16(&entity_type) ||
        !reader.read_u32(&packet.owner_peer) ||
        !reader.read_u32(&packet.server_tick) ||
        !reader.read_vec3(&packet.position) ||
        !reader.read_quat(&packet.rotation) ||
        !reader.done()) {
        return false;
    }
    packet.entity_type = static_cast<EntityType>(entity_type);
    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_entity_despawn_packet(
    const EntityDespawnPacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kEntityDespawnPayloadSize);
    payload.write_u32(packet.net_id);
    payload.write_u32(packet.server_tick);
    payload.write_u32(packet.reason);
    return protocol_internal::wrap_packet(
        MessageType::kEntityDespawn,
        payload.bytes(),
        sequence);
}

bool decode_entity_despawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntityDespawnPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kEntityDespawn,
            &payload,
            &payload_size) ||
        payload_size != kEntityDespawnPayloadSize) {
        return false;
    }

    EntityDespawnPacket packet{};
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.net_id) ||
        !reader.read_u32(&packet.server_tick) ||
        !reader.read_u32(&packet.reason) ||
        !reader.done()) {
        return false;
    }
    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_projectile_spawn_batch_packet(
    const ProjectileSpawnBatchPacket& packet,
    std::uint32_t sequence) {
    std::size_t record_count = 0;
    for (const ProjectileSpawnGroup& group : packet.groups) {
        record_count += group.records.size();
    }

    protocol_internal::PacketWriter payload;
    payload.reserve(
        kProjectileSpawnBatchHeaderPayloadSize +
        packet.groups.size() * kProjectileSpawnGroupHeaderPayloadSize +
        record_count * kProjectileSpawnRecordPayloadSize);
    payload.write_u32(packet.server_tick);
    payload.write_u64(packet.server_time_us);
    payload.write_u64(packet.catalog_hash);
    payload.write_u32(static_cast<std::uint32_t>(packet.groups.size()));

    for (const ProjectileSpawnGroup& group : packet.groups) {
        payload.write_u32(group.projectile_template_id);
        payload.write_u32(static_cast<std::uint32_t>(group.records.size()));
        for (const ProjectileSpawnRecord& record : group.records) {
            payload.write_u32(record.projectile_net_id);
            payload.write_u32(record.owner_net_id);
            payload.write_u32(record.owner_peer);
            payload.write_u32(record.client_action_id);
            payload.write_vec3(record.spawn_position);
            payload.write_vec3(record.initial_velocity);
        }
    }

    return protocol_internal::wrap_packet(
        MessageType::kProjectileSpawnBatch,
        payload.bytes(),
        sequence);
}

bool decode_projectile_spawn_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    ProjectileSpawnBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kProjectileSpawnBatch,
            &payload,
            &payload_size) ||
        payload_size < kProjectileSpawnBatchHeaderPayloadSize) {
        return false;
    }

    ProjectileSpawnBatchPacket packet;
    std::uint32_t group_count = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) ||
        !reader.read_u64(&packet.server_time_us) ||
        !reader.read_u64(&packet.catalog_hash) ||
        !reader.read_u32(&group_count)) {
        return false;
    }

    packet.groups.reserve(group_count);
    for (std::uint32_t group_index = 0; group_index < group_count; ++group_index) {
        ProjectileSpawnGroup group;
        std::uint32_t record_count = 0;
        if (!reader.read_u32(&group.projectile_template_id) ||
            !reader.read_u32(&record_count)) {
            return false;
        }
        group.records.reserve(record_count);
        for (std::uint32_t record_index = 0; record_index < record_count; ++record_index) {
            ProjectileSpawnRecord record;
            if (!reader.read_u32(&record.projectile_net_id) ||
                !reader.read_u32(&record.owner_net_id) ||
                !reader.read_u32(&record.owner_peer) ||
                !reader.read_u32(&record.client_action_id) ||
                !reader.read_vec3(&record.spawn_position) ||
                !reader.read_vec3(&record.initial_velocity)) {
                return false;
            }
            group.records.push_back(record);
        }
        packet.groups.push_back(std::move(group));
    }
    if (!reader.done()) {
        return false;
    }

    *out_packet = std::move(packet);
    return true;
}

}  // namespace network_example
