#include "protocol/public/network_packets.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/src/packet_codec.h"

namespace network_example {
namespace {

constexpr std::size_t kInputPayloadSize = 57;
constexpr std::size_t kSnapshotHeaderPayloadSize = 16;
constexpr std::size_t kSnapshotSectionHeaderPayloadSize = 4;
// Sections present in every snapshot; the beam section is optional on top.
constexpr std::uint16_t kSnapshotSectionCount = 5;
constexpr std::size_t kActorSnapshotBasePayloadSize = 52;
// net_id 4 + record flags 1 + position 12 + velocity 6 + facing 2 + aim 3 +
// animation state 2 + visual flags 2.
//
// An agent is an actor that is never the receiving session's own player, which
// removes most of what the shared actor record spends its bytes on. Its type
// and actor type are implied by the section. Its facing is a quaternion holding
// one live axis -- it stands on the ground -- so a turn in a u16 says the same
// thing in an eighth of the space, and the aim vector goes the same way. Only
// position stays a full trio of floats, because narrowing it needs a bounded
// range to quantise against and that is a separate decision.
constexpr std::size_t kAgentSnapshotBasePayloadSize = 32;
constexpr std::size_t kActorActionTimelinePayloadSize = 20;
constexpr std::size_t kActorOwnerPeerPayloadSize = 4;
constexpr std::size_t kActorRotationPayloadSize = 16;
constexpr std::size_t kActorHealthPayloadSize = 4;
constexpr std::size_t kActorMovementPayloadSize = 22;
constexpr std::size_t kProjectileCompactSnapshotPayloadSize = 34;
// net_id 4 + effective_length 2. No position, rotation or velocity: a beam does
// not move, and its origin and aim are the shooter's, which every snapshot
// already carries in the actor section. No state or flags either -- beam
// templates author speed 0 and carry neither ReplicationState nor HomingState,
// so both were always zero on the wire. Only the reach is genuinely the beam's
// own, and only because the server is what decides where it stops.
constexpr std::size_t kProjectileBeamSnapshotPayloadSize = 6;
// Beam reach on the wire: centimetres in a u16. Templates author 8 m and 14 m
// beams today and the u16 still covers 655.35 m, while 1 cm is far below what
// the presentation can resolve at any of those ranges.
constexpr float kBeamLengthWireScale = 100.0f;
constexpr float kBeamLengthWireMax = 65535.0f / kBeamLengthWireScale;
constexpr std::size_t kProjectileHybridCorrectionSnapshotPayloadSize = 46;
constexpr std::size_t kGenericSnapshotPayloadSize = 44;
constexpr std::size_t kGenericHealthPayloadSize = 4;
constexpr std::size_t kReliableEventPayloadSize = 34;
constexpr std::size_t kEntitySpawnPayloadSize = 73;
constexpr std::size_t kEntityDespawnPayloadSize = 12;
constexpr std::size_t kEntityTemplateUpdatePayloadSize = 12;
constexpr std::size_t kProjectileSpawnBatchHeaderPayloadSize = 24;
constexpr std::size_t kProjectileSpawnGroupHeaderPayloadSize = 8;
constexpr std::size_t kProjectileSpawnRecordPayloadSize = 40;
constexpr std::size_t kActionBatchHeaderPayloadSize = 8;
constexpr std::size_t kLocalActionResultPayloadSize = 12;
constexpr std::size_t kRemoteActionPresentationPayloadSize = 32;
constexpr std::size_t kStatusEffectStateHeaderPayloadSize = 16;
constexpr std::size_t kStatusEffectStateRecordPayloadSize = 24;
constexpr std::size_t kGameplayRequestPayloadSize = 60;
constexpr std::size_t kGameplayRequestOutcomePayloadSize = 32;
constexpr std::size_t kInventorySnapshotRequestPayloadSize = 16;
constexpr std::size_t kMaxInventoryPacketPayloadSize = 16u * 1024u;

constexpr float kTwoPi = 6.283185307179586f;
constexpr float kHalfPi = 1.5707963267948966f;
constexpr float kYawWireSteps = 65536.0f;
// A turn in a u16 is 0.005 degrees a step, which no presentation resolves, and
// it replaces a four-component quaternion carrying one live axis.
std::uint16_t yaw_to_wire(float radians) {
    float turns = radians / kTwoPi;
    turns -= std::floor(turns);
    return static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(std::lround(turns * kYawWireSteps)) & 0xFFFFu);
}

float yaw_from_wire(std::uint16_t value) {
    return static_cast<float>(value) / kYawWireSteps * kTwoPi;
}

// Pitch in a signed byte over a half turn: 0.7 degrees a step. Aim only has to
// point a muzzle for presentation here -- the server hits what it hits from its
// own state, never from this.
std::int8_t pitch_to_wire(float radians) {
    const float clamped = std::clamp(radians, -kHalfPi, kHalfPi);
    return static_cast<std::int8_t>(std::lround(clamped / kHalfPi * 127.0f));
}

float pitch_from_wire(std::int8_t value) {
    return static_cast<float>(value) / 127.0f * kHalfPi;
}

// Velocity in i16 at 1/256 m/s: 4 mm/s a step against a walking 2.5 m/s, and
// 128 m/s of range. Quantised rather than dropped, because the client feeds it
// into the render state and interpolates it between snapshots -- unlike the
// rotation's three dead axes, something downstream can see this one.
constexpr float kVelocityWireScale = 256.0f;

std::uint16_t velocity_to_wire(float value) {
    const float clamped =
        std::clamp(value * kVelocityWireScale, -32768.0f, 32767.0f);
    return static_cast<std::uint16_t>(
        static_cast<std::int16_t>(std::lround(clamped)));
}

float velocity_from_wire(std::uint16_t value) {
    return static_cast<float>(static_cast<std::int16_t>(value)) /
        kVelocityWireScale;
}

// +X is forward, the same convention update_vision_states resolves a vision
// cone with.
float yaw_of_direction(const glm::vec3& direction) {
    if (std::fabs(direction.x) < 1e-6f && std::fabs(direction.z) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(direction.z, direction.x);
}

glm::quat rotation_from_yaw(float yaw) {
    // Negated because a positive turn about +Y takes +X towards -Z, and
    // yaw_of_direction measures towards +Z.
    return glm::angleAxis(-yaw, glm::vec3{0.0f, 1.0f, 0.0f});
}

std::uint16_t beam_length_to_wire(float length) {
    const float clamped = std::clamp(length, 0.0f, kBeamLengthWireMax);
    return static_cast<std::uint16_t>(
        std::lround(clamped * kBeamLengthWireScale));
}

float beam_length_from_wire(std::uint16_t value) {
    return static_cast<float>(value) / kBeamLengthWireScale;
}

bool valid_wire_item(const InventoryWireItem& item) {
    return item.item_instance_id != 0u && item.item_template_id != 0u &&
        item.quantity != 0u &&
        item.portable_values.size() <= KERNEL_MAX_PORTABLE_STATE_FIELDS;
}

void write_wire_item(
    protocol_internal::PacketWriter* writer,
    const InventoryWireItem& item,
    bool include_template,
    std::uint16_t changed_fields) {
    writer->write_u64(item.item_instance_id);
    if (include_template) writer->write_u32(item.item_template_id);
    if ((changed_fields & kInventoryChangeQuantity) != 0u) {
        writer->write_u32(item.quantity);
    }
    if ((changed_fields & kInventoryChangeCooldown) != 0u) {
        writer->write_u32(item.next_use_tick);
    }
    if ((changed_fields & kInventoryChangePortableState) != 0u) {
        writer->write_u8(
            static_cast<std::uint8_t>(item.portable_values.size()));
        for (const std::uint32_t value : item.portable_values) {
            writer->write_u32(value);
        }
    }
}

bool read_wire_item(
    protocol_internal::PacketReader* reader,
    InventoryWireItem* item,
    bool include_template,
    std::uint16_t changed_fields) {
    if (!reader->read_u64(&item->item_instance_id)) return false;
    if (include_template && !reader->read_u32(&item->item_template_id)) {
        return false;
    }
    if ((changed_fields & kInventoryChangeQuantity) != 0u &&
        !reader->read_u32(&item->quantity)) {
        return false;
    }
    if ((changed_fields & kInventoryChangeCooldown) != 0u &&
        !reader->read_u32(&item->next_use_tick)) {
        return false;
    }
    if ((changed_fields & kInventoryChangePortableState) != 0u) {
        std::uint8_t count = 0;
        if (!reader->read_u8(&count) ||
            count > KERNEL_MAX_PORTABLE_STATE_FIELDS) {
            return false;
        }
        item->portable_values.resize(count);
        for (std::uint32_t& value : item->portable_values) {
            if (!reader->read_u32(&value)) return false;
        }
    }
    return item->item_instance_id != 0u;
}

enum class SnapshotSectionType : std::uint16_t {
    kActor = 1,
    kProjectileCompact = 2,
    kProjectileHybridCorrection = 3,
    kGeneric = 4,
    kProjectileBeam = 5,
    kActorAgent = 6,
};

enum AgentSnapshotRecordFlag : std::uint8_t {
    kAgentSnapshotHasActionTimeline = 1u << 0,
};

enum ActorSnapshotRecordFlag : std::uint16_t {
    kActorSnapshotHasOwnerPeer = 1u << 0,
    kActorSnapshotHasRotation = 1u << 1,
    kActorSnapshotHasHealth = 1u << 2,
    kActorSnapshotHasActionTimeline = 1u << 3,
    kActorSnapshotHasMovementState = 1u << 4,
};

bool is_actor_entity_type(EntityType type) {
    return type == EntityType::kActor;
}

std::uint16_t actor_record_flags(const EntitySnapshot& entity) {
    std::uint16_t flags = kActorSnapshotHasRotation;
    if (entity.actor_type == ActorType::kPlayer) {
        flags |= kActorSnapshotHasOwnerPeer;
        if ((entity.state_flags & kSnapshotStateFlagHpUnknown) == 0u) {
            flags |= kActorSnapshotHasHealth;
        }
    }
    if (entity.action_template_id != 0u ||
        entity.action_phase != KernelActionPhase_None) {
        flags |= kActorSnapshotHasActionTimeline;
    }
    if (entity.has_authoritative_movement_state) {
        flags |= kActorSnapshotHasMovementState;
    }
    return flags;
}

std::uint8_t agent_record_flags(const EntitySnapshot& entity) {
    std::uint8_t flags = 0;
    if (entity.action_template_id != 0u ||
        entity.action_phase != KernelActionPhase_None) {
        flags |= kAgentSnapshotHasActionTimeline;
    }
    return flags;
}

bool is_hybrid_correction_projectile(const EntitySnapshot& entity) {
    return entity.type == EntityType::kProjectile &&
           (entity.state_flags & kSnapshotStateFlagProjectileHybridCorrection) != 0u;
}

glm::quat projectile_rotation_from_velocity(const glm::vec3& velocity) {
    const float speed = glm::length(velocity);
    if (speed <= 0.001f) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    const glm::vec3 from{1.0f, 0.0f, 0.0f};
    const glm::vec3 to = velocity / speed;
    const float dot = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    if (dot > 0.999f) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    if (dot < -0.999f) {
        return glm::quat{0.0f, 0.0f, 1.0f, 0.0f};
    }
    const glm::vec3 axis = glm::normalize(glm::cross(from, to));
    return glm::angleAxis(std::acos(dot), axis);
}

SnapshotSectionType snapshot_section_type(EntityType type) {
    if (is_actor_entity_type(type)) {
        return SnapshotSectionType::kActor;
    }
    if (type == EntityType::kProjectile) {
        return SnapshotSectionType::kProjectileCompact;
    }
    return SnapshotSectionType::kGeneric;
}

SnapshotSectionType snapshot_section_type(const EntitySnapshot& entity) {
    if (is_actor_entity_type(entity.type)) {
        // Movement state is written for the receiving session's own player and
        // nothing else, so an actor carrying it is never eligible for the
        // agent record however its actor type reads.
        return entity.actor_type == ActorType::kAgent &&
                !entity.has_authoritative_movement_state
            ? SnapshotSectionType::kActorAgent
            : SnapshotSectionType::kActor;
    }
    if (is_hybrid_correction_projectile(entity)) {
        return SnapshotSectionType::kProjectileHybridCorrection;
    }
    if (entity.type == EntityType::kProjectile &&
        (entity.state_flags & kSnapshotStateFlagProjectileBeam) != 0u) {
        return SnapshotSectionType::kProjectileBeam;
    }
    if (entity.type == EntityType::kProjectile) {
        return SnapshotSectionType::kProjectileCompact;
    }
    return SnapshotSectionType::kGeneric;
}

std::vector<const EntitySnapshot*> section_entities(
    const WorldSnapshot& snapshot,
    SnapshotSectionType section_type) {
    std::vector<const EntitySnapshot*> entities;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (snapshot_section_type(entity) == section_type) {
            entities.push_back(&entity);
        }
    }
    return entities;
}

}  // namespace

