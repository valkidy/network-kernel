#include "protocol/public/network_packets.h"

#include <cstddef>
#include <cstring>
#include <utility>

namespace network_example {
namespace {

constexpr std::size_t kInputPayloadSize = 41;
constexpr std::size_t kSnapshotHeaderPayloadSize = 16;
constexpr std::size_t kEntitySnapshotPayloadSize = 66;
constexpr std::size_t kReliableEventPayloadSize = 18;
constexpr std::size_t kEntitySpawnPayloadSize = 42;
constexpr std::size_t kEntityDespawnPayloadSize = 12;

void write_u8(std::vector<std::uint8_t>* buffer, std::uint8_t value) {
    buffer->push_back(value);
}

void write_u16(std::vector<std::uint8_t>* buffer, std::uint16_t value) {
    buffer->push_back(static_cast<std::uint8_t>(value & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void write_u32(std::vector<std::uint8_t>* buffer, std::uint32_t value) {
    buffer->push_back(static_cast<std::uint8_t>(value & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    buffer->push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

void write_float(std::vector<std::uint8_t>* buffer, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    write_u32(buffer, bits);
}

bool read_u8(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    std::uint8_t* out_value) {
    if (*offset + 1 > size) {
        return false;
    }
    *out_value = data[*offset];
    *offset += 1;
    return true;
}

bool read_u16(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    std::uint16_t* out_value) {
    if (*offset + 2 > size) {
        return false;
    }
    *out_value = static_cast<std::uint16_t>(data[*offset]) |
                 static_cast<std::uint16_t>(data[*offset + 1] << 8u);
    *offset += 2;
    return true;
}

bool read_u32(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    std::uint32_t* out_value) {
    if (*offset + 4 > size) {
        return false;
    }
    *out_value = static_cast<std::uint32_t>(data[*offset]) |
                 (static_cast<std::uint32_t>(data[*offset + 1]) << 8u) |
                 (static_cast<std::uint32_t>(data[*offset + 2]) << 16u) |
                 (static_cast<std::uint32_t>(data[*offset + 3]) << 24u);
    *offset += 4;
    return true;
}

bool read_float(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    float* out_value) {
    std::uint32_t bits = 0;
    if (!read_u32(data, size, offset, &bits)) {
        return false;
    }
    std::memcpy(out_value, &bits, sizeof(float));
    return true;
}

std::vector<std::uint8_t> wrap_packet(
    MessageType message_type,
    const std::vector<std::uint8_t>& payload,
    std::uint32_t sequence) {
    PacketHeader header;
    header.message_type = static_cast<std::uint16_t>(message_type);
    header.sequence = sequence;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.payload_crc = compute_payload_crc(payload.data(), payload.size());

    const EncodedPacketHeader encoded_header = encode_packet_header(header);
    std::vector<std::uint8_t> packet;
    packet.reserve(kPacketHeaderSize + payload.size());
    packet.insert(packet.end(), encoded_header.begin(), encoded_header.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

bool unwrap_packet(
    const std::uint8_t* data,
    std::size_t size,
    MessageType expected_type,
    const std::uint8_t** out_payload,
    std::size_t* out_payload_size) {
    if (data == nullptr || out_payload == nullptr || out_payload_size == nullptr ||
        size < kPacketHeaderSize) {
        return false;
    }

    PacketHeader header;
    if (!decode_packet_header(data, size, &header) ||
        header.message_type != static_cast<std::uint16_t>(expected_type)) {
        return false;
    }

    if (header.payload_size != size - kPacketHeaderSize) {
        return false;
    }

    const std::uint8_t* payload = data + kPacketHeaderSize;
    if (compute_payload_crc(payload, header.payload_size) != header.payload_crc) {
        return false;
    }

    *out_payload = payload;
    *out_payload_size = header.payload_size;
    return true;
}

void write_vec3(std::vector<std::uint8_t>* buffer, const glm::vec3& value) {
    write_float(buffer, value.x);
    write_float(buffer, value.y);
    write_float(buffer, value.z);
}

bool read_vec3(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    glm::vec3* out_value) {
    return read_float(data, size, offset, &out_value->x) &&
           read_float(data, size, offset, &out_value->y) &&
           read_float(data, size, offset, &out_value->z);
}

void write_quat(std::vector<std::uint8_t>* buffer, const glm::quat& value) {
    write_float(buffer, value.x);
    write_float(buffer, value.y);
    write_float(buffer, value.z);
    write_float(buffer, value.w);
}

bool read_quat(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t* offset,
    glm::quat* out_value) {
    return read_float(data, size, offset, &out_value->x) &&
           read_float(data, size, offset, &out_value->y) &&
           read_float(data, size, offset, &out_value->z) &&
           read_float(data, size, offset, &out_value->w);
}

}  // namespace

std::vector<std::uint8_t> encode_input_packet(
    PeerId player_id,
    const PlayerInput& input,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kInputPayloadSize);
    write_u32(&payload, player_id);
    write_u32(&payload, input.input_seq);
    write_u32(&payload, input.client_tick);
    write_u32(&payload, input.client_projectile_id);
    write_float(&payload, input.move.x);
    write_float(&payload, input.move.y);
    write_float(&payload, input.aim_dir.x);
    write_float(&payload, input.aim_dir.y);
    write_float(&payload, input.aim_dir.z);
    write_u32(&payload, input.buttons);
    write_u8(&payload, input.selected_weapon);
    return wrap_packet(MessageType::kInputPacket, payload, sequence);
}

bool decode_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    PlayerInput* out_input) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_player_id == nullptr || out_input == nullptr ||
        !unwrap_packet(data, size, MessageType::kInputPacket, &payload, &payload_size) ||
        payload_size != kInputPayloadSize) {
        return false;
    }

    PlayerInput input{};
    std::size_t offset = 0;
    return read_u32(payload, payload_size, &offset, out_player_id) &&
           read_u32(payload, payload_size, &offset, &input.input_seq) &&
           read_u32(payload, payload_size, &offset, &input.client_tick) &&
           read_u32(payload, payload_size, &offset, &input.client_projectile_id) &&
           read_float(payload, payload_size, &offset, &input.move.x) &&
           read_float(payload, payload_size, &offset, &input.move.y) &&
           read_float(payload, payload_size, &offset, &input.aim_dir.x) &&
           read_float(payload, payload_size, &offset, &input.aim_dir.y) &&
           read_float(payload, payload_size, &offset, &input.aim_dir.z) &&
           read_u32(payload, payload_size, &offset, &input.buttons) &&
           read_u8(payload, payload_size, &offset, &input.selected_weapon) &&
           ((*out_input = input), offset == payload_size);
}

std::vector<std::uint8_t> encode_snapshot_packet(
    const WorldSnapshot& snapshot,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(
        kSnapshotHeaderPayloadSize + snapshot.entities.size() * kEntitySnapshotPayloadSize);
    write_u32(&payload, snapshot.header.server_tick);
    write_u32(&payload, snapshot.header.server_time_ms);
    write_u32(&payload, snapshot.header.last_processed_input_seq);
    write_u32(&payload, static_cast<std::uint32_t>(snapshot.entities.size()));

    for (const EntitySnapshot& entity : snapshot.entities) {
        write_u32(&payload, entity.net_id);
        write_u16(&payload, static_cast<std::uint16_t>(entity.type));
        write_u32(&payload, entity.owner_peer);
        write_vec3(&payload, entity.position);
        write_quat(&payload, entity.rotation);
        write_vec3(&payload, entity.velocity);
        write_u16(&payload, entity.hp);
        write_u16(&payload, entity.state);
        write_u32(&payload, entity.flags);
        write_u32(&payload, entity.spawn_tick);
        write_u32(&payload, entity.client_projectile_id);
    }

    return wrap_packet(MessageType::kSnapshotPacket, payload, sequence);
}

bool decode_snapshot_packet(
    const std::uint8_t* data,
    std::size_t size,
    WorldSnapshot* out_snapshot) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_snapshot == nullptr ||
        !unwrap_packet(data, size, MessageType::kSnapshotPacket, &payload, &payload_size) ||
        payload_size < kSnapshotHeaderPayloadSize) {
        return false;
    }

    WorldSnapshot snapshot;
    std::size_t offset = 0;
    std::uint32_t entity_count = 0;
    if (!read_u32(payload, payload_size, &offset, &snapshot.header.server_tick) ||
        !read_u32(payload, payload_size, &offset, &snapshot.header.server_time_ms) ||
        !read_u32(payload, payload_size, &offset, &snapshot.header.last_processed_input_seq) ||
        !read_u32(payload, payload_size, &offset, &entity_count)) {
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
        if (!read_u32(payload, payload_size, &offset, &entity.net_id) ||
            !read_u16(payload, payload_size, &offset, &entity_type) ||
            !read_u32(payload, payload_size, &offset, &entity.owner_peer) ||
            !read_vec3(payload, payload_size, &offset, &entity.position) ||
            !read_quat(payload, payload_size, &offset, &entity.rotation) ||
            !read_vec3(payload, payload_size, &offset, &entity.velocity) ||
            !read_u16(payload, payload_size, &offset, &entity.hp) ||
            !read_u16(payload, payload_size, &offset, &entity.state) ||
            !read_u32(payload, payload_size, &offset, &entity.flags) ||
            !read_u32(payload, payload_size, &offset, &entity.spawn_tick) ||
            !read_u32(payload, payload_size, &offset, &entity.client_projectile_id)) {
            return false;
        }
        entity.type = static_cast<EntityType>(entity_type);
        snapshot.entities.push_back(entity);
    }

    if (offset != payload_size) {
        return false;
    }

    *out_snapshot = std::move(snapshot);
    return true;
}

std::vector<std::uint8_t> encode_reliable_event_packet(
    const KernelEvent& event,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kReliableEventPayloadSize);
    write_u16(&payload, static_cast<std::uint16_t>(event.type));
    write_u32(&payload, event.tick);
    write_u32(&payload, event.net_id);
    write_u32(&payload, event.peer_id);
    write_u32(&payload, event.code);
    return wrap_packet(MessageType::kReliableEventPacket, payload, sequence);
}

bool decode_reliable_event_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelEvent* out_event) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_event == nullptr ||
        !unwrap_packet(
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
    std::size_t offset = 0;
    if (!read_u16(payload, payload_size, &offset, &event_type) ||
        !read_u32(payload, payload_size, &offset, &event.tick) ||
        !read_u32(payload, payload_size, &offset, &event.net_id) ||
        !read_u32(payload, payload_size, &offset, &event.peer_id) ||
        !read_u32(payload, payload_size, &offset, &event.code) ||
        offset != payload_size) {
        return false;
    }

    event.type = static_cast<KernelEventType>(event_type);
    *out_event = event;
    return true;
}

std::vector<std::uint8_t> encode_entity_spawn_packet(
    const EntitySpawnPacket& packet,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kEntitySpawnPayloadSize);
    write_u32(&payload, packet.net_id);
    write_u16(&payload, static_cast<std::uint16_t>(packet.entity_type));
    write_u32(&payload, packet.owner_peer);
    write_u32(&payload, packet.server_tick);
    write_vec3(&payload, packet.position);
    write_quat(&payload, packet.rotation);
    return wrap_packet(MessageType::kEntitySpawn, payload, sequence);
}