std::vector<std::uint8_t> encode_player_input_packet(
    PeerId player_id,
    const KernelPlayerInput& input,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kInputPayloadSize);
    payload.write_u32(player_id);
    payload.write_u32(input.input_seq);
    payload.write_u64(input.client_action_time_us);
    payload.write_float(input.move.x);
    payload.write_float(input.move.y);
    payload.write_float(input.aim_dir.x);
    payload.write_float(input.aim_dir.y);
    payload.write_float(input.aim_dir.z);
    payload.write_u32(input.buttons);
    payload.write_u8(input.selected_weapon);
    payload.write_u32(input.action_intent.action_instance_id);
    payload.write_u16(input.action_intent.binding_id);
    payload.write_u8(input.action_intent.flags);
    payload.write_u8(input.action_intent.reserved);
    payload.write_u32(input.action_input.action_instance_id);
    payload.write_u8(input.action_input.held);
    payload.write_u8(input.action_input.flags);
    payload.write_u16(input.action_input.reserved);
    return protocol_internal::wrap_packet(
        MessageType::kPlayerInputPacket,
        payload.bytes(),
        sequence);
}

bool decode_player_input_packet(
    const std::uint8_t* data,
    std::size_t size,
    PeerId* out_player_id,
    KernelPlayerInput* out_input) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_player_id == nullptr || out_input == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kPlayerInputPacket,
            &payload,
            &payload_size) ||
        payload_size != kInputPayloadSize) {
        return false;
    }

    KernelPlayerInput input{};
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(out_player_id) ||
        !reader.read_u32(&input.input_seq) ||
        !reader.read_u64(&input.client_action_time_us) ||
        !reader.read_float(&input.move.x) ||
        !reader.read_float(&input.move.y) ||
        !reader.read_float(&input.aim_dir.x) ||
        !reader.read_float(&input.aim_dir.y) ||
        !reader.read_float(&input.aim_dir.z) ||
        !reader.read_u32(&input.buttons) ||
        !reader.read_u8(&input.selected_weapon) ||
        !reader.read_u32(&input.action_intent.action_instance_id) ||
        !reader.read_u16(&input.action_intent.binding_id) ||
        !reader.read_u8(&input.action_intent.flags) ||
        !reader.read_u8(&input.action_intent.reserved) ||
        !reader.read_u32(&input.action_input.action_instance_id) ||
        !reader.read_u8(&input.action_input.held) ||
        !reader.read_u8(&input.action_input.flags) ||
        !reader.read_u16(&input.action_input.reserved) ||
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
    payload.reserve(estimate_snapshot_packet_size(snapshot) - kPacketHeaderSize);
    payload.write_u32(snapshot.header.server_tick);
    payload.write_u32(snapshot.header.server_time_ms);
    payload.write_u32(snapshot.header.last_processed_input_seq);
    // Beams are rare, so their section is written only when one exists rather
    // than costing every snapshot in every game an empty 4-byte header. The
    // reader is driven by this count, so a variable number of sections is fine.
    const bool has_beam_section =
        !section_entities(snapshot, SnapshotSectionType::kProjectileBeam).empty();
    payload.write_u16(
        has_beam_section ? kSnapshotSectionCount + 1 : kSnapshotSectionCount);
    payload.write_u16(0);

    for (const SnapshotSectionType section_type : {
             SnapshotSectionType::kActor,
             SnapshotSectionType::kActorAgent,
             SnapshotSectionType::kProjectileCompact,
             SnapshotSectionType::kProjectileHybridCorrection,
             SnapshotSectionType::kProjectileBeam,
             SnapshotSectionType::kGeneric,
         }) {
        if (section_type == SnapshotSectionType::kProjectileBeam &&
            !has_beam_section) {
            continue;
        }
        const std::vector<const EntitySnapshot*> entities =
            section_entities(snapshot, section_type);
        payload.write_u16(static_cast<std::uint16_t>(section_type));
        payload.write_u16(static_cast<std::uint16_t>(entities.size()));
        for (const EntitySnapshot* entity : entities) {
            payload.write_u32(entity->net_id);
            switch (section_type) {
                case SnapshotSectionType::kActor: {
                    const std::uint16_t record_flags = actor_record_flags(*entity);
                    payload.write_u16(static_cast<std::uint16_t>(entity->type));
                    payload.write_u16(static_cast<std::uint16_t>(entity->actor_type));
                    payload.write_u16(record_flags);
                    payload.write_vec3(entity->position);
                    payload.write_vec3(entity->velocity);
                    payload.write_u16(entity->state);
                    payload.write_u32(entity->flags);
                    payload.write_vec3(entity->aim_direction);
                    if ((record_flags & kActorSnapshotHasOwnerPeer) != 0u) {
                        payload.write_u32(entity->owner_peer);
                    }
                    if ((record_flags & kActorSnapshotHasRotation) != 0u) {
                        payload.write_quat(entity->rotation);
                    }
                    if ((record_flags & kActorSnapshotHasHealth) != 0u) {
                        payload.write_u16(entity->hp);
                        payload.write_u16(entity->max_hp);
                    }
                    if ((record_flags & kActorSnapshotHasActionTimeline) != 0u) {
                        payload.write_u32(entity->action_template_id);
                        payload.write_u32(entity->action_instance_id);
                        payload.write_u32(entity->action_start_tick);
                        payload.write_u32(entity->action_commit_count);
                        payload.write_u16(entity->action_phase);
                        payload.write_u16(0u);
                    }
                    if ((record_flags & kActorSnapshotHasMovementState) != 0u) {
                        payload.write_u16(entity->ground_state);
                        payload.write_vec3(entity->ground_normal);
                        payload.write_u32(entity->supporting_entity_net_id);
                        payload.write_u32(entity->supporting_collider_id);
                    }
                    break;
                }
                case SnapshotSectionType::kActorAgent: {
                    const std::uint8_t record_flags = agent_record_flags(*entity);
                    payload.write_u8(record_flags);
                    payload.write_vec3(entity->position);
                    payload.write_u16(velocity_to_wire(entity->velocity.x));
                    payload.write_u16(velocity_to_wire(entity->velocity.y));
                    payload.write_u16(velocity_to_wire(entity->velocity.z));
                    payload.write_u16(yaw_to_wire(yaw_of_direction(
                        entity->rotation * glm::vec3{1.0f, 0.0f, 0.0f})));
                    const glm::vec3 aim = entity->aim_direction;
                    const float aim_length = glm::length(aim);
                    // A zero aim cannot be carried as angles at all. It decodes
                    // as +X, which is what EntitySnapshot defaults to and what
                    // an agent with no ActionInputState already reports.
                    payload.write_u16(yaw_to_wire(yaw_of_direction(aim)));
                    payload.write_u8(static_cast<std::uint8_t>(pitch_to_wire(
                        aim_length > 1e-6f
                            ? std::asin(std::clamp(aim.y / aim_length, -1.0f, 1.0f))
                            : 0.0f)));
                    payload.write_u16(entity->state);
                    payload.write_u16(static_cast<std::uint16_t>(entity->flags));
                    if ((record_flags & kAgentSnapshotHasActionTimeline) != 0u) {
                        payload.write_u32(entity->action_template_id);
                        payload.write_u32(entity->action_instance_id);
                        payload.write_u32(entity->action_start_tick);
                        payload.write_u32(entity->action_commit_count);
                        payload.write_u16(entity->action_phase);
                        payload.write_u16(0u);
                    }
                    break;
                }
                case SnapshotSectionType::kProjectileCompact:
                    payload.write_vec3(entity->position);
                    payload.write_vec3(entity->velocity);
                    payload.write_u16(entity->state);
                    payload.write_u32(entity->flags);
                    break;
                case SnapshotSectionType::kProjectileBeam:
                    payload.write_u16(
                        beam_length_to_wire(entity->beam_effective_length));
                    break;
                case SnapshotSectionType::kProjectileHybridCorrection:
                    payload.write_u32(entity->owner_peer);
                    payload.write_vec3(entity->position);
                    payload.write_vec3(entity->velocity);
                    payload.write_u16(entity->state);
                    payload.write_u32(entity->flags);
                    payload.write_u32(entity->spawn_tick);
                    payload.write_u32(entity->action_instance_id);
                    break;
                case SnapshotSectionType::kGeneric:
                    payload.write_u16(static_cast<std::uint16_t>(entity->type));
                    payload.write_u32(entity->owner_peer);
                    payload.write_vec3(entity->position);
                    payload.write_vec3(entity->velocity);
                    payload.write_u16(entity->state);
                    payload.write_u32(entity->flags);
                    payload.write_u32(entity->state_flags);
                    if ((entity->state_flags & kSnapshotStateFlagHpUnknown) == 0u) {
                        payload.write_u16(entity->hp);
                        payload.write_u16(entity->max_hp);
                    }
                    break;
            }
        }
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
    std::uint16_t section_count = 0;
    std::uint16_t reserved = 0;
    if (!reader.read_u32(&snapshot.header.server_tick) ||
        !reader.read_u32(&snapshot.header.server_time_ms) ||
        !reader.read_u32(&snapshot.header.last_processed_input_seq) ||
        !reader.read_u16(&section_count) ||
        !reader.read_u16(&reserved)) {
        return false;
    }

    // The beam section is only written when a beam exists, so a snapshot carries
    // either the four sections every packet always has or those plus the beam
    // one. Requiring exactly four here rejected the entire packet the moment
    // anything fired a beam, which drops every entity in it, not just the beam.
    if ((section_count != kSnapshotSectionCount &&
         section_count != kSnapshotSectionCount + 1) ||
        reserved != 0) {
        return false;
    }

    for (std::uint16_t section_index = 0; section_index < section_count; ++section_index) {
        std::uint16_t raw_section_type = 0;
        std::uint16_t entity_count = 0;
        if (!reader.read_u16(&raw_section_type) ||
            !reader.read_u16(&entity_count)) {
            return false;
        }
        const SnapshotSectionType section_type =
            static_cast<SnapshotSectionType>(raw_section_type);
        for (std::uint16_t index = 0; index < entity_count; ++index) {
            EntitySnapshot entity;
            if (!reader.read_u32(&entity.net_id)) {
                return false;
            }
            switch (section_type) {
                case SnapshotSectionType::kActor: {
                    std::uint16_t entity_type = 0;
                    std::uint16_t actor_type = 0;
                    std::uint16_t record_flags = 0;
                    if (!reader.read_u16(&entity_type) ||
                        !reader.read_u16(&actor_type) ||
                        !reader.read_u16(&record_flags) ||
                        !reader.read_vec3(&entity.position) ||
                        !reader.read_vec3(&entity.velocity) ||
                        !reader.read_u16(&entity.state) ||
                        !reader.read_u32(&entity.flags) ||
                        !reader.read_vec3(&entity.aim_direction)) {
                        return false;
                    }
                    entity.type = static_cast<EntityType>(entity_type);
                    entity.actor_type = static_cast<ActorType>(actor_type);
                    if ((record_flags & kActorSnapshotHasOwnerPeer) != 0u &&
                        !reader.read_u32(&entity.owner_peer)) {
                        return false;
                    }
                    if ((record_flags & kActorSnapshotHasRotation) != 0u &&
                        !reader.read_quat(&entity.rotation)) {
                        return false;
                    }
                    if ((record_flags & kActorSnapshotHasHealth) != 0u) {
                        if (!reader.read_u16(&entity.hp) ||
                            !reader.read_u16(&entity.max_hp)) {
                            return false;
                        }
                    } else {
                        entity.state_flags |= kSnapshotStateFlagHpUnknown;
                    }
                    if ((record_flags & kActorSnapshotHasActionTimeline) != 0u) {
                        std::uint16_t action_phase = 0;
                        std::uint16_t action_reserved = 0;
                        if (!reader.read_u32(&entity.action_template_id) ||
                            !reader.read_u32(&entity.action_instance_id) ||
                            !reader.read_u32(&entity.action_start_tick) ||
                            !reader.read_u32(&entity.action_commit_count) ||
                            !reader.read_u16(&action_phase) ||
                            !reader.read_u16(&action_reserved) ||
                            action_phase > KernelActionPhase_Recovery ||
                            action_reserved != 0u) {
                            return false;
                        }
                        entity.action_phase =
                            static_cast<std::uint8_t>(action_phase);
                    }
                    if ((record_flags & kActorSnapshotHasMovementState) != 0u) {
                        if (!reader.read_u16(&entity.ground_state) ||
                            entity.ground_state > 2u ||
                            !reader.read_vec3(&entity.ground_normal) ||
                            !reader.read_u32(&entity.supporting_entity_net_id) ||
                            !reader.read_u32(&entity.supporting_collider_id)) {
                            return false;
                        }
                        entity.has_authoritative_movement_state = true;
                    }
                    break;
                }
                case SnapshotSectionType::kActorAgent: {
                    std::uint8_t record_flags = 0;
                    std::uint16_t velocity_x = 0;
                    std::uint16_t velocity_y = 0;
                    std::uint16_t velocity_z = 0;
                    std::uint16_t facing_yaw = 0;
                    std::uint16_t aim_yaw = 0;
                    std::uint8_t aim_pitch = 0;
                    std::uint16_t visual_flags = 0;
                    if (!reader.read_u8(&record_flags) ||
                        !reader.read_vec3(&entity.position) ||
                        !reader.read_u16(&velocity_x) ||
                        !reader.read_u16(&velocity_y) ||
                        !reader.read_u16(&velocity_z) ||
                        !reader.read_u16(&facing_yaw) ||
                        !reader.read_u16(&aim_yaw) ||
                        !reader.read_u8(&aim_pitch) ||
                        !reader.read_u16(&entity.state) ||
                        !reader.read_u16(&visual_flags)) {
                        return false;
                    }
                    entity.type = EntityType::kActor;
                    entity.actor_type = ActorType::kAgent;
                    entity.state_flags |= kSnapshotStateFlagHpUnknown;
                    entity.velocity = glm::vec3{
                        velocity_from_wire(velocity_x),
                        velocity_from_wire(velocity_y),
                        velocity_from_wire(velocity_z)};
                    entity.rotation = rotation_from_yaw(yaw_from_wire(facing_yaw));
                    const float aim_yaw_radians = yaw_from_wire(aim_yaw);
                    const float aim_pitch_radians =
                        pitch_from_wire(static_cast<std::int8_t>(aim_pitch));
                    const float aim_cos_pitch = std::cos(aim_pitch_radians);
                    entity.aim_direction = glm::vec3{
                        aim_cos_pitch * std::cos(aim_yaw_radians),
                        std::sin(aim_pitch_radians),
                        aim_cos_pitch * std::sin(aim_yaw_radians)};
                    entity.flags = visual_flags;
                    if ((record_flags & kAgentSnapshotHasActionTimeline) != 0u) {
                        std::uint16_t action_phase = 0;
                        std::uint16_t padding = 0;
                        if (!reader.read_u32(&entity.action_template_id) ||
                            !reader.read_u32(&entity.action_instance_id) ||
                            !reader.read_u32(&entity.action_start_tick) ||
                            !reader.read_u32(&entity.action_commit_count) ||
                            !reader.read_u16(&action_phase) ||
                            !reader.read_u16(&padding) ||
                            action_phase > KernelActionPhase_Recovery ||
                            padding != 0u) {
                            return false;
                        }
                        entity.action_phase =
                            static_cast<std::uint8_t>(action_phase);
                    }
                    break;
                }
                case SnapshotSectionType::kProjectileCompact:
                    entity.type = EntityType::kProjectile;
                    entity.state_flags |= kSnapshotStateFlagHpUnknown;
                    if (!reader.read_vec3(&entity.position) ||
                        !reader.read_vec3(&entity.velocity) ||
                        !reader.read_u16(&entity.state) ||
                        !reader.read_u32(&entity.flags)) {
                        return false;
                    }
                    entity.rotation = projectile_rotation_from_velocity(entity.velocity);
                    break;
                case SnapshotSectionType::kProjectileBeam: {
                    entity.type = EntityType::kProjectile;
                    entity.state_flags |= kSnapshotStateFlagHpUnknown;
                    entity.state_flags |= kSnapshotStateFlagProjectileBeam;
                    std::uint16_t reach = 0;
                    if (!reader.read_u16(&reach)) {
                        return false;
                    }
                    entity.beam_effective_length = beam_length_from_wire(reach);
                    // position and rotation stay at their defaults: only the
                    // owner knows where this beam starts and which way it
                    // points, and the decoder cannot see the shooter. The
                    // kernel fills both in before the snapshot is rendered.
                    break;
                }
                case SnapshotSectionType::kProjectileHybridCorrection:
                    entity.type = EntityType::kProjectile;
                    entity.state_flags |= kSnapshotStateFlagHpUnknown;
                    entity.state_flags |= kSnapshotStateFlagProjectileHybridCorrection;
                    if (!reader.read_u32(&entity.owner_peer) ||
                        !reader.read_vec3(&entity.position) ||
                        !reader.read_vec3(&entity.velocity) ||
                        !reader.read_u16(&entity.state) ||
                        !reader.read_u32(&entity.flags) ||
                        !reader.read_u32(&entity.spawn_tick) ||
                        !reader.read_u32(&entity.action_instance_id)) {
                        return false;
                    }
                    entity.rotation = projectile_rotation_from_velocity(entity.velocity);
                    break;
                case SnapshotSectionType::kGeneric: {
                    std::uint16_t entity_type = 0;
                    if (!reader.read_u16(&entity_type) ||
                        !reader.read_u32(&entity.owner_peer) ||
                        !reader.read_vec3(&entity.position) ||
                        !reader.read_vec3(&entity.velocity) ||
                        !reader.read_u16(&entity.state) ||
                        !reader.read_u32(&entity.flags) ||
                        !reader.read_u32(&entity.state_flags)) {
                        return false;
                    }
                    if ((entity.state_flags & kSnapshotStateFlagHpUnknown) == 0u &&
                        (!reader.read_u16(&entity.hp) ||
                         !reader.read_u16(&entity.max_hp))) {
                        return false;
                    }
                    entity.type = static_cast<EntityType>(entity_type);
                    break;
                }
                default:
                    return false;
            }
            snapshot.entities.push_back(entity);
        }
    }

    if (!reader.done()) {
        return false;
    }

    *out_snapshot = std::move(snapshot);
    return true;
}

std::size_t estimate_snapshot_base_packet_size() {
    return kPacketHeaderSize + kSnapshotHeaderPayloadSize +
           kSnapshotSectionCount * kSnapshotSectionHeaderPayloadSize;
}

std::size_t estimate_snapshot_entity_size(EntityType type) {
    switch (type) {
        case EntityType::kActor:
            return kActorSnapshotBasePayloadSize +
                   kActorRotationPayloadSize;
        case EntityType::kProjectile:
            return kProjectileCompactSnapshotPayloadSize;
        default:
            return kGenericSnapshotPayloadSize;
    }
}

std::size_t estimate_snapshot_entity_size(const EntitySnapshot& entity) {
    switch (snapshot_section_type(entity)) {
        case SnapshotSectionType::kActor:
            return kActorSnapshotBasePayloadSize +
                   ((actor_record_flags(entity) & kActorSnapshotHasOwnerPeer) != 0u
                        ? kActorOwnerPeerPayloadSize
                        : 0u) +
                   ((actor_record_flags(entity) & kActorSnapshotHasRotation) != 0u
                        ? kActorRotationPayloadSize
                        : 0u) +
                   ((actor_record_flags(entity) & kActorSnapshotHasHealth) != 0u
                        ? kActorHealthPayloadSize
                        : 0u) +
                   ((actor_record_flags(entity) &
                     kActorSnapshotHasActionTimeline) != 0u
                        ? kActorActionTimelinePayloadSize
                        : 0u) +
                   ((actor_record_flags(entity) &
                     kActorSnapshotHasMovementState) != 0u
                        ? kActorMovementPayloadSize
                        : 0u);
        case SnapshotSectionType::kActorAgent:
            return kAgentSnapshotBasePayloadSize +
                   ((agent_record_flags(entity) &
                     kAgentSnapshotHasActionTimeline) != 0u
                        ? kActorActionTimelinePayloadSize
                        : 0u);
        case SnapshotSectionType::kProjectileCompact:
            return kProjectileCompactSnapshotPayloadSize;
        case SnapshotSectionType::kProjectileBeam:
            // Plus the section header, which only exists because this beam does.
            // Charging it to every beam rather than only the first over-counts
            // slightly, which keeps the send budget on the conservative side.
            return kProjectileBeamSnapshotPayloadSize +
                   kSnapshotSectionHeaderPayloadSize;
        case SnapshotSectionType::kProjectileHybridCorrection:
            return kProjectileHybridCorrectionSnapshotPayloadSize;
        case SnapshotSectionType::kGeneric:
        default:
            return kGenericSnapshotPayloadSize +
                ((entity.state_flags & kSnapshotStateFlagHpUnknown) == 0u
                    ? kGenericHealthPayloadSize
                    : 0u);
    }
}