bool decode_entity_spawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntitySpawnPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !unwrap_packet(data, size, MessageType::kEntitySpawn, &payload, &payload_size) ||
        payload_size != kEntitySpawnPayloadSize) {
        return false;
    }

    EntitySpawnPacket packet{};
    std::uint16_t entity_type = 0;
    std::size_t offset = 0;
    if (!read_u32(payload, payload_size, &offset, &packet.net_id) ||
        !read_u16(payload, payload_size, &offset, &entity_type) ||
        !read_u32(payload, payload_size, &offset, &packet.owner_peer) ||
        !read_u32(payload, payload_size, &offset, &packet.server_tick) ||
        !read_vec3(payload, payload_size, &offset, &packet.position) ||
        !read_quat(payload, payload_size, &offset, &packet.rotation) ||
        offset != payload_size) {
        return false;
    }
    packet.entity_type = static_cast<EntityType>(entity_type);
    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_entity_despawn_packet(
    const EntityDespawnPacket& packet,
    std::uint32_t sequence) {
    std::vector<std::uint8_t> payload;
    payload.reserve(kEntityDespawnPayloadSize);
    write_u32(&payload, packet.net_id);
    write_u32(&payload, packet.server_tick);
    write_u32(&payload, packet.reason);
    return wrap_packet(MessageType::kEntityDespawn, payload, sequence);
}

bool decode_entity_despawn_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntityDespawnPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !unwrap_packet(data, size, MessageType::kEntityDespawn, &payload, &payload_size) ||
        payload_size != kEntityDespawnPayloadSize) {
        return false;
    }

    EntityDespawnPacket packet{};
    std::size_t offset = 0;
    if (!read_u32(payload, payload_size, &offset, &packet.net_id) ||
        !read_u32(payload, payload_size, &offset, &packet.server_tick) ||
        !read_u32(payload, payload_size, &offset, &packet.reason) ||
        offset != payload_size) {
        return false;
    }
    *out_packet = packet;
    return true;
}

}  // namespace network_example