std::size_t estimate_snapshot_packet_size(const WorldSnapshot& snapshot) {
    std::size_t size = estimate_snapshot_base_packet_size();
    for (const EntitySnapshot& entity : snapshot.entities) {
        size += estimate_snapshot_entity_size(entity);
    }
    return size;
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
    payload.write_u16(static_cast<std::uint16_t>(packet.actor_type));
    payload.write_u32(packet.owner_peer);
    payload.write_u32(packet.server_tick);
    payload.write_u32(packet.actor_template_id);
    payload.write_vec3(packet.position);
    payload.write_quat(packet.rotation);
    payload.write_u32(packet.entity_template_id);
    payload.write_u32(packet.collider_template_id);
    payload.write_u32(packet.item_template_id);
    payload.write_u64(packet.item_instance_id);
    payload.write_u8(packet.world_item_mode);
    payload.write_u32(packet.carrier_entity_id);
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
    std::uint16_t actor_type = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.net_id) ||
        !reader.read_u16(&entity_type) ||
        !reader.read_u16(&actor_type) ||
        !reader.read_u32(&packet.owner_peer) ||
        !reader.read_u32(&packet.server_tick) ||
        !reader.read_u32(&packet.actor_template_id) ||
        !reader.read_vec3(&packet.position) ||
        !reader.read_quat(&packet.rotation) ||
        !reader.read_u32(&packet.entity_template_id) ||
        !reader.read_u32(&packet.collider_template_id) ||
        !reader.read_u32(&packet.item_template_id) ||
        !reader.read_u64(&packet.item_instance_id) ||
        !reader.read_u8(&packet.world_item_mode) ||
        packet.world_item_mode > KernelWorldItemMode_InFlight ||
        !reader.read_u32(&packet.carrier_entity_id) ||
        !reader.done()) {
        return false;
    }
    packet.entity_type = static_cast<EntityType>(entity_type);
    packet.actor_type = static_cast<ActorType>(actor_type);
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

std::vector<std::uint8_t> encode_entity_template_update_packet(
    const EntityTemplateUpdatePacket& packet,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kEntityTemplateUpdatePayloadSize);
    payload.write_u32(packet.net_id);
    payload.write_u32(packet.server_tick);
    payload.write_u32(packet.actor_template_id);
    return protocol_internal::wrap_packet(
        MessageType::kEntityTemplateUpdate,
        payload.bytes(),
        sequence);
}

bool decode_entity_template_update_packet(
    const std::uint8_t* data,
    std::size_t size,
    EntityTemplateUpdatePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kEntityTemplateUpdate,
            &payload,
            &payload_size) ||
        payload_size != kEntityTemplateUpdatePayloadSize) {
        return false;
    }

    EntityTemplateUpdatePacket packet{};
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.net_id) ||
        !reader.read_u32(&packet.server_tick) ||
        !reader.read_u32(&packet.actor_template_id) ||
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
            payload.write_u32(record.action_instance_id);
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
                !reader.read_u32(&record.action_instance_id) ||
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

std::vector<std::uint8_t> encode_local_action_result_batch_packet(
    const LocalActionResultBatchPacket& packet,
    std::uint32_t sequence) {
    if (packet.records.size() > UINT16_MAX) {
        return {};
    }
    protocol_internal::PacketWriter payload;
    payload.reserve(
        kActionBatchHeaderPayloadSize +
        packet.records.size() * kLocalActionResultPayloadSize);
    payload.write_u32(packet.server_tick);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    payload.write_u16(0u);
    for (const KernelLocalActionResult& record : packet.records) {
        payload.write_u32(record.action_instance_id);
        payload.write_u16(record.confirmed_commit_count);
        payload.write_u8(record.result);
        payload.write_u8(record.reason);
        payload.write_u32(record.authoritative_tick);
    }
    return protocol_internal::wrap_packet(
        MessageType::kLocalActionResultBatch,
        payload.bytes(),
        sequence);
}

bool decode_local_action_result_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    LocalActionResultBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kLocalActionResultBatch,
            &payload,
            &payload_size) ||
        payload_size < kActionBatchHeaderPayloadSize) {
        return false;
    }
    LocalActionResultBatchPacket packet;
    std::uint16_t record_count = 0;
    std::uint16_t reserved = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) ||
        !reader.read_u16(&record_count) ||
        !reader.read_u16(&reserved) || reserved != 0u ||
        payload_size != kActionBatchHeaderPayloadSize +
                            static_cast<std::size_t>(record_count) *
                                kLocalActionResultPayloadSize) {
        return false;
    }
    packet.records.reserve(record_count);
    for (std::uint16_t index = 0; index < record_count; ++index) {
        KernelLocalActionResult record{};
        if (!reader.read_u32(&record.action_instance_id) ||
            !reader.read_u16(&record.confirmed_commit_count) ||
            !reader.read_u8(&record.result) ||
            !reader.read_u8(&record.reason) ||
            !reader.read_u32(&record.authoritative_tick) ||
            record.action_instance_id == 0u ||
            record.result > KernelLocalActionResultType_Rejected) {
            return false;
        }
        packet.records.push_back(record);
    }
    if (!reader.done()) {
        return false;
    }
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_remote_action_presentation_batch_packet(
    const RemoteActionPresentationBatchPacket& packet,
    std::uint32_t sequence) {
    if (packet.records.size() > UINT16_MAX) {
        return {};
    }
    protocol_internal::PacketWriter payload;
    payload.reserve(
        kActionBatchHeaderPayloadSize +
        packet.records.size() * kRemoteActionPresentationPayloadSize);
    payload.write_u32(packet.server_tick);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    payload.write_u16(0u);
    for (const KernelRemoteActionPresentationEvent& record : packet.records) {
        const bool is_status_event =
            record.event_type == KernelRemoteActionPresentationEventType_StatusApplied ||
            record.event_type == KernelRemoteActionPresentationEventType_StatusRemoved ||
            record.event_type == KernelRemoteActionPresentationEventType_StatusUpdated;
        if ((is_status_event &&
             (record.stack_count == 0u ||
              record.stack_count > kMaxActiveStatusEffects)) ||
            (!is_status_event && record.stack_count != 0u)) {
            return {};
        }
        payload.write_u32(record.actor_net_id);
        payload.write_u32(record.action_template_id);
        payload.write_u32(record.action_instance_id);
        payload.write_u16(record.first_commit_index);
        payload.write_u16(record.commit_count);
        payload.write_u8(record.event_type);
        payload.write_u8(record.flags);
        payload.write_u16(record.server_tick_delta);
        payload.write_u32(record.status_effect_id);
        payload.write_u32(record.status_instance_id);
        payload.write_u16(record.stack_count);
        payload.write_u16(0u);
    }
    return protocol_internal::wrap_packet(
        MessageType::kRemoteActionPresentationBatch,
        payload.bytes(),
        sequence);
}

bool decode_remote_action_presentation_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    RemoteActionPresentationBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kRemoteActionPresentationBatch,
            &payload,
            &payload_size) ||
        payload_size < kActionBatchHeaderPayloadSize) {
        return false;
    }
    RemoteActionPresentationBatchPacket packet;
    std::uint16_t record_count = 0;
    std::uint16_t reserved = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) ||
        !reader.read_u16(&record_count) ||
        !reader.read_u16(&reserved) || reserved != 0u ||
        payload_size != kActionBatchHeaderPayloadSize +
                            static_cast<std::size_t>(record_count) *
                                kRemoteActionPresentationPayloadSize) {
        return false;
    }
    packet.records.reserve(record_count);
    for (std::uint16_t index = 0; index < record_count; ++index) {
        KernelRemoteActionPresentationEvent record{};
        std::uint16_t reserved1 = 0u;
        if (!reader.read_u32(&record.actor_net_id) ||
            !reader.read_u32(&record.action_template_id) ||
            !reader.read_u32(&record.action_instance_id) ||
            !reader.read_u16(&record.first_commit_index) ||
            !reader.read_u16(&record.commit_count) ||
            !reader.read_u8(&record.event_type) ||
            !reader.read_u8(&record.flags) ||
            !reader.read_u16(&record.server_tick_delta) ||
            !reader.read_u32(&record.status_effect_id) ||
            !reader.read_u32(&record.status_instance_id) ||
            !reader.read_u16(&record.stack_count) ||
            !reader.read_u16(&reserved1) || reserved1 != 0u ||
            record.actor_net_id == 0u ||
            record.first_commit_index == 0u || record.commit_count == 0u ||
            static_cast<std::uint32_t>(record.first_commit_index) +
                    static_cast<std::uint32_t>(record.commit_count) - 1u >
                UINT16_MAX ||
            record.event_type >
                KernelRemoteActionPresentationEventType_StatusUpdated ||
            ((record.event_type == KernelRemoteActionPresentationEventType_StatusApplied ||
              record.event_type == KernelRemoteActionPresentationEventType_StatusRemoved ||
              record.event_type == KernelRemoteActionPresentationEventType_StatusUpdated) &&
             (record.action_template_id != 0u ||
              record.action_instance_id != 0u ||
              record.status_effect_id == 0u ||
              record.status_instance_id == 0u ||
              record.stack_count == 0u ||
              record.stack_count > kMaxActiveStatusEffects ||
              record.first_commit_index != 1u ||
              record.commit_count != 1u)) ||
            (record.event_type != KernelRemoteActionPresentationEventType_StatusApplied &&
             record.event_type != KernelRemoteActionPresentationEventType_StatusRemoved &&
             record.event_type != KernelRemoteActionPresentationEventType_StatusUpdated &&
             (record.action_instance_id == 0u ||
              record.status_effect_id != 0u ||
              record.status_instance_id != 0u ||
              record.stack_count != 0u ||
              record.status_channel_id != 0u ||
              record.duration_ticks != 0u))) {
            return false;
        }
        packet.records.push_back(record);
    }
    if (!reader.done()) {
        return false;
    }
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_status_effect_state_packet(
    const StatusEffectStatePacket& packet,
    std::uint32_t sequence) {
    if (packet.target_net_id == 0u || packet.revision == 0u ||
        packet.records.size() > kMaxActiveStatusEffects) {
        return {};
    }
    protocol_internal::PacketWriter payload;
    payload.reserve(
        kStatusEffectStateHeaderPayloadSize +
        packet.records.size() * kStatusEffectStateRecordPayloadSize);
    payload.write_u32(packet.server_tick);
    payload.write_u32(packet.target_net_id);
    payload.write_u32(packet.revision);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    payload.write_u16(0u);
    for (const StatusEffectStateRecord& record : packet.records) {
        if (record.status_effect_id == 0u || record.status_instance_id == 0u ||
            record.instigator_net_id == 0u ||
            record.stack_count == 0u ||
            record.stack_count > kMaxActiveStatusEffects ||
            record.expire_tick <= record.applied_tick) {
            return {};
        }
        payload.write_u32(record.status_effect_id);
        payload.write_u32(record.status_instance_id);
        payload.write_u32(record.instigator_net_id);
        payload.write_u32(record.applied_tick);
        payload.write_u32(record.expire_tick);
        payload.write_u16(record.stack_count);
        payload.write_u16(0u);
    }
    return protocol_internal::wrap_packet(
        MessageType::kStatusEffectState, payload.bytes(), sequence);
}

bool decode_status_effect_state_packet(
    const std::uint8_t* data,
    std::size_t size,
    StatusEffectStatePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kStatusEffectState,
            &payload,
            &payload_size) ||
        payload_size < kStatusEffectStateHeaderPayloadSize) {
        return false;
    }
    StatusEffectStatePacket packet;
    std::uint16_t record_count = 0;
    std::uint16_t reserved = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) ||
        !reader.read_u32(&packet.target_net_id) ||
        !reader.read_u32(&packet.revision) ||
        !reader.read_u16(&record_count) ||
        !reader.read_u16(&reserved) ||
        packet.target_net_id == 0u || packet.revision == 0u ||
        reserved != 0u || record_count > kMaxActiveStatusEffects ||
        payload_size != kStatusEffectStateHeaderPayloadSize +
                            static_cast<std::size_t>(record_count) *
                                kStatusEffectStateRecordPayloadSize) {
        return false;
    }
    packet.records.reserve(record_count);
    for (std::uint16_t index = 0u; index < record_count; ++index) {
        StatusEffectStateRecord record;
        std::uint16_t reserved_record = 0u;
        if (!reader.read_u32(&record.status_effect_id) ||
            !reader.read_u32(&record.status_instance_id) ||
            !reader.read_u32(&record.instigator_net_id) ||
            !reader.read_u32(&record.applied_tick) ||
            !reader.read_u32(&record.expire_tick) ||
            !reader.read_u16(&record.stack_count) ||
            !reader.read_u16(&reserved_record) || reserved_record != 0u ||
            record.status_effect_id == 0u || record.status_instance_id == 0u ||
            record.instigator_net_id == 0u ||
            record.stack_count == 0u ||
            record.stack_count > kMaxActiveStatusEffects ||
            record.expire_tick <= record.applied_tick ||
            std::any_of(
                packet.records.begin(),
                packet.records.end(),
                [&](const StatusEffectStateRecord& existing) {
                    return existing.status_instance_id ==
                        record.status_instance_id;
                })) {
            return false;
        }
        packet.records.push_back(record);
    }
    if (!reader.done()) {
        return false;
    }
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_gameplay_request_packet(
    const KernelGameplayRequest& request,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kGameplayRequestPayloadSize);
    payload.write_u32(request.requester_peer);
    payload.write_u64(request.request_id);
    payload.write_u32(request.instigator_net_id);
    payload.write_u8(request.domain_action);
    payload.write_u8(0u);
    payload.write_u16(0u);
    payload.write_u64(request.selected_item_instance_id);
    payload.write_u32(request.target_net_id);
    payload.write_u32(request.requested_quantity);
    payload.write_float(request.placement_position.x);
    payload.write_float(request.placement_position.y);
    payload.write_float(request.placement_position.z);
    payload.write_float(request.throw_direction.x);
    payload.write_float(request.throw_direction.y);
    payload.write_float(request.throw_direction.z);
    return protocol_internal::wrap_packet(
        MessageType::kGameplayRequest, payload.bytes(), sequence);
}

bool decode_gameplay_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelGameplayRequest* out_request) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_request == nullptr ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kGameplayRequest, &payload, &payload_size) ||
        payload_size != kGameplayRequestPayloadSize) {
        return false;
    }
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    std::uint8_t reserved0 = 0;
    std::uint16_t reserved1 = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&request.requester_peer) ||
        !reader.read_u64(&request.request_id) ||
        !reader.read_u32(&request.instigator_net_id) ||
        !reader.read_u8(&request.domain_action) ||
        !reader.read_u8(&reserved0) ||
        !reader.read_u16(&reserved1) ||
        !reader.read_u64(&request.selected_item_instance_id) ||
        !reader.read_u32(&request.target_net_id) ||
        !reader.read_u32(&request.requested_quantity) ||
        !reader.read_float(&request.placement_position.x) ||
        !reader.read_float(&request.placement_position.y) ||
        !reader.read_float(&request.placement_position.z) ||
        !reader.read_float(&request.throw_direction.x) ||
        !reader.read_float(&request.throw_direction.y) ||
        !reader.read_float(&request.throw_direction.z) ||
        reserved0 != 0u || reserved1 != 0u || !reader.done()) {
        return false;
    }
    *out_request = request;
    return true;
}

std::vector<std::uint8_t> encode_gameplay_request_outcome_packet(
    const KernelGameplayRequestOutcome& outcome,
    std::uint32_t sequence) {
    protocol_internal::PacketWriter payload;
    payload.reserve(kGameplayRequestOutcomePayloadSize);
    payload.write_u32(outcome.requester_peer);
    payload.write_u64(outcome.request_id);
    payload.write_u8(outcome.status);
    payload.write_u8(outcome.graph_outcome);
    payload.write_u8(outcome.domain_action);
    payload.write_u8(outcome.rejection_reason);
    payload.write_u64(outcome.item_instance_id);
    payload.write_u32(outcome.prop_entity_id);
    payload.write_u32(outcome.committed_quantity);
    return protocol_internal::wrap_packet(
        MessageType::kGameplayRequestOutcome, payload.bytes(), sequence);
}

bool decode_gameplay_request_outcome_packet(
    const std::uint8_t* data,
    std::size_t size,
    KernelGameplayRequestOutcome* out_outcome) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_outcome == nullptr ||
        !protocol_internal::unwrap_packet(
            data,
            size,
            MessageType::kGameplayRequestOutcome,
            &payload,
            &payload_size) ||
        payload_size != kGameplayRequestOutcomePayloadSize) {
        return false;
    }
    KernelGameplayRequestOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&outcome.requester_peer) ||
        !reader.read_u64(&outcome.request_id) ||
        !reader.read_u8(&outcome.status) ||
        !reader.read_u8(&outcome.graph_outcome) ||
        !reader.read_u8(&outcome.domain_action) ||
        !reader.read_u8(&outcome.rejection_reason) ||
        !reader.read_u64(&outcome.item_instance_id) ||
        !reader.read_u32(&outcome.prop_entity_id) ||
        !reader.read_u32(&outcome.committed_quantity) ||
        !reader.done()) {
        return false;
    }
    *out_outcome = outcome;
    return true;
}

std::vector<std::uint8_t> encode_inventory_delta_batch_packet(
    const InventoryDeltaBatchPacket& packet,
    std::uint32_t sequence) {
    if (packet.inventory_container_id == 0u || packet.first_revision == 0u ||
        packet.records.empty() || packet.records.size() > UINT16_MAX) {
        return {};
    }
    protocol_internal::PacketWriter payload;
    payload.write_u64(packet.inventory_container_id);
    payload.write_u64(packet.first_revision);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    for (const InventoryDeltaRecord& record : packet.records) {
        if (record.type > KernelInventoryDeltaType_Move ||
            record.changed_fields > kInventoryChangeAll ||
            ((record.type == KernelInventoryDeltaType_Add ||
              record.type == KernelInventoryDeltaType_Update) &&
             !valid_wire_item(record.item))) {
            return {};
        }
        payload.write_u8(static_cast<std::uint8_t>(record.type));
        payload.write_u16(record.slot);
        payload.write_u16(record.previous_slot);
        payload.write_u16(record.changed_fields);
        if (record.type == KernelInventoryDeltaType_Add) {
            write_wire_item(&payload, record.item, true, kInventoryChangeAll);
        } else if (record.type == KernelInventoryDeltaType_Update) {
            write_wire_item(
                &payload, record.item, false, record.changed_fields);
        } else {
            payload.write_u64(record.item.item_instance_id);
        }
    }
    if (payload.bytes().size() > kMaxInventoryPacketPayloadSize) return {};
    return protocol_internal::wrap_packet(
        MessageType::kInventoryDeltaBatch, payload.bytes(), sequence);
}

bool decode_inventory_delta_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventoryDeltaBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr || payload_size > kMaxInventoryPacketPayloadSize ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kInventoryDeltaBatch, &payload, &payload_size) ||
        payload_size < 18u || payload_size > kMaxInventoryPacketPayloadSize) {
        return false;
    }
    InventoryDeltaBatchPacket packet;
    std::uint16_t count = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u64(&packet.inventory_container_id) ||
        !reader.read_u64(&packet.first_revision) || !reader.read_u16(&count) ||
        packet.inventory_container_id == 0u || packet.first_revision == 0u ||
        count == 0u) {
        return false;
    }
    packet.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        InventoryDeltaRecord record;
        std::uint8_t type = 0;
        if (!reader.read_u8(&type) || !reader.read_u16(&record.slot) ||
            !reader.read_u16(&record.previous_slot) ||
            !reader.read_u16(&record.changed_fields) ||
            type > KernelInventoryDeltaType_Move ||
            record.changed_fields > kInventoryChangeAll) {
            return false;
        }
        record.type = static_cast<KernelInventoryDeltaType>(type);
        if (record.type == KernelInventoryDeltaType_Add) {
            if (!read_wire_item(
                    &reader, &record.item, true, kInventoryChangeAll) ||
                record.item.item_template_id == 0u || record.item.quantity == 0u) {
                return false;
            }
        } else if (record.type == KernelInventoryDeltaType_Update) {
            if (!read_wire_item(
                    &reader, &record.item, false, record.changed_fields)) {
                return false;
            }
        } else if (!reader.read_u64(&record.item.item_instance_id) ||
                   record.item.item_instance_id == 0u) {
            return false;
        }
        packet.records.push_back(std::move(record));
    }
    if (!reader.done()) return false;
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_inventory_snapshot_request_packet(
    const InventorySnapshotRequestPacket& packet,
    std::uint32_t sequence) {
    if (packet.inventory_container_id == 0u) return {};
    protocol_internal::PacketWriter payload;
    payload.write_u64(packet.inventory_container_id);
    payload.write_u64(packet.client_revision);
    return protocol_internal::wrap_packet(
        MessageType::kInventorySnapshotRequest, payload.bytes(), sequence);
}

bool decode_inventory_snapshot_request_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventorySnapshotRequestPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    InventorySnapshotRequestPacket packet;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kInventorySnapshotRequest,
            &payload, &payload_size) ||
        payload_size != kInventorySnapshotRequestPayloadSize) {
        return false;
    }
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u64(&packet.inventory_container_id) ||
        !reader.read_u64(&packet.client_revision) ||
        packet.inventory_container_id == 0u || !reader.done()) {
        return false;
    }
    *out_packet = packet;
    return true;
}

std::vector<std::uint8_t> encode_inventory_snapshot_page_packet(
    const InventorySnapshotPagePacket& packet,
    std::uint32_t sequence) {
    if (packet.inventory_container_id == 0u || packet.owner_entity_id == 0u ||
        packet.slot_capacity == 0u || packet.page_count == 0u ||
        packet.page_index >= packet.page_count || packet.entries.size() > UINT16_MAX) {
        return {};
    }
    protocol_internal::PacketWriter payload;
    payload.write_u64(packet.inventory_container_id);
    payload.write_u32(packet.owner_entity_id);
    payload.write_u64(packet.revision);
    payload.write_u32(packet.slot_capacity);
    payload.write_u16(packet.page_index);
    payload.write_u16(packet.page_count);
    payload.write_u16(static_cast<std::uint16_t>(packet.entries.size()));
    for (const InventorySnapshotEntry& entry : packet.entries) {
        if (entry.slot >= packet.slot_capacity || !valid_wire_item(entry.item)) {
            return {};
        }
        payload.write_u16(entry.slot);
        write_wire_item(&payload, entry.item, true, kInventoryChangeAll);
    }
    if (payload.bytes().size() > kMaxInventoryPacketPayloadSize) return {};
    return protocol_internal::wrap_packet(
        MessageType::kInventorySnapshotPage, payload.bytes(), sequence);
}

bool decode_inventory_snapshot_page_packet(
    const std::uint8_t* data,
    std::size_t size,
    InventorySnapshotPagePacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kInventorySnapshotPage,
            &payload, &payload_size) ||
        payload_size < 30u || payload_size > kMaxInventoryPacketPayloadSize) {
        return false;
    }
    InventorySnapshotPagePacket packet;
    std::uint16_t count = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u64(&packet.inventory_container_id) ||
        !reader.read_u32(&packet.owner_entity_id) ||
        !reader.read_u64(&packet.revision) ||
        !reader.read_u32(&packet.slot_capacity) ||
        !reader.read_u16(&packet.page_index) ||
        !reader.read_u16(&packet.page_count) || !reader.read_u16(&count) ||
        packet.inventory_container_id == 0u || packet.owner_entity_id == 0u ||
        packet.slot_capacity == 0u || packet.page_count == 0u ||
        packet.page_index >= packet.page_count) {
        return false;
    }
    packet.entries.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        InventorySnapshotEntry entry;
        if (!reader.read_u16(&entry.slot) || entry.slot >= packet.slot_capacity ||
            !read_wire_item(&reader, &entry.item, true, kInventoryChangeAll) ||
            entry.item.item_template_id == 0u || entry.item.quantity == 0u) {
            return false;
        }
        packet.entries.push_back(std::move(entry));
    }
    if (!reader.done()) return false;
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_locomotion_step_batch_packet(
    const LocomotionStepBatchPacket& packet,
    std::uint32_t sequence) {
    if (packet.records.empty() || packet.records.size() > UINT16_MAX) return {};
    protocol_internal::PacketWriter payload;
    payload.write_u32(packet.server_tick);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    for (const LocomotionStepRecord& record : packet.records) {
        if (record.net_id == 0u ||
            record.leg_index >= KERNEL_MAX_SKELETON_LEGS ||
            !std::isfinite(record.landing_target_world.x) ||
            !std::isfinite(record.landing_target_world.y) ||
            !std::isfinite(record.landing_target_world.z)) {
            return {};
        }
        payload.write_u32(record.net_id);
        payload.write_u8(record.leg_index);
        payload.write_u8(record.start_tick_delta);
        payload.write_float(record.landing_target_world.x);
        payload.write_float(record.landing_target_world.y);
        payload.write_float(record.landing_target_world.z);
    }
    return protocol_internal::wrap_packet(
        MessageType::kLocomotionStepBatch, payload.bytes(), sequence);
}

bool decode_locomotion_step_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    LocomotionStepBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kLocomotionStepBatch,
            &payload, &payload_size) || payload_size < 6u) {
        return false;
    }
    LocomotionStepBatchPacket packet;
    std::uint16_t count = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) || !reader.read_u16(&count) ||
        count == 0u) {
        return false;
    }
    packet.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        LocomotionStepRecord record;
        if (!reader.read_u32(&record.net_id) ||
            !reader.read_u8(&record.leg_index) ||
            !reader.read_u8(&record.start_tick_delta) ||
            !reader.read_float(&record.landing_target_world.x) ||
            !reader.read_float(&record.landing_target_world.y) ||
            !reader.read_float(&record.landing_target_world.z) ||
            record.net_id == 0u ||
            record.leg_index >= KERNEL_MAX_SKELETON_LEGS ||
            !std::isfinite(record.landing_target_world.x) ||
            !std::isfinite(record.landing_target_world.y) ||
            !std::isfinite(record.landing_target_world.z)) {
            return false;
        }
        packet.records.push_back(record);
    }
    if (!reader.done()) return false;
    *out_packet = std::move(packet);
    return true;
}

std::vector<std::uint8_t> encode_prop_state_change_batch_packet(
    const PropStateChangeBatchPacket& packet,
    std::uint32_t sequence) {
    if (packet.records.empty() || packet.records.size() > UINT16_MAX) return {};
    protocol_internal::PacketWriter payload;
    payload.write_u32(packet.server_tick);
    payload.write_u16(static_cast<std::uint16_t>(packet.records.size()));
    for (const PropStateChangeRecord& record : packet.records) {
        if (record.net_id == 0u || record.changed_fields == 0u ||
            (record.changed_fields & ~(kPropStateChangeMode |
                kPropStateChangeTransform | kPropStateChangeVelocity |
                kPropStateChangeHealth)) != 0u ||
            ((record.changed_fields & kPropStateChangeHealth) != 0u &&
             (record.max_hp == 0u || record.hp > record.max_hp))) {
            return {};
        }
        payload.write_u32(record.net_id);
        payload.write_u8(record.changed_fields);
        if ((record.changed_fields & kPropStateChangeMode) != 0u) {
            payload.write_u8(static_cast<std::uint8_t>(record.world_mode));
            payload.write_u32(record.carrier_entity_id);
        }
        if ((record.changed_fields & kPropStateChangeTransform) != 0u) {
            payload.write_float(record.position.x);
            payload.write_float(record.position.y);
            payload.write_float(record.position.z);
            payload.write_float(record.rotation.x);
            payload.write_float(record.rotation.y);
            payload.write_float(record.rotation.z);
            payload.write_float(record.rotation.w);
        }
        if ((record.changed_fields & kPropStateChangeVelocity) != 0u) {
            payload.write_float(record.velocity.x);
            payload.write_float(record.velocity.y);
            payload.write_float(record.velocity.z);
        }
        if ((record.changed_fields & kPropStateChangeHealth) != 0u) {
            payload.write_u16(record.hp);
            payload.write_u16(record.max_hp);
        }
    }
    return protocol_internal::wrap_packet(
        MessageType::kPropStateChangeBatch, payload.bytes(), sequence);
}

bool decode_prop_state_change_batch_packet(
    const std::uint8_t* data,
    std::size_t size,
    PropStateChangeBatchPacket* out_packet) {
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (out_packet == nullptr ||
        !protocol_internal::unwrap_packet(
            data, size, MessageType::kPropStateChangeBatch,
            &payload, &payload_size) || payload_size < 6u) {
        return false;
    }
    PropStateChangeBatchPacket packet;
    std::uint16_t count = 0;
    protocol_internal::PacketReader reader(payload, payload_size);
    if (!reader.read_u32(&packet.server_tick) || !reader.read_u16(&count) ||
        count == 0u) {
        return false;
    }
    packet.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        PropStateChangeRecord record;
        if (!reader.read_u32(&record.net_id) ||
            !reader.read_u8(&record.changed_fields) || record.net_id == 0u ||
            record.changed_fields == 0u ||
            (record.changed_fields & ~(kPropStateChangeMode |
                kPropStateChangeTransform | kPropStateChangeVelocity |
                kPropStateChangeHealth)) != 0u) {
            return false;
        }
        if ((record.changed_fields & kPropStateChangeMode) != 0u) {
            std::uint8_t mode = 0;
            if (!reader.read_u8(&mode) ||
                !reader.read_u32(&record.carrier_entity_id) ||
                mode > KernelWorldItemMode_InFlight) {
                return false;
            }
            record.world_mode = static_cast<KernelWorldItemMode>(mode);
        }
        if ((record.changed_fields & kPropStateChangeTransform) != 0u &&
            (!reader.read_float(&record.position.x) ||
             !reader.read_float(&record.position.y) ||
             !reader.read_float(&record.position.z) ||
             !reader.read_float(&record.rotation.x) ||
             !reader.read_float(&record.rotation.y) ||
             !reader.read_float(&record.rotation.z) ||
             !reader.read_float(&record.rotation.w))) {
            return false;
        }
        if ((record.changed_fields & kPropStateChangeVelocity) != 0u &&
            (!reader.read_float(&record.velocity.x) ||
             !reader.read_float(&record.velocity.y) ||
             !reader.read_float(&record.velocity.z))) {
            return false;
        }
        if ((record.changed_fields & kPropStateChangeHealth) != 0u &&
            (!reader.read_u16(&record.hp) ||
             !reader.read_u16(&record.max_hp) ||
             record.max_hp == 0u || record.hp > record.max_hp)) {
            return false;
        }
        packet.records.push_back(record);
    }
    if (!reader.done()) return false;
    *out_packet = std::move(packet);
    return true;
}

}  // namespace network_example
