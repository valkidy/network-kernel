#include "kernel/src/kernel.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "kernel/src/build_info.h"
#include "kernel/src/render_state_builder.h"
#include "protocol/public/network_packets.h"
#include "protocol/public/packet_header.h"
#include "protocol/public/session_packets.h"
#include "protocol/public/sha256.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/movement_solver.h"
#include "simulation/src/command_dispatcher.h"
#include "simulation/src/systems.h"
#include "simulation/src/item_gameplay_system.h"
#include "transport/public/gns_transport.h"
#include "transport/public/listen_server_transport.h"
#include "transport/public/network_simulator_transport.h"

namespace network_example {
namespace {

constexpr std::uint32_t kClientSnapshotMetadataGraceTicks = 2;
constexpr bool kDropStaleClientSnapshotsMissingMetadata = true;
constexpr std::uint64_t kLocalActionResultTimeoutUs = UINT64_C(1000000);
constexpr std::uint64_t kInputIntentTimeoutUs = UINT64_C(250000);
constexpr std::uint32_t kDefaultActionPacketBudgetBytes = 1200;
constexpr std::uint32_t kDefaultRemotePresentationExpiryMs = 250;
constexpr std::uint32_t kDefaultRemotePresentationClientBudgetBytesPerSecond = 8192;
constexpr std::uint32_t kDefaultRemotePresentationServerBudgetBytesPerSecond = 262144;
constexpr std::size_t kActionBatchFixedBytes = 36;
constexpr std::size_t kLocalActionResultRecordBytes = 12;
constexpr std::size_t kRemotePresentationRecordBytes = 32;
constexpr std::size_t kRemotePresentationDedupCapacity = 256;
constexpr std::size_t kRecentActionResultCapacity = 256;
constexpr std::size_t kMaxPendingClientActionIntents = 32;
constexpr float kPredictionCorrectionHalfLifeSeconds = 0.05f;
constexpr float kPredictionCorrectionEpsilonMeters = 0.001f;
constexpr float kPredictionCorrectionSnapDistanceMeters = 2.0f;
constexpr float kPredictionPresentationMinSpeedMetersPerSecond = 0.001f;
constexpr std::size_t kInventoryDeltaRecordsPerPacket = 64u;
constexpr std::size_t kInventorySnapshotEntriesPerPage = 128u;

std::uint32_t portable_state_word(
    const KernelPortableStateFieldDefinition& field) {
    if (field.type == KernelPortableStateType_Float) {
        return std::bit_cast<std::uint32_t>(field.float_default);
    }
    if (field.type == KernelPortableStateType_Bool) {
        return field.bool_default != 0u ? 1u : 0u;
    }
    return field.uint32_default;
}

InventoryWireItem inventory_wire_item(const KernelItemInstanceView& view) {
    InventoryWireItem item;
    item.item_instance_id = view.item_instance_id;
    item.item_template_id = view.item_template_id;
    item.quantity = view.quantity;
    item.next_use_tick = view.next_use_tick;
    item.portable_values.reserve(view.portable_state_field_count);
    for (std::uint32_t index = 0;
         index < view.portable_state_field_count;
         ++index) {
        item.portable_values.push_back(
            portable_state_word(view.portable_state_fields[index]));
    }
    return item;
}

bool inventory_view_from_wire(
    const ItemStore& store,
    const InventoryWireItem& wire,
    KernelInventoryContainerId container_id,
    std::uint16_t slot,
    KernelItemInstanceView* out_view) {
    if (out_view == nullptr || wire.item_instance_id == 0u ||
        wire.item_template_id == 0u || wire.quantity == 0u) {
        return false;
    }
    const KernelItemTemplateDefinition* item_template =
        store.find_template(wire.item_template_id);
    if (item_template == nullptr || wire.portable_values.size() !=
            item_template->portable_state_field_count) {
        return false;
    }
    KernelItemInstanceView view{};
    view.struct_size = sizeof(view);
    view.item_instance_id = wire.item_instance_id;
    view.item_template_id = wire.item_template_id;
    view.quantity = wire.quantity;
    view.residency = KernelItemResidency_Inventory;
    view.world_mode = KernelWorldItemMode_Placed;
    view.slot = slot;
    view.inventory_container_id = container_id;
    view.next_use_tick = wire.next_use_tick;
    view.portable_state_field_count =
        item_template->portable_state_field_count;
    for (std::uint32_t index = 0;
         index < item_template->portable_state_field_count;
         ++index) {
        view.portable_state_fields[index] =
            item_template->portable_state_fields[index];
        const std::uint32_t word = wire.portable_values[index];
        if (view.portable_state_fields[index].type ==
            KernelPortableStateType_Float) {
            view.portable_state_fields[index].float_default =
                std::bit_cast<float>(word);
        } else if (view.portable_state_fields[index].type ==
                   KernelPortableStateType_Bool) {
            view.portable_state_fields[index].bool_default = word != 0u;
        } else {
            view.portable_state_fields[index].uint32_default = word;
        }
    }
    *out_view = view;
    return true;
}

KernelConfig with_kernel_defaults(KernelConfig config) {
    config.tick = with_tick_defaults(config.tick);
    if (config.max_render_states == 0) {
        config.max_render_states = 2048;
    }
    if (config.max_events == 0) {
        config.max_events = 2048;
    }
    if (config.network_stats.mode == KernelNetworkStatsMode_Default ||
        config.network_stats.mode > KernelNetworkStatsMode_Detailed) {
        config.network_stats.mode = KernelNetworkStatsMode_Basic;
    }
    if (config.network_stats.action_packet_budget_bytes == 0) {
        config.network_stats.action_packet_budget_bytes =
            kDefaultActionPacketBudgetBytes;
    }
    if (config.network_stats.remote_presentation_expiry_ms == 0) {
        config.network_stats.remote_presentation_expiry_ms =
            kDefaultRemotePresentationExpiryMs;
    }
    if (config.network_stats.remote_presentation_client_budget_bytes_per_second == 0) {
        config.network_stats.remote_presentation_client_budget_bytes_per_second =
            kDefaultRemotePresentationClientBudgetBytesPerSecond;
    }
    if (config.network_stats.remote_presentation_server_budget_bytes_per_second == 0) {
        config.network_stats.remote_presentation_server_budget_bytes_per_second =
            kDefaultRemotePresentationServerBudgetBytesPerSecond;
    }
    return config;
}

KernelVec3 to_kernel_vec3(const glm::vec3& value) {
    return KernelVec3{value.x, value.y, value.z};
}

KernelQuat to_kernel_quat(const glm::quat& value) {
    return KernelQuat{value.x, value.y, value.z, value.w};
}

glm::vec3 from_kernel_vec3(const KernelVec3& value) {
    return glm::vec3{value.x, value.y, value.z};
}

glm::quat from_kernel_quat(const KernelQuat& value) {
    return glm::quat{value.w, value.x, value.y, value.z};
}

bool is_server_mode(KernelMode mode) {
    return mode == KernelMode_DedicatedServer || mode == KernelMode_ListenServer;
}

bool to_simulation_command_source(
    std::uint32_t command_source,
    simulation::CommandSource* out_source) {
    if (out_source == nullptr) {
        return false;
    }
    switch (command_source) {
        case KernelCommandSource_Internal:
            *out_source = simulation::CommandSource::kInternal;
            return true;
        case KernelCommandSource_PlayerInput:
            *out_source = simulation::CommandSource::kPlayerInput;
            return true;
        case KernelCommandSource_AI:
            *out_source = simulation::CommandSource::kAi;
            return true;
        case KernelCommandSource_ControlPlane:
            *out_source = simulation::CommandSource::kControlPlane;
            return true;
        case KernelCommandSource_Test:
            *out_source = simulation::CommandSource::kTest;
            return true;
        default:
            return false;
    }
}

bool is_valid_content_namespace(const char* value) {
    if (value == nullptr || value[0] == '\0' ||
        std::strcmp(value, ".") == 0 || std::strcmp(value, "..") == 0) {
        return false;
    }
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        if (!std::isalnum(character) && character != '-' && character != '_' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

bool is_valid_bundle_entry_path(const char* value) {
    if (value == nullptr || value[0] == '\0' || value[0] == '/' ||
        std::strchr(value, '\\') != nullptr) {
        return false;
    }
    const char* segment = value;
    for (const char* cursor = value;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') {
            continue;
        }
        const std::size_t segment_size = static_cast<std::size_t>(cursor - segment);
        if (segment_size == 0 ||
            (segment_size == 1 && segment[0] == '.') ||
            (segment_size == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        if (*cursor == '\0') {
            return true;
        }
        segment = cursor + 1;
    }
}

bool is_authoritative_combat_event(KernelEventType type) {
    return type == KernelEventType_FireConfirmed ||
           type == KernelEventType_HitConfirmed ||
           type == KernelEventType_DamageApplied ||
           type == KernelEventType_Explosion;
}

bool is_valid_agent_camp(std::uint8_t camp) {
    return camp <= KernelAgentCamp_Neutral;
}

std::uint8_t classify_agent_relation(
    NetId self_id,
    std::uint8_t self_camp,
    NetId candidate_id,
    std::uint8_t candidate_camp) {
    if (self_id == candidate_id) {
        return KernelAgentRelation_Self;
    }
    if (self_camp == KernelAgentCamp_Neutral ||
        candidate_camp == KernelAgentCamp_Neutral) {
        return KernelAgentRelation_Neutral;
    }
    if (self_camp == KernelAgentCamp_PlayerSide &&
        candidate_camp == KernelAgentCamp_EnemySide) {
        return KernelAgentRelation_Hostile;
    }
    if (self_camp == KernelAgentCamp_EnemySide &&
        candidate_camp == KernelAgentCamp_PlayerSide) {
        return KernelAgentRelation_Hostile;
    }
    if (self_camp != KernelAgentCamp_Unknown &&
        self_camp == candidate_camp) {
        return KernelAgentRelation_Ally;
    }
    return KernelAgentRelation_Unknown;
}

bool is_zero_vec3(const glm::vec3& value) {
    return glm::length(value) <= 0.0001f;
}

glm::vec3 normalized_or_forward(const glm::vec3& value) {
    if (is_zero_vec3(value)) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(value);
}

std::uint64_t elapsed_cost_us(
    std::chrono::steady_clock::time_point start_time) {
    const auto elapsed =
        std::chrono::steady_clock::now() - start_time;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (elapsed_ns <= 0) {
        return 0;
    }
    return std::max<std::uint64_t>(
        1,
        static_cast<std::uint64_t>(elapsed_ns) / 1000u);
}

constexpr PeerId kLocalListenPeerId = 1;
constexpr PeerId kServerPeerId = 0;
constexpr std::uint32_t kClientNonce = 0x4d330001u;
constexpr std::uint32_t kMaxCompensationWindowUs = 100000u;
constexpr std::uint64_t kClockSyncIntervalUs = 1000000u;
constexpr double kClientClockOffsetSmoothingFactor = 0.25;
constexpr float kMaxHomingVisualExtrapolationSeconds = 0.2f;
constexpr float kDefaultEntityRelevanceDistanceMeters = 40.0f;
// The radius an entity has to pass to STOP being relevant, as opposed to the
// one it has to pass to start. Leaving costs a reliable despawn and returning
// costs a reliable spawn plus a locomotion baseline, so a single threshold
// turns an entity idling on the boundary into a packet source: it flips on
// every snapshot its position happens to jitter across the line. The band has
// to outlast that jitter, not merely exceed it -- at a walking 2.5 m/s it is
// over a second of travel.
constexpr float kDefaultEntityRelevanceExitDistanceMeters = 44.0f;
constexpr float kDefaultProjectileRelevanceDistanceMeters = 80.0f;
// Slots go to whoever has waited longest, scaled by how much the receiving
// player is likely to notice. Without the weights every relevant agent gets the
// same share, which at 200 agents and 32 slots is 1.8 Hz each whether it is
// firing in the player's face or idling at the edge of the relevance sphere.
//
// The bands are deliberately coarse rather than a curve: what an entity's turn
// costs its neighbours should be obvious from reading the constants, and a
// discrete factor is something a test can pin.
// A teammate standing next to you is the most noticeable thing on screen after
// yourself, and one 35 m away is not. Players used to be written before the
// rotation ran at all, unconditionally, which spent budget on both alike. The
// bonus keeps a near teammate ahead of the agents around it while letting a
// distant one fall back to roughly what a near agent gets.
constexpr std::uint64_t kSnapshotPriorityPlayerWeight = 4;
constexpr float kSnapshotPriorityNearMeters = 10.0f;
constexpr float kSnapshotPriorityMidMeters = 25.0f;
constexpr std::uint64_t kSnapshotPriorityNearWeight = 4;
constexpr std::uint64_t kSnapshotPriorityMidWeight = 2;
constexpr std::uint64_t kSnapshotPriorityActingWeight = 2;
// An actor's snapshot record is the only thing that tells the client where it
// is. A projectile's is a correction on top of a reliable spawn that already
// carried its position and velocity, which the client dead-reckons between
// corrections -- so what a projectile needs is a prompt first delivery, not a
// high sustained rate, and `wait * weight` only buys the latter.
//
// Left neutral, the queue splits by population: measured at 192 relevant agents
// against 228 relevant projectiles it gave 13 agent slots to 12 projectile ones,
// taking agents from 30 a snapshot to 13. This is the multiplier that says an
// actor's turn is worth eight projectile turns.
constexpr std::uint64_t kSnapshotPriorityActorWeight = 8;
constexpr std::uint64_t kSnapshotPriorityProjectileWeight = 1;
// The backstop under the weights. A weighted queue serves in proportion to
// weight, so a crowd that is mostly high-weight can hold a low-weight entity off
// for far longer than the unweighted rotation ever did. Anything that has gone
// this many snapshots without a turn jumps ahead of the weighting entirely.
//
// At 15 snapshots per second this is a one second floor, and it holds as long as
// the budget can still carry the whole relevant population inside that window --
// past roughly 480 agents at the current record size there are not enough slots
// for any rule to promise it.
constexpr std::uint64_t kMaxSnapshotsWithoutSend = 15;
// What one flush of the locomotion step channel may cost. It is its own packet
// and the snapshot budget does not reach it, so until this existed the channel
// was bounded by nothing but how many legged rigs happened to be relevant --
// measured at 18 B a step and 2.7 steps a second a quadruped, 200 of them want
// about 680 B a flush and would simply take it.
//
// A third of the snapshot budget holds 20 records. Against the spread the
// benchmarks use that covers the near and mid bands of a 200-rig crowd and cuts
// the far tail, which is the trade this is for. It reaches the *average* demand
// at about 110 rigs, but steps do not arrive evenly -- measured at 64 rigs, an
// 11.5-a-flush average against a cap of 20 still trims 2% of steps off the burst
// peaks. Re-derive it from the locomotion table in agent_cpu_bench rather than
// adjusting it by feel.
constexpr std::size_t kLocomotionStepBudgetBytes = kSnapshotSendBudgetBytes / 3;
// Reorders a distance band from flush to flush. Without it a budget that cuts
// into a band cuts the same rigs every time, and their legs hold the pose the
// baseline planted while their bodies keep moving -- skating, which is worse
// than the lag a dropped step normally costs.
//
// A mixer rather than `net_id + tick`: adding the tick shifts every id by the
// same amount, which leaves the relative order intact everywhere except the
// wrap, so the rigs at the front stay at the front. This is deterministic and
// needs no per-session state -- the tick is the whole seed.
std::uint32_t locomotion_step_rotation(NetId net_id, std::uint32_t tick) {
    std::uint32_t value = net_id ^ (tick * 0x9E3779B9u);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    return value;
}
// How many entities one session may be introduced to in a single snapshot.
// Every introduction is a reliable entity spawn plus a locomotion baseline, and
// none of it is charged to the snapshot byte budget -- so a player who rounds a
// corner onto a crowd used to produce one reliable packet per crowd member, all
// in the same snapshot, however large the crowd was.
constexpr std::size_t kMaxEntitySpawnsPerSnapshot = 16;

KernelEntityLifecycleEventType lifecycle_type_for_despawn_reason(
    std::uint32_t reason) {
    if (reason == KernelDespawnReason_OutOfRange) {
        return KernelEntityLifecycleEventType_OutOfRange;
    }
    if (reason == KernelDespawnReason_Destroyed) {
        return KernelEntityLifecycleEventType_Destroyed;
    }
    return KernelEntityLifecycleEventType_Despawned;
}

const char* disconnect_reason_name(std::uint32_t reason_code) {
    switch (reason_code) {
        case kDisconnectReasonProtocolVersionMismatch:
            return "ProtocolVersionMismatch";
        case kDisconnectReasonSnapshotSchemaMismatch:
            return "SnapshotSchemaMismatch";
        case kDisconnectReasonPacketSchemaMismatch:
            return "PacketSchemaMismatch";
        case kDisconnectReasonCatalogMismatch:
            return "CatalogMismatch";
        default:
            return "Unknown";
    }
}

const char* channel_name(ChannelId channel) {
    switch (channel) {
        case ChannelId::kInput:
            return "Input";
        case ChannelId::kSnapshot:
            return "Snapshot";
        case ChannelId::kReliableEvent:
            return "ReliableEvent";
        case ChannelId::kSession:
            return "Session";
    }
    return "Unknown";
}

const char* send_mode_name(SendMode mode) {
    switch (mode) {
        case SendMode::kUnreliable:
            return "Unreliable";
        case SendMode::kReliable:
            return "Reliable";
    }
    return "Unknown";
}

std::uint32_t handshake_reject_reason(
    const HandshakePacket& handshake,
    std::uint32_t local_catalog_version,
    std::uint64_t local_catalog_hash) {
    if (handshake.protocol_version != kProtocolVersion) {
        return kDisconnectReasonProtocolVersionMismatch;
    }
    if (handshake.snapshot_schema_version != kSnapshotSchemaVersion) {
        return kDisconnectReasonSnapshotSchemaMismatch;
    }
    if (handshake.packet_schema_version != kPacketSchemaVersion) {
        return kDisconnectReasonPacketSchemaMismatch;
    }
    if (handshake.catalog_version != local_catalog_version ||
        handshake.catalog_hash != local_catalog_hash) {
        return kDisconnectReasonCatalogMismatch;
    }
    return 0;
}

void copy_handshake_text(char* destination, std::size_t size, const char* source) {
    if (destination == nullptr || size == 0) {
        return;
    }
    std::snprintf(destination, size, "%s", source == nullptr ? "unknown" : source);
}

HandshakePacket make_client_handshake(
    std::uint32_t catalog_version,
    std::uint64_t catalog_hash) {
    const KernelBuildInfo build_info = current_build_info();
    HandshakePacket handshake;
    handshake.client_nonce = kClientNonce;
    handshake.protocol_version = static_cast<std::uint16_t>(build_info.protocol_version);
    handshake.snapshot_schema_version =
        static_cast<std::uint16_t>(build_info.snapshot_schema_version);
    handshake.packet_schema_version =
        static_cast<std::uint16_t>(build_info.packet_schema_version);
    handshake.catalog_version = catalog_version;
    handshake.catalog_hash = catalog_hash;
    copy_handshake_text(
        handshake.module_version,
        sizeof(handshake.module_version),
        build_info.module_version);
    copy_handshake_text(
        handshake.git_commit,
        sizeof(handshake.git_commit),
        build_info.git_commit);
    return handshake;
}

void log_snapshot_decode_failure(const TransportEvent& transport_event) {
    PacketHeader header;
    const bool has_header = decode_packet_header(
        transport_event.payload.data(),
        transport_event.payload.size(),
        &header);
    const KernelBuildInfo build_info = current_build_info();
    spdlog::error(
        "[NetworkExample] decode_snapshot_packet failed error_code=6 "
        "local_module_version={} local_protocol_version={} "
        "local_snapshot_schema_version={} local_packet_schema_version={} "
        "local_git_commit={} packet_size={} packet_type={} peer_id={} channel={}",
        build_info.module_version,
        build_info.protocol_version,
        build_info.snapshot_schema_version,
        build_info.packet_schema_version,
        build_info.git_commit,
        transport_event.payload.size(),
        has_header ? header.message_type : 0,
        transport_event.peer,
        static_cast<int>(transport_event.channel));
}

std::uint32_t derived_visual_flags(const World& world, entt::entity entity) {
    std::uint32_t flags = 0;
    if (world.registry().all_of<Velocity>(entity) &&
        glm::length(world.registry().get<Velocity>(entity).linear) > 0.001f) {
        flags |= kVisualFlagMoving;
    }
    if (world.registry().all_of<WeaponState>(entity) &&
        world.registry().get<WeaponState>(entity).is_reloading) {
        flags |= kVisualFlagReloading;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0) {
        flags |= kVisualFlagDead;
    }
    if (world.registry().all_of<MovementState>(entity)) {
        const MovementState& movement =
            world.registry().get<MovementState>(entity);
        flags |= movement.ground_state == MovementState::GroundState::kGrounded
            ? kVisualFlagGrounded
            : kVisualFlagFalling;
        if (movement.landed_this_tick) {
            flags |= kVisualFlagLanded;
        }
    }
    return flags;
}

glm::vec3 input_aim_to_world(const KernelPlayerInput& input) {
    glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    if (glm::length(aim) <= 0.0001f) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(aim);
}

std::uint64_t tick_time_us(std::uint32_t tick, float fixed_delta_seconds) {
    return static_cast<std::uint64_t>(
        static_cast<double>(tick) * static_cast<double>(fixed_delta_seconds) *
        1000000.0);
}

std::uint64_t offset_time_us(std::uint64_t time_us, std::int64_t offset_us) {
    if (offset_us >= 0) {
        return time_us + static_cast<std::uint64_t>(offset_us);
    }
    const std::uint64_t absolute_offset =
        static_cast<std::uint64_t>(-offset_us);
    return time_us > absolute_offset ? time_us - absolute_offset : 0;
}

std::int64_t time_delta_us(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs >= rhs) {
        return static_cast<std::int64_t>(lhs - rhs);
    }
    return -static_cast<std::int64_t>(rhs - lhs);
}

std::uint32_t tick_for_time_us(
    std::uint64_t time_us,
    float fixed_delta_seconds) {
    const double tick =
        static_cast<double>(time_us) /
        (static_cast<double>(fixed_delta_seconds) * 1000000.0);
    return static_cast<std::uint32_t>(tick + 0.0001);
}

bool debug_filter_matches(
    const KernelDebugRecordFilter* filter,
    const KernelDebugInfo& record) {
    if (filter == nullptr) {
        return true;
    }
    if (filter->struct_size < sizeof(KernelDebugRecordFilter)) {
        return false;
    }
    if (filter->record_type_mask != 0 &&
        (filter->record_type_mask & record.record_type) == 0) {
        return false;
    }
    if (filter->min_tick != 0 || filter->max_tick != 0) {
        if (record.tick < filter->min_tick) {
            return false;
        }
        if (filter->max_tick != 0 && record.tick > filter->max_tick) {
            return false;
        }
    }
    if (record.record_type == KernelDebugRecordType_Projectile) {
        const KernelProjectileDebugInfo& projectile = record.data.projectile;
        if (filter->projectile_net_id != 0 &&
            filter->projectile_net_id != projectile.projectile_net_id) {
            return false;
        }
        if (filter->source_net_id != 0 &&
            filter->source_net_id != projectile.owner_net_id) {
            return false;
        }
        if (filter->weapon_id != KERNEL_DEBUG_WILDCARD_U8 &&
            filter->weapon_id != projectile.weapon_id) {
            return false;
        }
        if (filter->motion_model != KERNEL_DEBUG_WILDCARD_U8 &&
            filter->motion_model != projectile.motion_model) {
            return false;
        }
        if (filter->sync_mode != KERNEL_DEBUG_WILDCARD_U8 &&
            filter->sync_mode != projectile.sync_mode) {
            return false;
        }
    }
    if (record.record_type == KernelDebugRecordType_Hit) {
        const KernelHitDebugInfo& hit = record.data.hit;
        if (filter->source_net_id != 0 &&
            filter->source_net_id != hit.source_net_id) {
            return false;
        }
        if (filter->target_net_id != 0 &&
            filter->target_net_id != hit.target_net_id) {
            return false;
        }
        if (filter->weapon_id != KERNEL_DEBUG_WILDCARD_U8 &&
            filter->weapon_id != hit.weapon_id) {
            return false;
        }
    }
    return true;
}

const KernelColliderTemplateDefinition* find_collider_template(
    const std::vector<KernelColliderTemplateDefinition>& templates,
    std::uint32_t template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [template_id](const KernelColliderTemplateDefinition& collider_template) {
            return collider_template.template_id == template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

const KernelProjectileTemplateDefinition* find_projectile_template(
    const std::vector<KernelProjectileTemplateDefinition>& templates,
    std::uint32_t projectile_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [projectile_template_id](
            const KernelProjectileTemplateDefinition& projectile_template) {
            return projectile_template.projectile_template_id == projectile_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

bool valid_throw_trajectory(
    const std::vector<KernelProjectileTemplateDefinition>& templates,
    std::uint32_t projectile_template_id) {
    const KernelProjectileTemplateDefinition* projectile_template =
        find_projectile_template(templates, projectile_template_id);
    if (projectile_template == nullptr) return false;
    const KernelProjectileMechanicsDefinition& mechanics =
        projectile_template->mechanics;
    return mechanics.projectile_type == KernelProjectileType_Standard &&
        (mechanics.motion_model == KernelProjectileMotionModel_Linear ||
         mechanics.motion_model == KernelProjectileMotionModel_Parabolic) &&
        std::isfinite(mechanics.speed) && mechanics.speed > 0.0f &&
        std::isfinite(mechanics.gravity.x) &&
        std::isfinite(mechanics.gravity.y) &&
        std::isfinite(mechanics.gravity.z);
}

bool valid_throw_collider(
    const KernelColliderTemplateDefinition* collider_template) {
    return collider_template != nullptr &&
        (collider_template->purpose_flags & KernelColliderPurpose_Hit) != 0u &&
        (collider_template->shape_type == KernelColliderShapeType_Aabb ||
         collider_template->shape_type ==
             KernelColliderShapeType_OrientedBox ||
         collider_template->shape_type == KernelColliderShapeType_Sphere ||
         collider_template->shape_type == KernelColliderShapeType_Capsule);
}

const KernelActorTemplateDefinition* find_actor_template(
    const std::vector<KernelActorTemplateDefinition>& templates,
    std::uint32_t actor_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [actor_template_id](const KernelActorTemplateDefinition& actor_template) {
            return actor_template.actor_template_id == actor_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

const KernelEntityTemplateDefinition* find_entity_template(
    const std::vector<KernelEntityTemplateDefinition>& templates,
    std::uint32_t entity_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [entity_template_id](const KernelEntityTemplateDefinition& entity_template) {
            return entity_template.entity_template_id == entity_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

const RuntimeSkeletonAsset* find_skeleton_asset(
    const std::vector<RuntimeSkeletonAsset>& assets,
    std::uint32_t skeleton_asset_id) {
    const auto found = std::find_if(
        assets.begin(),
        assets.end(),
        [skeleton_asset_id](const RuntimeSkeletonAsset& asset) {
            return asset.skeleton_asset_id == skeleton_asset_id;
        });
    return found == assets.end() ? nullptr : &*found;
}

glm::vec3 collider_template_half_extents(
    const KernelColliderTemplateDefinition& collider_template) {
    return glm::vec3{
        collider_template.shape_params.x,
        collider_template.shape_params.y,
        collider_template.shape_params.z,
    };
}

float collider_template_radius(
    const KernelColliderTemplateDefinition& collider_template) {
    return collider_template.shape_type == KernelColliderShapeType_Capsule
        ? collider_template.shape_params.y
        : collider_template.shape_params.x;
}

float collider_template_cone_range(
    const KernelColliderTemplateDefinition& collider_template) {
    return collider_template.shape_params.x;
}

float collider_template_cone_fov_degrees(
    const KernelColliderTemplateDefinition& collider_template) {
    return collider_template.shape_params.y;
}

KernelVec4 collider_instance_shape_params(const ColliderInstance& collider) {
    if (collider.shape_type == ColliderShapeType::kSphere) {
        return KernelVec4{collider.radius, 0.0f, 0.0f, 0.0f};
    }
    if (collider.shape_type == ColliderShapeType::kSegment) {
        const float length =
            glm::length(collider.segment_end - collider.segment_start);
        return KernelVec4{length, collider.radius, 0.0f, 0.0f};
    }
    if (collider.shape_type == ColliderShapeType::kCapsule) {
        return KernelVec4{
            collider.capsule_half_height,
            collider.radius,
            0.0f,
            0.0f};
    }
    return KernelVec4{
        collider.half_extents.x,
        collider.half_extents.y,
        collider.half_extents.z,
        0.0f,
    };
}

std::vector<std::uint32_t> spawned_projectile_ids(
    const KernelActionTriggerDefinition& trigger) {
    std::vector<std::uint32_t> ids;
    if (trigger.action_count == 0u) {
        if (trigger.action_type ==
            KernelEntityTriggerActionType_SpawnProjectile) {
            ids.push_back(trigger.spawn_projectile_template_id);
        }
        return ids;
    }
    ids.reserve(trigger.action_count);
    for (std::uint32_t index = 0;
         index < trigger.action_count &&
         index < KERNEL_MAX_ACTION_GRAPH_ACTIONS;
         ++index) {
        if (trigger.actions[index].action_type ==
            KernelEntityTriggerActionType_SpawnProjectile) {
            ids.push_back(
                trigger.actions[index].spawn_projectile_template_id);
        }
    }
    return ids;
}

bool projectile_trigger_is_valid(
    const KernelActionTriggerDefinition& trigger,
    const std::vector<KernelProjectileTemplateDefinition>& templates) {
    if (trigger.struct_size == 0u) {
        return true;
    }
    if (trigger.struct_size < sizeof(KernelActionTriggerDefinition) ||
        trigger.action_count > KERNEL_MAX_ACTION_GRAPH_ACTIONS) {
        return false;
    }
    const std::vector<std::uint32_t> spawned_ids =
        spawned_projectile_ids(trigger);
    const std::uint32_t expected_count = trigger.action_count == 0u
        ? (trigger.action_type == KernelEntityTriggerActionType_None ? 0u : 1u)
        : trigger.action_count;
    // Only the spawn actions have to name a projectile. Requiring every action
    // to be one rejected the impulse-on-impact triggers an area effect authors
    // (rocket_explosion), even though simulate_projectiles implements exactly
    // that -- the shape of the trigger is checked by validate_projectile_
    // mechanics, and this function only owns the spawn references.
    for (std::uint32_t index = 0; index < expected_count; ++index) {
        const std::uint32_t condition_type = trigger.action_count == 0u
            ? trigger.condition_type
            : trigger.actions[index].condition_type;
        if (condition_type > KernelActionConditionType_EventHasTarget) {
            return false;
        }
    }
    return std::all_of(
        spawned_ids.begin(),
        spawned_ids.end(),
        [&](std::uint32_t id) {
            return id != 0u && find_projectile_template(templates, id) != nullptr;
        });
}

bool projectile_template_has_trigger_cycle(
    const std::vector<KernelProjectileTemplateDefinition>& templates,
    std::uint32_t projectile_template_id,
    std::unordered_set<std::uint32_t>* path) {
    if (projectile_template_id == 0u || path == nullptr) {
        return false;
    }
    if (!path->insert(projectile_template_id).second) {
        return true;
    }
    const KernelProjectileTemplateDefinition* projectile_template =
        find_projectile_template(templates, projectile_template_id);
    if (projectile_template != nullptr) {
        const KernelProjectileMechanicsDefinition& mechanics =
            projectile_template->mechanics;
        for (const KernelActionTriggerDefinition* trigger : {
                 &mechanics.projectile_impact_trigger,
                 &mechanics.expired_trigger,
             }) {
            for (const std::uint32_t next_id :
                 spawned_projectile_ids(*trigger)) {
                if (projectile_template_has_trigger_cycle(
                        templates, next_id, path)) {
                    return true;
                }
            }
        }
    }
    path->erase(projectile_template_id);
    return false;
}

bool projectile_template_has_trigger_cycle(
    const std::vector<KernelProjectileTemplateDefinition>& templates,
    std::uint32_t projectile_template_id) {
    std::unordered_set<std::uint32_t> path;
    return projectile_template_has_trigger_cycle(
        templates, projectile_template_id, &path);
}

ColliderShapeType to_collider_shape_type(std::uint8_t shape_type) {
    if (shape_type == KernelColliderShapeType_Sphere) {
        return ColliderShapeType::kSphere;
    }
    if (shape_type == KernelColliderShapeType_OrientedBox) {
        return ColliderShapeType::kOrientedBox;
    }
    if (shape_type == KernelColliderShapeType_Segment) {
        return ColliderShapeType::kSegment;
    }
    if (shape_type == KernelColliderShapeType_Cone) {
        return ColliderShapeType::kCone;
    }
    if (shape_type == KernelColliderShapeType_Capsule) {
        return ColliderShapeType::kCapsule;
    }
    return ColliderShapeType::kAabb;
}

std::uint8_t to_kernel_collider_shape_type(ColliderShapeType shape_type) {
    switch (shape_type) {
        case ColliderShapeType::kSphere:
            return KernelColliderShapeType_Sphere;
        case ColliderShapeType::kOrientedBox:
            return KernelColliderShapeType_OrientedBox;
        case ColliderShapeType::kSegment:
            return KernelColliderShapeType_Segment;
        case ColliderShapeType::kCone:
            return KernelColliderShapeType_Cone;
        case ColliderShapeType::kCapsule:
            return KernelColliderShapeType_Capsule;
        case ColliderShapeType::kAabb:
        default:
            return KernelColliderShapeType_Aabb;
    }
}

ColliderWorldBounds collider_world_bounds(const ColliderInstance& collider);

// The one rule that turns a solved bone frame into a collider, shared verbatim
// by the authoritative path and the client's follower path.
//
// It is deliberately a free function taking the root explicitly, rather than
// two methods reading two different transforms. The whole argument for spending
// zero snapshot bytes on limbs is that both sides derive them the same way from
// the same inputs; if the derivation lived twice, that would be a claim about
// two pieces of code staying in step rather than a property of one.
//
// The root to pass is the one the solve itself used and published --
// last_root_position and applied_root_rotation -- not the entity's current
// transform. They are equal today on both sides, but the published pair is what
// the feet were actually placed with, so it cannot drift out from under the
// colliders.
ColliderInstance make_limb_collider(
    NetId entity_net_id,
    EntityType entity_type,
    ActorType actor_type,
    const KernelSkeletonColliderDefinition& limb,
    const SolvedColliderPose& pose,
    std::span<const KernelBoneLocalTransform> bind_pose,
    const glm::vec3& root_position,
    const glm::quat& root_rotation) {
    // Half the bone's own rest scale, which is where the size lives; the solved
    // rotation had that scale divided out, so it has to come back in here or
    // the box would be a unit cube in the right place.
    const glm::vec3 half_extents =
        locomotion_collider_half_extents(bind_pose, limb);
    ColliderInstance collider{};
    collider.collider_template_id = 0u;
    collider.owner_net_id = entity_net_id;
    collider.entity_net_id = entity_net_id;
    collider.entity_type = entity_type;
    collider.actor_type = actor_type;
    collider.shape_type = to_collider_shape_type(limb.shape_type);
    collider.purpose_flags = limb.purpose_flags;
    collider.layer_mask = limb.layer_mask;
    collider.hit_zone = limb.hit_zone;
    collider.local_center = pose.local_position;
    collider.local_rotation = pose.local_rotation;
    collider.world_rotation = root_rotation * collider.local_rotation;
    collider.world_center =
        root_position + root_rotation * collider.local_center;
    collider.half_extents = half_extents;
    // A sphere limb is validated uniform at catalog load, so any axis is the
    // radius.
    collider.radius = half_extents.x;
    collider.world_bounds = collider_world_bounds(collider);
    return collider;
}

ColliderWorldBounds collider_world_bounds(const ColliderInstance& collider) {
    if (collider.shape_type == ColliderShapeType::kSegment) {
        const glm::vec3 min_corner =
            glm::min(collider.segment_start, collider.segment_end);
        const glm::vec3 max_corner =
            glm::max(collider.segment_start, collider.segment_end);
        return ColliderWorldBounds{
            (min_corner + max_corner) * 0.5f,
            (max_corner - min_corner) * 0.5f + glm::vec3{collider.radius},
        };
    }
    if (collider.shape_type == ColliderShapeType::kSphere) {
        return ColliderWorldBounds{
            collider.world_center,
            glm::vec3{std::max(0.0f, collider.radius)},
        };
    }
    if (collider.shape_type == ColliderShapeType::kCapsule) {
        return ColliderWorldBounds{
            collider.world_center,
            glm::vec3{
                collider.radius,
                collider.capsule_half_height + collider.radius,
                collider.radius},
        };
    }
    return ColliderWorldBounds{
        collider.world_center,
        glm::abs(collider.half_extents),
    };
}

// What a beam actually occupies. simulate_beams sweeps a sphere from the beam
// origin along the aim, so the shape that describes it is a segment carrying a
// radius -- a capsule -- not the oriented box the template authors it as. The
// box stays the authority on how long and how wide (apply_projectile_mechanics
// converts its half_extents into beam length/radius); it is simply not where
// the beam is. Placed as authored it would sit centred on the muzzle, half of
// it behind the shooter, and world-axis aligned, because a projectile's
// transform rotation is only written for beams and only by simulate_beams.
void apply_beam_collider_geometry(
    const ProjectileBeamRuntime& beam,
    ColliderInstance* collider) {
    collider->shape_type = ColliderShapeType::kSegment;
    collider->segment_start = beam.origin;
    collider->segment_end = beam.origin + beam.direction * beam.length;
    collider->radius = beam.radius;
    collider->half_extents = glm::vec3{0.0f, 0.0f, 0.0f};
    collider->local_center = glm::vec3{0.0f, 0.0f, 0.0f};
    collider->local_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    // Endpoints are world space, so the segment needs no orientation of its
    // own. Leaving a stale rotation here would not move it, but it would be a
    // lie for anything that reads world_rotation back.
    collider->world_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    collider->world_center =
        (collider->segment_start + collider->segment_end) * 0.5f;
}

const EntitySnapshot* find_snapshot_entity(
    const WorldSnapshot& snapshot,
    NetId net_id) {
    const auto found = std::find_if(
        snapshot.entities.begin(),
        snapshot.entities.end(),
        [net_id](const EntitySnapshot& entity) {
            return entity.net_id == net_id;
        });
    if (found == snapshot.entities.end()) {
        return nullptr;
    }
    return &(*found);
}

constexpr std::uint32_t kKernelServerEntityStateBaseSize =
    offsetof(KernelServerEntityState, active_weapon_slot);

KernelServerEntityState to_server_entity_state(
    const World& world,
    entt::entity entity,
    std::uint32_t current_tick) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(KernelServerEntityState);

    const NetworkIdentity& identity = world.registry().get<NetworkIdentity>(entity);
    const EntityKind& kind = world.registry().get<EntityKind>(entity);
    const Transform& transform = world.registry().get<Transform>(entity);
    state.net_id = identity.net_id;
    state.entity_type = static_cast<std::uint16_t>(kind.type);
    state.actor_type = static_cast<std::uint16_t>(kind.actor_type);
    if (world.registry().all_of<ActorTemplateRef>(entity)) {
        state.actor_template_id =
            world.registry().get<ActorTemplateRef>(entity).actor_template_id;
    }
    if (world.registry().all_of<ItemTemplateRef>(entity)) {
        state.item_template_id =
            world.registry().get<ItemTemplateRef>(entity).item_template_id;
    }
    if (world.registry().all_of<ItemInstanceRef>(entity)) {
        state.item_instance_id =
            world.registry().get<ItemInstanceRef>(entity).item_instance_id;
    }
    if (world.registry().all_of<PropWorldMode>(entity)) {
        state.world_item_mode = static_cast<std::uint8_t>(
            world.registry().get<PropWorldMode>(entity).mode);
    }
    if (world.registry().all_of<CarriedBy>(entity)) {
        state.carrier_entity_id =
            world.registry().get<CarriedBy>(entity).carrier_entity_id;
    }
    state.owner_peer = identity.owner_peer;
    state.position = to_kernel_vec3(transform.position);
    state.rotation = to_kernel_quat(transform.rotation);
    state.valid = 1u;

    if (world.registry().all_of<Velocity>(entity)) {
        state.velocity =
            to_kernel_vec3(world.registry().get<Velocity>(entity).linear);
    }
    if (world.registry().all_of<Health>(entity)) {
        const Health& health = world.registry().get<Health>(entity);
        state.hp = health.hp;
        state.max_hp = health.max_hp;
    }
    state.visual_flags = derived_visual_flags(world, entity);
    if (world.registry().all_of<ReplicationState>(entity)) {
        const ReplicationState& replication =
            world.registry().get<ReplicationState>(entity);
        state.animation_state = replication.animation_state;
        state.visual_flags |= replication.visual_flags;
    }
    state.visual_flags &= ~kVisualFlagFiring;
    if (world.registry().all_of<WeaponState>(entity)) {
        const WeaponState& weapon = world.registry().get<WeaponState>(entity);
        state.active_weapon_slot = weapon.active_weapon_slot;
        state.weapon_slot_count = weapon.weapon_slot_count;
        for (std::size_t slot = 0; slot < kWeaponSlotCount; ++slot) {
            state.weapon_ids[slot] = weapon.weapon_ids[slot];
            state.ammo[slot] = weapon.ammo[slot];
            state.reserve_magazines[slot] = weapon.reserve_magazines[slot];
        }
        state.is_reloading = weapon.is_reloading ? 1u : 0u;
        state.reload_remaining_ticks = 0u;
    }
    state.action.struct_size = sizeof(KernelActionRuntimeView);
    if (world.registry().all_of<ActionRuntimeState>(entity)) {
        const ActionRuntimeState& action =
            world.registry().get<ActionRuntimeState>(entity);
        state.action.action_template_id = action.action_template_id;
        state.action.action_instance_id = action.action_instance_id;
        state.action.phase = action.phase;
        state.action.start_tick = action.start_tick;
        state.action.commit_count = action.commit_count;
        if (action.binding_id == KernelActionBinding_PrimaryFire &&
            action.phase == KernelActionPhase_Active) {
            state.visual_flags |= kVisualFlagFiring;
        }
        if (action.binding_id == KernelActionBinding_Reload &&
            action.phase != KernelActionPhase_None &&
            action.next_commit_tick > current_tick) {
            state.reload_remaining_ticks = action.next_commit_tick - current_tick;
        }
    }
    state.aim_direction = KernelVec3{1.0f, 0.0f, 0.0f};
    if (world.registry().all_of<ActionInputState>(entity)) {
        state.aim_direction = to_kernel_vec3(
            world.registry().get<ActionInputState>(entity).aim_direction);
    }
    return state;
}

bool write_server_entity_state(
    const World& world,
    entt::entity entity,
    std::uint32_t current_tick,
    KernelServerEntityState* out_state) {
    if (out_state == nullptr ||
        out_state->struct_size < kKernelServerEntityStateBaseSize) {
        return false;
    }
    const std::uint32_t requested_size = out_state->struct_size;
    const KernelServerEntityState state =
        to_server_entity_state(world, entity, current_tick);
    std::memcpy(
        out_state,
        &state,
        std::min<std::uint32_t>(requested_size, sizeof(KernelServerEntityState)));
    return true;
}

std::uint32_t history_frame_count(const TickConfig& config) {
    const TickConfig tick = with_tick_defaults(config);
    const std::uint64_t history_tick_numerator =
        static_cast<std::uint64_t>(tick.server_tick_rate) * tick.history_ms;
    const std::uint64_t history_ticks =
        (history_tick_numerator + 999u) / 1000u;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, history_ticks));
}

std::uint32_t action_graph_dedup_retention_ticks(const TickConfig& config) {
    const std::uint64_t horizon = history_frame_count(config);
    const std::uint64_t retention = horizon + 1u;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        retention, std::numeric_limits<std::uint32_t>::max() / 2u));
}

ProjectileMotionModel to_projectile_motion_model(std::uint8_t motion_model) {
    if (motion_model == KernelProjectileMotionModel_Homing) {
        return ProjectileMotionModel::kHoming;
    }
    if (motion_model == KernelProjectileMotionModel_Parabolic) {
        return ProjectileMotionModel::kParabolic;
    }
    return ProjectileMotionModel::kLinear;
}

std::uint8_t to_kernel_projectile_motion_model(ProjectileMotionModel motion_model) {
    if (motion_model == ProjectileMotionModel::kHoming) {
        return KernelProjectileMotionModel_Homing;
    }
    if (motion_model == ProjectileMotionModel::kParabolic) {
        return KernelProjectileMotionModel_Parabolic;
    }
    return KernelProjectileMotionModel_Linear;
}

HomingMode to_homing_mode(std::uint8_t homing_mode) {
    return homing_mode == KernelHomingMode_FireAndForget
               ? HomingMode::kFireAndForget
               : HomingMode::kFireAndForget;
}

std::uint8_t to_kernel_homing_mode(HomingMode homing_mode) {
    switch (homing_mode) {
        case HomingMode::kFireAndForget:
        default:
            return KernelHomingMode_FireAndForget;
    }
}

ProjectileSyncMode to_projectile_sync_mode(std::uint8_t sync_mode) {
    if (sync_mode == KernelProjectileSyncMode_LocalPredictedDeterministic) {
        return ProjectileSyncMode::kLocalPredictedDeterministic;
    }
    if (sync_mode == KernelProjectileSyncMode_ServerSnapshotOnly) {
        return ProjectileSyncMode::kServerSnapshotOnly;
    }
    return ProjectileSyncMode::kHybridDeterministicThenSnapshot;
}

std::uint8_t to_kernel_projectile_sync_mode(ProjectileSyncMode sync_mode) {
    switch (sync_mode) {
        case ProjectileSyncMode::kLocalPredictedDeterministic:
            return KernelProjectileSyncMode_LocalPredictedDeterministic;
        case ProjectileSyncMode::kServerSnapshotOnly:
            return KernelProjectileSyncMode_ServerSnapshotOnly;
        case ProjectileSyncMode::kHybridDeterministicThenSnapshot:
        default:
            return KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    }
}

std::uint8_t to_kernel_guidance_phase(MissileGuidancePhase phase) {
    switch (phase) {
        case MissileGuidancePhase::kGuided:
            return KernelMissileGuidancePhase_Guided;
        case MissileGuidancePhase::kLostTarget:
            return KernelMissileGuidancePhase_LostTarget;
        case MissileGuidancePhase::kExpired:
            return KernelMissileGuidancePhase_Expired;
        case MissileGuidancePhase::kBoost:
        default:
            return KernelMissileGuidancePhase_Boost;
    }
}

ProjectileHitResponse to_projectile_hit_response(std::uint8_t hit_response) {
    if (hit_response == KernelProjectileHitResponse_Continue) {
        return ProjectileHitResponse::kContinue;
    }
    if (hit_response == KernelProjectileHitResponse_Bounce) {
        return ProjectileHitResponse::kBounce;
    }
    if (hit_response == KernelProjectileHitResponse_Attach) {
        return ProjectileHitResponse::kAttach;
    }
    return ProjectileHitResponse::kDestroy;
}

std::uint8_t to_kernel_projectile_hit_response(
    ProjectileHitResponse hit_response) {
    switch (hit_response) {
        case ProjectileHitResponse::kContinue:
            return KernelProjectileHitResponse_Continue;
        case ProjectileHitResponse::kBounce:
            return KernelProjectileHitResponse_Bounce;
        case ProjectileHitResponse::kAttach:
            return KernelProjectileHitResponse_Attach;
        case ProjectileHitResponse::kDestroy:
        default:
            return KernelProjectileHitResponse_Destroy;
    }
}

ProjectileDamageShape to_projectile_damage_shape(std::uint8_t damage_shape) {
    if (damage_shape == KernelProjectileDamageShape_None) {
        return ProjectileDamageShape::kNone;
    }
    if (damage_shape == KernelProjectileDamageShape_PiercingSegment) {
        return ProjectileDamageShape::kPiercingSegment;
    }
    return ProjectileDamageShape::kDirectHit;
}

ProjectileType to_projectile_type(std::uint8_t projectile_type) {
    if (projectile_type == KernelProjectileType_AreaEffect) {
        return ProjectileType::kAreaEffect;
    }
    if (projectile_type == KernelProjectileType_Beam) {
        return ProjectileType::kBeam;
    }
    return ProjectileType::kStandard;
}

std::uint8_t to_kernel_projectile_type(ProjectileType projectile_type) {
    switch (projectile_type) {
        case ProjectileType::kAreaEffect:
            return KernelProjectileType_AreaEffect;
        case ProjectileType::kBeam:
            return KernelProjectileType_Beam;
        case ProjectileType::kStandard:
        default:
            return KernelProjectileType_Standard;
    }
}

ProjectileDamageFalloff to_projectile_damage_falloff(std::uint8_t damage_falloff) {
    if (damage_falloff == KernelProjectileDamageFalloff_Linear) {
        return ProjectileDamageFalloff::kLinear;
    }
    return ProjectileDamageFalloff::kNone;
}

ProjectileCollisionQueryMode to_projectile_collision_query_mode(
    std::uint8_t collision_query_mode) {
    if (collision_query_mode == KernelProjectileCollisionQueryMode_Overlap) {
        return ProjectileCollisionQueryMode::kOverlap;
    }
    if (collision_query_mode == KernelProjectileCollisionQueryMode_Sweep) {
        return ProjectileCollisionQueryMode::kSweep;
    }
    if (collision_query_mode == KernelProjectileCollisionQueryMode_Ray) {
        return ProjectileCollisionQueryMode::kRay;
    }
    return ProjectileCollisionQueryMode::kAuto;
}

std::uint8_t to_kernel_projectile_damage_shape(
    ProjectileDamageShape damage_shape) {
    switch (damage_shape) {
        case ProjectileDamageShape::kNone:
            return KernelProjectileDamageShape_None;
        case ProjectileDamageShape::kPiercingSegment:
            return KernelProjectileDamageShape_PiercingSegment;
        case ProjectileDamageShape::kDirectHit:
        default:
            return KernelProjectileDamageShape_DirectHit;
    }
}

ProjectileCollisionGeometry projectile_collision_geometry_from_template(
    const KernelColliderTemplateDefinition& collider_template) {
    ProjectileCollisionGeometry geometry{};
    geometry.shape_type = to_collider_shape_type(collider_template.shape_type);
    geometry.center = from_kernel_vec3(collider_template.center);
    geometry.half_extents = collider_template_half_extents(collider_template);
    geometry.radius = collider_template_radius(collider_template);
    if (collider_template.shape_type == KernelColliderShapeType_Segment) {
        // A segment's thickness is the one shape value the query can use; its
        // reach comes from how far the projectile travelled, not from here.
        geometry.radius = collider_template.shape_params.y;
    }
    return geometry;
}

RuntimeProjectileTemplate to_runtime_projectile_template(
    const KernelProjectileTemplateDefinition& definition,
    const KernelColliderTemplateDefinition* collider_template) {
    RuntimeProjectileTemplate projectile_template{};
    const KernelProjectileMechanicsDefinition& mechanics = definition.mechanics;
    projectile_template.projectile_template_id = definition.projectile_template_id;
    projectile_template.weapon_id = definition.weapon_id;
    projectile_template.projectile_type =
        to_projectile_type(mechanics.projectile_type);
    projectile_template.motion_model =
        to_projectile_motion_model(mechanics.motion_model);
    projectile_template.sync_mode =
        to_projectile_sync_mode(mechanics.sync_mode);
    projectile_template.hit_response =
        to_projectile_hit_response(mechanics.hit_response);
    projectile_template.damage_shape =
        to_projectile_damage_shape(mechanics.damage_shape);
    projectile_template.damage_falloff =
        to_projectile_damage_falloff(mechanics.damage_falloff);
    projectile_template.collision_query_mode =
        to_projectile_collision_query_mode(mechanics.collision_query_mode);
    projectile_template.damage = mechanics.damage;
    projectile_template.speed = mechanics.speed;
    projectile_template.lifetime_ticks = mechanics.lifetime_ticks;
    projectile_template.gravity = from_kernel_vec3(mechanics.gravity);
    projectile_template.collider_template_id = mechanics.collider_template_id;
    if (collider_template != nullptr) {
        projectile_template.has_collision_geometry = true;
        projectile_template.collision_geometry =
            projectile_collision_geometry_from_template(*collider_template);
        projectile_template.area_radius =
            collider_template_radius(*collider_template);
        if (projectile_template.area_radius <= 0.0f) {
            const glm::vec3 half_extents =
                collider_template_half_extents(*collider_template);
            projectile_template.area_radius =
                std::max(half_extents.x, std::max(half_extents.y, half_extents.z));
        }
    }
    projectile_template.collision_mask = mechanics.collision_mask;
    projectile_template.max_hit_count = mechanics.max_hit_count;
    if (const auto binding = compile_action_trigger_definition(
            TriggerEventType::kProjectileImpact,
            mechanics.projectile_impact_trigger)) {
        projectile_template.projectile_impact_binding = std::move(*binding);
    }
    if (const auto binding = compile_action_trigger_definition(
            TriggerEventType::kExpired,
            mechanics.expired_trigger)) {
        projectile_template.expired_binding = std::move(*binding);
    }
    if (mechanics.area_effect.lifetime_ticks > 0u) {
        projectile_template.lifetime_ticks = mechanics.area_effect.lifetime_ticks;
    }
    projectile_template.damage_interval_ticks =
        mechanics.area_effect.damage_interval_ticks;
    if (mechanics.area_effect.radius > 0.0f) {
        projectile_template.area_radius = mechanics.area_effect.radius;
    }
    if (mechanics.area_effect.damage_per_interval > 0u) {
        projectile_template.damage = mechanics.area_effect.damage_per_interval;
    }
    if (mechanics.area_effect.collision_mask != 0u) {
        projectile_template.collision_mask = mechanics.area_effect.collision_mask;
    }
    projectile_template.area_hit_instigator =
        mechanics.area_effect.hit_instigator != 0u;
    projectile_template.area_motion_collision_mask =
        mechanics.area_effect.motion_collision_mask;
    projectile_template.beam_length = mechanics.beam.length;
    projectile_template.beam_radius = mechanics.beam.radius;
    if (projectile_template.has_collision_geometry &&
        (projectile_template.collision_geometry.shape_type == ColliderShapeType::kAabb ||
         projectile_template.collision_geometry.shape_type ==
             ColliderShapeType::kOrientedBox)) {
        projectile_template.beam_length =
            projectile_template.collision_geometry.half_extents.z * 2.0f;
        projectile_template.beam_radius = std::max(
            projectile_template.collision_geometry.half_extents.x,
            projectile_template.collision_geometry.half_extents.y);
    }
    if (mechanics.beam.damage_per_tick > 0u) {
        projectile_template.damage = mechanics.beam.damage_per_tick;
    }
    if (mechanics.beam.lifetime_ticks > 0u) {
        projectile_template.lifetime_ticks = mechanics.beam.lifetime_ticks;
    }
    if (mechanics.beam.collision_mask != 0u) {
        projectile_template.collision_mask = mechanics.beam.collision_mask;
    }
    projectile_template.homing_mode = to_homing_mode(mechanics.homing.homing_mode);
    projectile_template.homing_boost_ticks = mechanics.homing.boost_ticks;
    projectile_template.homing_lock_on_range = mechanics.homing.lock_on_range;
    projectile_template.homing_lose_target_range =
        mechanics.homing.lose_target_range;
    projectile_template.homing_lock_cone_degrees =
        mechanics.homing.lock_cone_degrees;
    projectile_template.homing_max_turn_degrees_per_tick =
        mechanics.homing.max_turn_degrees_per_tick;
    projectile_template.homing_acceleration = mechanics.homing.acceleration;
    projectile_template.homing_max_speed = mechanics.homing.max_speed;
    return projectile_template;
}

WeaponFireMode to_weapon_fire_mode(std::uint8_t fire_mode) {
    if (fire_mode == KernelWeaponFireMode_Shotgun) {
        return WeaponFireMode::kShotgun;
    }
    if (fire_mode == KernelWeaponFireMode_Projectile) {
        return WeaponFireMode::kProjectile;
    }
    return WeaponFireMode::kHitscan;
}

WeaponMechanicsDefinition to_weapon_mechanics(
    const KernelWeaponMechanicsDefinition& definition) {
    WeaponMechanicsDefinition mechanics{};
    mechanics.id = definition.weapon_id;
    mechanics.mode = to_weapon_fire_mode(definition.fire_mode);
    mechanics.magazine_size = definition.magazine_size;
    mechanics.reserve_magazines = definition.reserve_magazines;
    mechanics.damage = definition.damage;
    mechanics.max_range = definition.max_range;
    mechanics.pellet_count = definition.pellet_count;
    mechanics.pellet_spread = definition.pellet_spread;
    mechanics.segment_collider_template_id =
        definition.segment_collider_template_id;
    mechanics.projectile_template_id = definition.projectile_template_id;
    mechanics.fire_action_template_id = definition.fire_action_template_id;
    mechanics.reload_action_template_id = definition.reload_action_template_id;
    mechanics.collision_mask = definition.collision_mask;
    return mechanics;
}

KernelWeaponMechanicsDefinition to_kernel_weapon_mechanics(
    const WeaponMechanicsDefinition& mechanics) {
    KernelWeaponMechanicsDefinition definition{};
    definition.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    definition.weapon_id = mechanics.id;
    definition.fire_mode = static_cast<std::uint8_t>(mechanics.mode);
    definition.magazine_size = mechanics.magazine_size;
    definition.reserve_magazines = mechanics.reserve_magazines;
    definition.damage = mechanics.damage;
    definition.max_range = mechanics.max_range;
    definition.pellet_count = mechanics.pellet_count;
    definition.pellet_spread = mechanics.pellet_spread;
    definition.segment_collider_template_id =
        mechanics.segment_collider_template_id;
    definition.projectile_template_id = mechanics.projectile_template_id;
    definition.fire_action_template_id = mechanics.fire_action_template_id;
    definition.reload_action_template_id = mechanics.reload_action_template_id;
    definition.collision_mask = mechanics.collision_mask;
    return definition;
}

bool validate_action_template(
    const KernelActionTemplateDefinition& definition) {
    constexpr std::uint8_t kKnownFlags =
        KernelActionTemplateFlag_CancelOnRelease |
        KernelActionTemplateFlag_CancelOnDeath |
        KernelActionTemplateFlag_CancelOnWeaponChange |
        KernelActionTemplateFlag_CancelBeforeFirstCommit;
    if (definition.struct_size < sizeof(KernelActionTemplateDefinition) ||
        definition.action_template_id == 0u ||
        definition.trigger_mode > KernelActionTriggerMode_Hold ||
        (definition.flags & ~kKnownFlags) != 0u ||
        (definition.max_commit_count != 1u &&
         definition.commit_interval_ticks == 0u)) {
        return false;
    }
    if (definition.trigger_mode == KernelActionTriggerMode_Press) {
        return definition.max_commit_count >= 1u &&
               definition.hold_input_timeout_ticks == 0u;
    }
    return definition.hold_input_timeout_ticks > 0u;
}

const KernelActionTemplateDefinition* find_action_template(
    const std::vector<KernelActionTemplateDefinition>& templates,
    std::uint32_t action_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [action_template_id](const KernelActionTemplateDefinition& definition) {
            return definition.action_template_id == action_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

bool validate_homing_mechanics(const KernelHomingMechanicsDefinition& homing) {
    return homing.struct_size >= sizeof(KernelHomingMechanicsDefinition) &&
           homing.homing_mode == KernelHomingMode_FireAndForget &&
           homing.sync_mode == KernelProjectileSyncMode_HybridDeterministicThenSnapshot &&
           homing.lock_on_range > 0.0f &&
           homing.lose_target_range >= homing.lock_on_range &&
           homing.lock_cone_degrees > 0.0f &&
           homing.lock_cone_degrees <= 180.0f &&
           homing.max_turn_degrees_per_tick > 0.0f &&
           homing.acceleration > 0.0f &&
           homing.max_speed > 0.0f;
}

bool validate_area_effect_mechanics(
    const KernelAreaEffectMechanicsDefinition& area_effect) {
    return area_effect.struct_size >= sizeof(KernelAreaEffectMechanicsDefinition) &&
           area_effect.radius > 0.0f &&
           area_effect.damage_per_interval > 0 &&
           area_effect.damage_interval_ticks > 0 &&
           area_effect.lifetime_ticks > 0 &&
           (area_effect.collision_mask &
            ~(KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP)) == 0u &&
           area_effect.hit_instigator <= 1u &&
           // Only the static world can stop a travelling field. Actors and
           // props are what it affects, which collision_mask above already
           // says, and letting those bits in here would read as a second,
           // contradictory answer to that question.
           (area_effect.motion_collision_mask &
            ~KERNEL_COLLISION_MASK_STATIC_WORLD) == 0u;
}

bool validate_beam_mechanics(const KernelBeamMechanicsDefinition& beam) {
    return beam.struct_size >= sizeof(KernelBeamMechanicsDefinition) &&
           beam.length > 0.0f &&
           beam.radius > 0.0f &&
           beam.damage_per_tick > 0 &&
           beam.lifetime_ticks > 0 &&
           (beam.collision_mask &
            ~(KERNEL_COLLISION_MASK_ACTOR |
              KERNEL_COLLISION_MASK_STATIC_WORLD |
              KERNEL_COLLISION_LAYER_LIMB)) == 0u;
}

bool validate_projectile_mechanics(
    const KernelProjectileMechanicsDefinition& mechanics) {
    const auto valid_trigger = [](const KernelActionTriggerDefinition& trigger) {
        if (trigger.struct_size == 0u) return true;
        if (trigger.struct_size < sizeof(KernelActionTriggerDefinition)) return false;
        const std::uint32_t count = trigger.action_count == 0u ? 1u : trigger.action_count;
        if (count > KERNEL_MAX_ACTION_GRAPH_ACTIONS) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            KernelActionDefinition action{};
            if (trigger.action_count == 0u) {
                action.action_type = trigger.action_type;
                action.target_source = trigger.target_source;
                action.damage_amount = trigger.damage_amount;
                action.spawn_entity_template_id = trigger.spawn_entity_template_id;
                action.spawn_projectile_template_id = trigger.spawn_projectile_template_id;
                action.position_source = trigger.position_source;
                action.direction_source = trigger.direction_source;
                action.owner_source = trigger.owner_source;
                action.spawn_item_template_id = trigger.spawn_item_template_id;
                action.spawn_item_quantity = trigger.spawn_item_quantity;
                action.health_change_amount = trigger.health_change_amount;
                action.condition_type = trigger.condition_type;
                action.impulse_strength = trigger.impulse_strength;
                action.impulse_collision_mask = trigger.impulse_collision_mask;
                action.impulse_direction = trigger.impulse_direction;
                action.impulse_lockout_ticks = trigger.impulse_lockout_ticks;
                action.impulse_strength_mode = trigger.impulse_strength_mode;
                action.impulse_strength_vertical =
                    trigger.impulse_strength_vertical;
            } else {
                action = trigger.actions[index];
            }
            if (action.action_type == KernelEntityTriggerActionType_SpawnProjectile) {
                if (action.spawn_projectile_template_id == 0u ||
                    action.position_source != KernelEventVec3Source_Position ||
                    action.direction_source != KernelEventVec3Source_Direction) {
                    return false;
                }
            } else if (action.action_type == KernelEntityTriggerActionType_ApplyDamage) {
                if (action.target_source > KernelEntityRefSource_EventInstigator ||
                    action.damage_amount == 0u) return false;
            } else if (action.action_type == KernelEntityTriggerActionType_ApplyImpulse) {
                if (action.target_source > KernelEntityRefSource_EventInstigator ||
                    !impulse_strength_is_authorable(
                        action.impulse_strength_mode,
                        action.impulse_strength,
                        action.impulse_strength_vertical) ||
                    (action.direction_source != KernelEventVec3Source_Direction &&
                     action.direction_source !=
                         KernelEventVec3Source_SubjectDirection &&
                     action.direction_source != KernelEventVec3Source_Literal) ||
                    (action.direction_source == KernelEventVec3Source_Literal &&
                     (!std::isfinite(action.impulse_direction.x) ||
                      !std::isfinite(action.impulse_direction.y) ||
                      !std::isfinite(action.impulse_direction.z) ||
                      (action.impulse_direction.x == 0.0f &&
                       action.impulse_direction.y == 0.0f &&
                       action.impulse_direction.z == 0.0f))) ||
                    (action.impulse_collision_mask &
                     (KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP)) == 0u ||
                    (action.impulse_collision_mask &
                     ~(KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP)) != 0u ||
                    action.impulse_lockout_ticks >
                        KERNEL_MAX_IMPULSE_LOCKOUT_TICKS) {
                    return false;
                }
            } else {
                return false;
            }
        }
        return true;
    };
    if (mechanics.struct_size < sizeof(KernelProjectileMechanicsDefinition) ||
        mechanics.projectile_type > KernelProjectileType_Beam ||
        mechanics.motion_model > KernelProjectileMotionModel_Homing ||
        mechanics.sync_mode > KernelProjectileSyncMode_ServerSnapshotOnly ||
        mechanics.hit_response > KernelProjectileHitResponse_Attach ||
        mechanics.damage_shape > KernelProjectileDamageShape_PiercingSegment ||
        mechanics.damage_falloff > KernelProjectileDamageFalloff_Linear ||
        mechanics.collision_query_mode > KernelProjectileCollisionQueryMode_Ray ||
        mechanics.hit_response == KernelProjectileHitResponse_Bounce ||
        mechanics.hit_response == KernelProjectileHitResponse_Attach ||
        (mechanics.damage_shape == KernelProjectileDamageShape_None
             ? mechanics.damage != 0
             : mechanics.damage == 0) ||
        mechanics.collider_template_id == 0 ||
        mechanics.max_hit_count == 0 ||
        !valid_trigger(mechanics.projectile_impact_trigger) ||
        !valid_trigger(mechanics.expired_trigger)) {
        return false;
    }
    // Kept in step with the same sets in gameplay_config.cc, which is where
    // authoring is checked; this is where a catalog built anywhere else is.
    const std::uint32_t supported_collision_mask =
        KERNEL_COLLISION_LAYER_LIMB |
        (mechanics.projectile_type == KernelProjectileType_AreaEffect
            ? KERNEL_COLLISION_MASK_ACTOR | KERNEL_COLLISION_MASK_PROP
            : mechanics.projectile_type == KernelProjectileType_Beam
                ? KERNEL_COLLISION_MASK_ACTOR |
                    KERNEL_COLLISION_MASK_STATIC_WORLD
                : KERNEL_COLLISION_MASK_ACTOR |
                    KERNEL_COLLISION_MASK_STATIC_WORLD |
                    KERNEL_COLLISION_LAYER_PROJECTILE);
    if ((mechanics.collision_mask & ~supported_collision_mask) != 0u) {
        return false;
    }
    if (mechanics.projectile_type == KernelProjectileType_Standard &&
        (mechanics.speed <= 0.0f || mechanics.lifetime_ticks == 0u)) {
        return false;
    }
    if (mechanics.motion_model == KernelProjectileMotionModel_Homing) {
        if (mechanics.projectile_type != KernelProjectileType_Standard ||
            !validate_homing_mechanics(mechanics.homing)) {
            return false;
        }
    } else if (mechanics.homing.struct_size != 0) {
        return false;
    }
    if (mechanics.projectile_type == KernelProjectileType_AreaEffect) {
        if (!validate_area_effect_mechanics(mechanics.area_effect)) {
            return false;
        }
    } else if (mechanics.area_effect.struct_size != 0) {
        return false;
    }
    if (mechanics.projectile_type == KernelProjectileType_Beam) {
        if (!validate_beam_mechanics(mechanics.beam)) {
            return false;
        }
    } else if (mechanics.beam.struct_size != 0) {
        return false;
    }
    return true;
}

bool validate_weapon_mechanics(const KernelWeaponMechanicsDefinition& definition) {
    if (definition.struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
        definition.magazine_size == 0 ||
        (definition.fire_mode != KernelWeaponFireMode_Projectile &&
         definition.damage == 0) ||
        definition.fire_action_template_id == 0u ||
        definition.reload_action_template_id == 0u) {
        return false;
    }
    if (definition.fire_mode > KernelWeaponFireMode_Projectile) {
        return false;
    }
    if (definition.fire_mode == KernelWeaponFireMode_Projectile) {
        return definition.projectile_template_id != 0;
    }
    if (definition.max_range <= 0.0f) {
        return false;
    }
    if ((definition.fire_mode == KernelWeaponFireMode_Hitscan ||
         definition.fire_mode == KernelWeaponFireMode_Shotgun) &&
        definition.segment_collider_template_id == 0) {
        return false;
    }
    if (definition.fire_mode == KernelWeaponFireMode_Shotgun &&
        definition.pellet_count == 0) {
        return false;
    }
    return true;
}

const WeaponMechanicsDefinition* entity_weapon_mechanics(
    const World& world,
    NetId net_id,
    std::uint8_t weapon_id) {
    const std::optional<entt::entity> entity = world.find_entity(net_id);
    if (!entity.has_value() ||
        !world.registry().all_of<WeaponTuning>(*entity)) {
        return nullptr;
    }
    const WeaponTuning& tuning = world.registry().get<WeaponTuning>(*entity);
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    return tuning.configured[index] ? &tuning.definitions[index] : nullptr;
}

// movement.collision_mask is symmetric, and this is the half that makes it so.
// The mask filters the owner's own sweeps (see movement_filter in
// player_movement.cc), but a query filter only ever says what stops *me*; it
// says nothing about what I stop. Registering every movement capsule on the
// actor layer regardless left the pair inconsistent: a legged actor authored
// terrain-only walked through a player while the player, sweeping the engine
// default, still walked into the 3 m x 16 m capsule carrying the belly -- the
// exact wall the mask existed to remove.
//
// So an actor that does not name the actor layer is not on it either: its
// movement capsule is left out of the physics world entirely. Nothing else
// queries CollisionObjectKind::kActorMovement, and the actor's damage hitbox and
// vision cone are separate colliders, so hits and perception are unaffected.
// Zero is still "engine default", which includes actors.
bool movement_capsule_blocks_other_actors(std::uint32_t movement_collision_mask) {
    return movement_collision_mask == 0u ||
        (movement_collision_mask & KERNEL_MOVEMENT_LAYER_ACTOR) != 0u;
}

}  // namespace

KernelEngine::KernelEngine(KernelConfig config)
    : config_(with_kernel_defaults(config)),
      tick_loop_(config_.tick),
      history_buffer_(history_frame_count(config_.tick)),
      transport_(std::make_unique<NetworkSimulatorTransport>()),
      rpc_dispatcher_(&rpc_method_registry_, &rpc_response_store_) {
    world_.set_action_graph_dedup_retention_ticks(
        action_graph_dedup_retention_ticks(config_.tick));
    render_states_.reserve(config_.max_render_states);
    events_.reserve(config_.max_events);
}

bool KernelEngine::set_physics_config(const KernelPhysicsConfig& config) {
    if (running_ || config.physics_simulation > 1) {
        return false;
    }
    physics_config_ = config;
    physics_config_.struct_size = sizeof(KernelPhysicsConfig);
    return true;
}

bool KernelEngine::set_session_rules(const KernelSessionRulesConfig& config) {
    if (running_ || !is_server_mode(config_.mode) ||
        config.actor_blocking_mode > KernelActorBlockingMode_Predicted) {
        return false;
    }
    session_rules_ = config;
    session_rules_.struct_size = sizeof(KernelSessionRulesConfig);
    return true;
}

bool KernelEngine::set_static_collision_scene(
    const KernelStaticCollisionSceneConfig& config) {
    std::vector<std::uint8_t> scene;
    if (!prepare_static_collision_scene(config, &scene)) {
        return false;
    }
    commit_static_collision_scene(std::move(scene), config);
    return true;
}

bool KernelEngine::prepare_static_collision_scene(
    const KernelStaticCollisionSceneConfig& config,
    std::vector<std::uint8_t>* out_scene) const {
    const bool client_catalog_registration =
        config_.mode == KernelMode_Client && running_ && !has_welcome_;
    if (!client_catalog_registration &&
        (running_ || !is_server_mode(config_.mode))) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: invalid lifecycle "
            "mode={} running={} has_welcome={}; server registration must "
            "occur before start and client registration before Welcome",
            static_cast<std::uint32_t>(config_.mode),
            running_,
            has_welcome_);
        return false;
    }
    if (config.artifact_bytes == nullptr) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: artifact_bytes is null");
        return false;
    }
    if (config.artifact_size == 0) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: artifact_size is zero");
        return false;
    }
    if (config.artifact_size > KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: artifact_size={} "
            "exceeds limit={}",
            config.artifact_size,
            KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES);
        return false;
    }
    if (config.scene_id == 0) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: scene_id must be non-zero");
        return false;
    }
    if (config.collider_id == 0) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: collider_id must be non-zero");
        return false;
    }
    if (config.collision_layer != KERNEL_STATIC_COLLISION_LAYER_TERRAIN) {
        spdlog::error(
            "Kernel_SetStaticCollisionScene rejected: collision_layer={} "
            "expected={}",
            config.collision_layer,
            KERNEL_STATIC_COLLISION_LAYER_TERRAIN);
        return false;
    }
    out_scene->assign(
        config.artifact_bytes,
        config.artifact_bytes + config.artifact_size);
    return true;
}

void KernelEngine::commit_static_collision_scene(
    std::vector<std::uint8_t> scene,
    const KernelStaticCollisionSceneConfig& config) {
    static_collision_scene_ = std::move(scene);
    static_collision_scene_id_ = config.scene_id;
    static_collision_collider_id_ = config.collider_id;
    static_collision_layer_ = config.collision_layer;
}

bool KernelEngine::prepare_prediction_physics() {
    if (static_collision_scene_.empty()) {
        spdlog::error(
            "client prediction physics requires a verified static collision scene");
        return false;
    }
    auto world = std::make_unique<physics::PhysicsWorld>(
        physics::PhysicsWorldConfig{physics_config_.physics_workers});
    if (!world->valid()) {
        spdlog::error("failed to initialize client prediction physics world");
        return false;
    }
    physics::CollisionObjectIdentity identity{};
    identity.collider_id = static_collision_collider_id_;
    identity.kind = physics::CollisionObjectKind::kTerrain;
    identity.layer = physics::CollisionLayer::kTerrain;
    identity.scene_id = static_collision_scene_id_;
    std::string error;
    if (!world->load_static_scene(static_collision_scene_, identity, &error)) {
        spdlog::error(
            "failed to load client prediction static collision scene: {}", error);
        return false;
    }
    prediction_physics_world_ = std::move(world);
    prediction_proxy_collider_ids_.clear();
    prediction_obstacle_collider_ids_.clear();
    prediction_limb_collider_ids_.clear();
    next_prediction_proxy_collider_id_ = 0xc0000000u;
    return true;
}

bool KernelEngine::prepare_server_physics(
    std::unique_ptr<physics::PhysicsWorld>* out_world) {
    if (out_world == nullptr || running_) {
        return false;
    }
    if (physics_config_.physics_simulation == 1) {
        spdlog::error(
            "physics_simulation=1 is reserved and not implemented; "
            "use query-only physics_simulation=0");
        return false;
    }
    auto world = std::make_unique<physics::PhysicsWorld>(
        physics::PhysicsWorldConfig{physics_config_.physics_workers});
    if (!world->valid()) {
        spdlog::error("failed to initialize query-only Jolt physics world");
        return false;
    }
    if (!static_collision_scene_.empty()) {
        physics::CollisionObjectIdentity identity{};
        identity.collider_id = static_collision_collider_id_;
        identity.kind = physics::CollisionObjectKind::kTerrain;
        identity.layer = physics::CollisionLayer::kTerrain;
        identity.scene_id = static_collision_scene_id_;
        std::string error;
        if (!world->load_static_scene(
                static_collision_scene_, identity, &error)) {
            spdlog::error("failed to load static collision scene: {}", error);
            return false;
        }
    }
    *out_world = std::move(world);
    return true;
}

bool KernelEngine::invoke_rpc(
    std::string_view request_json,
    std::uint64_t* out_request_id) {
    return rpc_dispatcher_.invoke(
        *this,
        request_json,
        KernelRpcAuthority::kDeveloperWrite,
        out_request_id);
}

bool KernelEngine::poll_rpc_response(
    std::uint64_t request_id,
    char* out_response_json,
    std::uint32_t response_json_capacity,
    std::uint32_t* out_response_json_size) {
    return rpc_dispatcher_.poll(
        request_id,
        out_response_json,
        response_json_capacity,
        out_response_json_size);
}

bool KernelEngine::start_client(const char* address) {
    if (running_) {
        return false;
    }
    auto gns_transport = std::make_unique<GnsTransport>();
    if (!gns_transport->StartClient(address)) {
        push_event(KernelEventType_Error, 0, 0, 1);
        return false;
    }

    listen_server_transport_ = nullptr;
    transport_ = std::move(gns_transport);
    reset_runtime_state(KernelMode_Client);
    physics_world_.reset();
    prediction_physics_world_.reset();
    return true;
}

bool KernelEngine::start_client_catalog_sync(
    const char* address,
    const KernelGameplayCatalogSyncClientConfig& config) {
    if (config.max_bundle_size >
        KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE) {
        return false;
    }
    if (!start_client(address)) {
        return false;
    }
    gameplay_catalog_sync_max_bundle_size_ =
        config.max_bundle_size == 0
            ? KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_MAX_BUNDLE_SIZE
            : config.max_bundle_size;
    gameplay_catalog_sync_timeout_ms_ =
        config.timeout_ms == 0
            ? KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_TIMEOUT_MS
            : config.timeout_ms;
    gameplay_catalog_sync_elapsed_us_ = 0;
    gameplay_catalog_sync_error_ = KernelGameplayCatalogSyncError_None;
    gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Connecting;
    downloaded_gameplay_catalog_bundle_.clear();
    gameplay_catalog_manifest_ = KernelGameplayCatalogManifest{};
    gameplay_catalog_manifest_.struct_size =
        sizeof(KernelGameplayCatalogManifest);
    return true;
}

bool KernelEngine::start_listen_server(std::uint16_t port) {
    std::unique_ptr<physics::PhysicsWorld> query_world;
    if (!prepare_server_physics(&query_world)) {
        return false;
    }
    if (!gameplay_catalog_sync_bundle_.empty() &&
        (gameplay_catalog_manifest_.catalog_version != catalog_version_ ||
         gameplay_catalog_manifest_.catalog_hash != catalog_hash_)) {
        return false;
    }
    prediction_physics_world_.reset();
    if ((!static_collision_scene_.empty() ||
         session_rules_.actor_blocking_mode ==
             KernelActorBlockingMode_Predicted) &&
        !prepare_prediction_physics()) {
        return false;
    }
    auto listen_transport = std::make_unique<ListenServerTransport>();
    if (!listen_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 2);
        return false;
    }

    listen_server_transport_ = listen_transport.get();
    transport_ = std::move(listen_transport);
    reset_runtime_state(KernelMode_ListenServer);
    physics_world_ = std::move(query_world);
    world_.set_collision_world(physics_world_.get());

    const NetId player =
        world_.spawn_player(kLocalListenPeerId, glm::vec3{0.0f, 0.0f, 0.0f});
    register_actor_for_first_physics(player);
    local_client_peer_id_ = kLocalListenPeerId;
    local_player_net_id_ = player;
    local_listen_session_ = PeerSession{kLocalListenPeerId, player, 0, true, {}};
    local_listen_session_.has_clock_sync = true;
    local_listen_session_.clock_offset_us = 0;
    has_welcome_ = true;

    push_event(KernelEventType_Connected, 0, kLocalListenPeerId);
    push_event(KernelEventType_PlayerJoined, player, kLocalListenPeerId);
    push_event(
        KernelEventType_EntitySpawned,
        player,
        kLocalListenPeerId,
        static_cast<std::uint32_t>(EntityType::kActor));
    publish_snapshot();
    poll_client_transport();
    rebuild_render_states();
    return true;
}

bool KernelEngine::start_dedicated_server(std::uint16_t port) {
    std::unique_ptr<physics::PhysicsWorld> query_world;
    if (!prepare_server_physics(&query_world)) {
        return false;
    }
    if (!gameplay_catalog_sync_bundle_.empty() &&
        (gameplay_catalog_manifest_.catalog_version != catalog_version_ ||
         gameplay_catalog_manifest_.catalog_hash != catalog_hash_)) {
        return false;
    }
    auto gns_transport = std::make_unique<GnsTransport>();
    listen_server_transport_ = nullptr;
    if (!gns_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 3);
        return false;
    }

    transport_ = std::move(gns_transport);
    reset_runtime_state(KernelMode_DedicatedServer);
    prediction_physics_world_.reset();
    physics_world_ = std::move(query_world);
    world_.set_collision_world(physics_world_.get());
    publish_snapshot();
    rebuild_render_states();
    return true;
}

bool KernelEngine::set_gameplay_catalog_sync_bundle(
    const KernelGameplayCatalogSyncServerConfig& config,
    KernelGameplayCatalogManifest* out_manifest) {
    if (running_ || !is_server_mode(config_.mode) || catalog_hash_ == 0 ||
        config.bundle_bytes == nullptr ||
        config.bundle_size == 0 ||
        config.bundle_size > KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE ||
        config.entry_path == nullptr ||
        config.entry_path[0] == '\0' || out_manifest == nullptr ||
        std::strlen(config.entry_path) >= KERNEL_GAMEPLAY_CATALOG_ENTRY_PATH_SIZE ||
        !is_valid_bundle_entry_path(config.entry_path)) {
        return false;
    }
    const char* content_namespace =
        config.content_namespace == nullptr || config.content_namespace[0] == '\0'
            ? "default"
            : config.content_namespace;
    if (std::strlen(content_namespace) >=
            KERNEL_GAMEPLAY_CATALOG_CONTENT_NAMESPACE_SIZE ||
        !is_valid_content_namespace(content_namespace)) {
        return false;
    }

    gameplay_catalog_sync_bundle_.assign(
        config.bundle_bytes,
        config.bundle_bytes + config.bundle_size);
    gameplay_catalog_manifest_ = KernelGameplayCatalogManifest{};
    gameplay_catalog_manifest_.struct_size =
        sizeof(KernelGameplayCatalogManifest);
    gameplay_catalog_manifest_.catalog_version = catalog_version_;
    gameplay_catalog_manifest_.catalog_hash = catalog_hash_;
    gameplay_catalog_manifest_.bundle_size = config.bundle_size;
    const std::array<std::uint8_t, 32> digest =
        compute_sha256(config.bundle_bytes, config.bundle_size);
    std::memcpy(
        gameplay_catalog_manifest_.bundle_sha256,
        digest.data(),
        digest.size());
    std::snprintf(
        gameplay_catalog_manifest_.entry_path,
        sizeof(gameplay_catalog_manifest_.entry_path),
        "%s",
        config.entry_path);
    std::snprintf(
        gameplay_catalog_manifest_.content_namespace,
        sizeof(gameplay_catalog_manifest_.content_namespace),
        "%s",
        content_namespace);
    *out_manifest = gameplay_catalog_manifest_;
    return true;
}

bool KernelEngine::get_gameplay_catalog_sync_status(
    KernelGameplayCatalogSyncStatus* out_status) const {
    if (out_status == nullptr) {
        return false;
    }
    const std::uint32_t struct_size = out_status->struct_size;
    *out_status = KernelGameplayCatalogSyncStatus{};
    out_status->struct_size = struct_size;
    out_status->state = gameplay_catalog_sync_state_;
    out_status->error = gameplay_catalog_sync_error_;
    out_status->received_bundle_size =
        static_cast<std::uint32_t>(downloaded_gameplay_catalog_bundle_.size());
    out_status->manifest = gameplay_catalog_manifest_;
    out_status->manifest.struct_size = sizeof(KernelGameplayCatalogManifest);
    return true;
}

bool KernelEngine::request_gameplay_catalog_bundle() {
    if (config_.mode != KernelMode_Client ||
        gameplay_catalog_sync_state_ !=
            KernelGameplayCatalogSyncState_ManifestReady ||
        transport_ == nullptr) {
        return false;
    }
    GameplayCatalogBundleRequestPacket request;
    std::copy(
        std::begin(gameplay_catalog_manifest_.bundle_sha256),
        std::end(gameplay_catalog_manifest_.bundle_sha256),
        request.bundle_sha256.begin());
    const std::vector<std::uint8_t> packet =
        encode_gameplay_catalog_bundle_request_packet(
            request,
            next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError_Transport);
        return false;
    }
    downloaded_gameplay_catalog_bundle_.clear();
    downloaded_gameplay_catalog_bundle_.reserve(
        gameplay_catalog_manifest_.bundle_size);
    gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Downloading;
    gameplay_catalog_sync_elapsed_us_ = 0;
    return true;
}

bool KernelEngine::copy_gameplay_catalog_bundle(
    std::uint8_t* out_bundle,
    std::uint32_t out_capacity,
    std::uint32_t* out_bundle_size) const {
    if (gameplay_catalog_sync_state_ !=
            KernelGameplayCatalogSyncState_BundleReady ||
        out_bundle_size == nullptr) {
        return false;
    }
    *out_bundle_size =
        static_cast<std::uint32_t>(downloaded_gameplay_catalog_bundle_.size());
    if (out_bundle == nullptr ||
        out_capacity < downloaded_gameplay_catalog_bundle_.size()) {
        return false;
    }
    std::memcpy(
        out_bundle,
        downloaded_gameplay_catalog_bundle_.data(),
        downloaded_gameplay_catalog_bundle_.size());
    return true;
}

bool KernelEngine::continue_client_handshake() {
    if (config_.mode != KernelMode_Client ||
        (gameplay_catalog_sync_state_ !=
             KernelGameplayCatalogSyncState_ManifestReady &&
         gameplay_catalog_sync_state_ !=
             KernelGameplayCatalogSyncState_BundleReady) ||
        catalog_version_ != gameplay_catalog_manifest_.catalog_version ||
        catalog_hash_ != gameplay_catalog_manifest_.catalog_hash) {
        return false;
    }
    send_client_handshake();
    if (!client_handshake_sent_) {
        fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError_Transport);
        return false;
    }
    gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Handshaking;
    gameplay_catalog_sync_elapsed_us_ = 0;
    return true;
}

void KernelEngine::update(float delta_seconds) {
    if (!running_) {
        return;
    }

    if (delta_seconds > 0.0f &&
        (config_.mode == KernelMode_Client || config_.mode == KernelMode_ListenServer)) {
        client_local_time_us_ += static_cast<std::uint64_t>(
            static_cast<double>(delta_seconds) * 1000000.0);
        advance_predicted_projectile_corrections(delta_seconds);
    }
    for (auto outstanding = outstanding_predicted_actions_.begin();
         outstanding != outstanding_predicted_actions_.end();) {
        if (client_local_time_us_ <= outstanding->second.last_activity_us +
                                         kLocalActionResultTimeoutUs) {
            ++outstanding;
            continue;
        }
        const std::uint32_t expired_action_id = outstanding->first;
        if (const auto actor = world_.find_entity(local_player_net_id_);
            actor.has_value() && world_.registry().all_of<WeaponState>(*actor)) {
            WeaponState& weapon = world_.registry().get<WeaponState>(*actor);
            const std::size_t slot =
                find_weapon_slot(weapon, outstanding->second.weapon_id);
            if (slot < weapon.weapon_slot_count) {
                weapon.ammo[slot] =
                    outstanding->second.ammo_before;
            }
            weapon.active_effect_net_id = outstanding->second.active_effect_before;
        }
        predicted_projectiles_.erase(
            std::remove_if(
                predicted_projectiles_.begin(),
                predicted_projectiles_.end(),
                [expired_action_id](const PredictedProjectile& projectile) {
                    return !projectile.bound &&
                           projectile.action_instance_id == expired_action_id;
                }),
            predicted_projectiles_.end());
        if (predicted_local_entity_.action_instance_id == expired_action_id) {
            predicted_local_entity_.action_phase = KernelActionPhase_None;
            predicted_local_entity_.action_template_id = 0u;
            predicted_local_entity_.action_instance_id = 0u;
            predicted_action_next_commit_tick_ = 0u;
            predicted_action_recovery_end_tick_ = 0u;
        }
        if (network_stats_enabled()) {
            ++network_stats_.local_action_results_timed_out;
        }
        outstanding = outstanding_predicted_actions_.erase(outstanding);
    }
    if (delta_seconds > 0.0f &&
        gameplay_catalog_sync_state_ >=
            KernelGameplayCatalogSyncState_Connecting &&
        gameplay_catalog_sync_state_ <=
            KernelGameplayCatalogSyncState_Handshaking) {
        gameplay_catalog_sync_elapsed_us_ += static_cast<std::uint64_t>(
            static_cast<double>(delta_seconds) * 1000000.0);
        if (gameplay_catalog_sync_elapsed_us_ >
            static_cast<std::uint64_t>(gameplay_catalog_sync_timeout_ms_) *
                1000u) {
            fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError_Timeout);
        }
    }

    poll_transport();
    pump_gameplay_catalog_transfers();
    const std::uint32_t ticks_to_run = tick_loop_.accumulate(delta_seconds);
    for (std::uint32_t tick = 0; tick < ticks_to_run; ++tick) {
        const bool emitted_client_input = emit_client_input_for_tick();
        if (emitted_client_input && config_.mode == KernelMode_ListenServer) {
            poll_transport();
        }
        simulate_tick();
    }
    poll_client_transport();
    advance_local_presentation(delta_seconds);
    rebuild_render_states();
}

void KernelEngine::submit_player_input(PeerId local_player_id, const KernelPlayerInput& input) {
    const bool local_client = config_.mode == KernelMode_Client ||
        (config_.mode == KernelMode_ListenServer &&
         listen_server_transport_ != nullptr);
    if (local_client) {
        if (!has_welcome_) {
            push_event(KernelEventType_Error, 0, 0, 8);
            return;
        }
        KernelPlayerInput prepared = prepare_client_input(input);
        const std::uint32_t action_instance_id =
            prepared.action_intent.action_instance_id;
        if (action_instance_id != 0u) {
            const bool duplicate =
                std::any_of(
                    pending_client_action_intents_.begin(),
                    pending_client_action_intents_.end(),
                    [action_instance_id](const KernelPlayerInput& pending) {
                        return pending.action_intent.action_instance_id ==
                            action_instance_id;
                    }) ||
                outstanding_predicted_actions_.contains(action_instance_id) ||
                applied_local_action_results_.contains(action_instance_id);
            if (!duplicate) {
                if (pending_client_action_intents_.size() >=
                    kMaxPendingClientActionIntents) {
                    push_event(
                        KernelEventType_Error,
                        local_player_net_id_,
                        local_client_peer_id_,
                        27);
                } else {
                    pending_client_action_intents_.push_back(prepared);
                }
            }
        }

        prepared.input_seq = 0u;
        prepared.action_intent = KernelActionIntent{};
        latest_client_input_ = prepared;
        latest_client_input_time_us_ = client_local_time_us_;
        latest_client_input_peer_ = config_.mode == KernelMode_ListenServer
            ? local_player_id
            : local_client_peer_id_;
        has_latest_client_input_ = true;
        return;
    }

    const std::uint64_t received_server_time_us = current_server_time_us();
    const std::uint64_t action_server_time_us =
        input.client_action_time_us == 0 ? received_server_time_us
                                         : input.client_action_time_us;
    pending_inputs_.push_back(QueuedInput{
        local_player_id,
        input,
        tick_loop_.current_tick(),
        action_server_time_us,
        true,
    });
    local_last_processed_input_seq_ =
        std::max(local_last_processed_input_seq_, input.input_seq);
}

bool KernelEngine::emit_client_input_for_tick() {
    if (!has_welcome_ || !has_latest_client_input_) {
        return false;
    }

    KernelPlayerInput input = latest_client_input_;
    const bool fresh = client_local_time_us_ >= latest_client_input_time_us_ &&
        client_local_time_us_ - latest_client_input_time_us_ <=
            kInputIntentTimeoutUs;
    if (!fresh) {
        input.move = KernelVec2{};
        input.look_delta = KernelVec2{};
        input.buttons = 0u;
        input.action_input = KernelActionInput{};
    }
    input.action_intent = KernelActionIntent{};
    if (!pending_client_action_intents_.empty()) {
        const KernelPlayerInput edge = pending_client_action_intents_.front();
        pending_client_action_intents_.pop_front();
        input.action_intent = edge.action_intent;
        input.client_action_time_us = edge.client_action_time_us;
        input.aim_dir = edge.aim_dir;
        input.selected_weapon = edge.selected_weapon;
    } else if (input.client_action_time_us == 0u) {
        input.client_action_time_us = client_local_action_time_us();
    }
    input.input_seq = next_client_input_seq_++;
    latest_client_input_.look_delta = KernelVec2{};
    process_client_input_command(latest_client_input_peer_, input);
    return true;
}

void KernelEngine::process_client_input_command(
    PeerId peer,
    const KernelPlayerInput& input) {
    predict_local_input(input);
    std::size_t predicted_weapon_slot = kWeaponSlotCount;
    const WeaponState* predicted_weapon_state = nullptr;
    if (const auto actor = world_.find_entity(local_player_net_id_);
        actor.has_value() && world_.registry().all_of<WeaponState>(*actor)) {
        predicted_weapon_state = &world_.registry().get<WeaponState>(*actor);
        predicted_weapon_slot =
            find_weapon_slot(*predicted_weapon_state, input.selected_weapon);
    }
    const std::uint32_t primary_gate_before =
        predicted_weapon_slot < predicted_next_primary_commit_tick_.size()
            ? predicted_next_primary_commit_tick_[predicted_weapon_slot]
            : 0u;
    const WeaponMechanicsDefinition* predicted_weapon = entity_weapon_mechanics(
        world_, local_player_net_id_, input.selected_weapon);
    const std::uint32_t predicted_action_template_id =
        predicted_weapon == nullptr
            ? 0u
            : input.action_intent.binding_id == KernelActionBinding_PrimaryFire
                ? predicted_weapon->fire_action_template_id
                : predicted_weapon->reload_action_template_id;
    const bool predicted_commit = predict_local_action(input);
    if (input.action_intent.action_instance_id != 0u) {
        std::uint16_t ammo_before = 0u;
        NetId active_effect_before = 0u;
        if (predicted_weapon_state != nullptr) {
            if (predicted_weapon_slot <
                predicted_weapon_state->weapon_slot_count) {
                ammo_before =
                    predicted_weapon_state->ammo[predicted_weapon_slot];
            }
            active_effect_before =
                predicted_weapon_state->active_effect_net_id;
        }
        auto outstanding = outstanding_predicted_actions_.try_emplace(
            input.action_intent.action_instance_id,
            OutstandingPredictedAction{
                input.action_intent.action_instance_id,
                client_local_time_us_,
                0u,
                input.action_intent.binding_id,
                input.selected_weapon,
                predicted_action_template_id,
                primary_gate_before,
                0u,
                ammo_before,
                active_effect_before,
            }).first;
        outstanding->second.last_activity_us = client_local_time_us_;
    }
    if (predicted_commit) {
        predict_local_projectile(input);
    }

    const std::vector<std::uint8_t> packet =
        encode_player_input_packet(peer, input, next_packet_sequence_++);
    const bool sent = config_.mode == KernelMode_ListenServer
        ? listen_server_transport_ != nullptr &&
            listen_server_transport_->SendLocalClient(
                peer,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kInput)
        : transport_ != nullptr && transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kUnreliable,
            ChannelId::kInput);
    if (!sent) {
        push_event(KernelEventType_Error, 0, peer, 4);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kUnreliable,
        ChannelId::kInput);
}

bool KernelEngine::cache_server_movement_input(
    PeerSession* session,
    const KernelPlayerInput& input,
    std::uint64_t received_server_time_us) {
    if (session == nullptr ||
        (session->has_received_input &&
         input.input_seq <= session->last_received_input_seq)) {
        return false;
    }
    session->latest_movement_input = input;
    session->latest_movement_input.action_intent = KernelActionIntent{};
    session->latest_movement_input.action_input = KernelActionInput{};
    session->last_received_input_seq = input.input_seq;
    session->last_movement_input_server_time_us = received_server_time_us;
    session->has_received_input = true;
    session->has_movement_input = true;
    return true;
}

std::vector<QueuedInput> KernelEngine::build_effective_movement_inputs(
    std::uint64_t server_time_us) {
    std::vector<QueuedInput> effective = pending_inputs_;
    const auto append_held_input = [this, server_time_us, &effective](
                                       PeerSession* session) {
        if (session == nullptr || !session->welcomed ||
            !session->has_movement_input) {
            return;
        }
        if (server_time_us < session->last_movement_input_server_time_us ||
            server_time_us - session->last_movement_input_server_time_us >
                kInputIntentTimeoutUs) {
            session->has_movement_input = false;
            session->latest_movement_input = KernelPlayerInput{};
            return;
        }
        effective.push_back(QueuedInput{
            session->peer,
            session->latest_movement_input,
            tick_loop_.current_tick(),
            0u,
            false,
        });
    };

    if (config_.mode == KernelMode_ListenServer) {
        append_held_input(&local_listen_session_);
    }
    for (PeerSession& session : peer_sessions_) {
        append_held_input(&session);
    }
    return effective;
}

void KernelEngine::acknowledge_simulated_movement_inputs(
    const std::vector<QueuedInput>& inputs) {
    for (const QueuedInput& queued_input : inputs) {
        if (queued_input.controlled_net_id != 0u) {
            continue;
        }
        PeerSession* session = result_session_for_peer(queued_input.owner_peer);
        if (session != nullptr) {
            session->last_processed_input_seq = std::max(
                session->last_processed_input_seq,
                queued_input.input.input_seq);
        }
        if (config_.mode == KernelMode_ListenServer &&
            queued_input.owner_peer == kLocalListenPeerId) {
            local_last_processed_input_seq_ = std::max(
                local_last_processed_input_seq_,
                queued_input.input.input_seq);
        }
    }
}

bool KernelEngine::load_gameplay_catalog(
    const KernelGameplayCatalogDefinition& catalog) {
    if (catalog.struct_size < sizeof(KernelGameplayCatalogDefinition)) {
        return false;
    }
    if ((catalog.actor_template_count != 0 &&
         catalog.actor_templates == nullptr) ||
        (catalog.projectile_template_count != 0 &&
         catalog.projectile_templates == nullptr) ||
        (catalog.collider_template_count != 0 &&
         catalog.collider_templates == nullptr) ||
        (catalog.action_template_count != 0 &&
         catalog.action_templates == nullptr) ||
        (catalog.item_template_count != 0 &&
         catalog.item_templates == nullptr) ||
        (catalog.prop_population_rule_count != 0 &&
         catalog.prop_population_rules == nullptr) ||
        (catalog.skeleton_asset_count != 0 &&
         catalog.skeleton_assets == nullptr) ||
        (catalog.status_effect_count != 0 &&
         catalog.status_effects == nullptr) ||
        (catalog.game_rule_count != 0 && catalog.game_rules == nullptr) ||
        (catalog.game_rule_node_count != 0 &&
         catalog.game_rule_nodes == nullptr) ||
        (catalog.game_rule_edge_count != 0 &&
         catalog.game_rule_edges == nullptr) ||
        (catalog.game_rule_effect_count != 0 &&
         catalog.game_rule_effects == nullptr) ||
        (catalog.entity_template_count != 0 &&
         catalog.entity_templates == nullptr) ||
        catalog.collider_binding_count != 0) {
        return false;
    }

    std::vector<RuntimeSkeletonAsset> validated_skeleton_assets;
    validated_skeleton_assets.reserve(catalog.skeleton_asset_count);
    for (std::uint32_t index = 0; index < catalog.skeleton_asset_count; ++index) {
        RuntimeSkeletonAsset asset;
        if (!load_runtime_skeleton_asset(catalog.skeleton_assets[index], &asset) ||
            find_skeleton_asset(
                validated_skeleton_assets,
                asset.skeleton_asset_id) != nullptr) {
            return false;
        }
        validated_skeleton_assets.push_back(std::move(asset));
    }

    std::vector<KernelActionTemplateDefinition> validated_action_templates;
    validated_action_templates.reserve(catalog.action_template_count);
    for (std::uint32_t index = 0; index < catalog.action_template_count; ++index) {
        const KernelActionTemplateDefinition& action_template =
            catalog.action_templates[index];
        if (!validate_action_template(action_template) ||
            find_action_template(
                validated_action_templates,
                action_template.action_template_id) != nullptr) {
            return false;
        }
        validated_action_templates.push_back(action_template);
    }
    const auto incoming_has_action =
        [&validated_action_templates](std::uint32_t action_template_id) {
            return find_action_template(
                       validated_action_templates,
                       action_template_id) != nullptr;
        };
    const auto weapon_tuning_view = world_.registry().view<WeaponTuning>();
    for (const auto entity : weapon_tuning_view) {
        const WeaponTuning& tuning = weapon_tuning_view.get<WeaponTuning>(entity);
        for (std::size_t index = 0; index < tuning.configured.size(); ++index) {
            if (tuning.configured[index]) {
                const KernelActionTemplateDefinition* fire_action =
                    find_action_template(
                        validated_action_templates,
                        tuning.definitions[index].fire_action_template_id);
                if (fire_action == nullptr ||
                    fire_action->commit_interval_ticks == 0u ||
                    !incoming_has_action(
                        tuning.definitions[index].reload_action_template_id)) {
                    return false;
                }
            }
        }
    }

    std::vector<KernelEntityTemplateDefinition> validated_entity_templates;
    std::vector<KernelActorTemplateDefinition> validated_actor_templates;
    std::vector<KernelProjectileTemplateDefinition>
        validated_projectile_templates;
    std::vector<KernelColliderTemplateDefinition> validated_collider_templates;
    std::vector<KernelGameRuleDefinition> validated_game_rules;
    std::vector<KernelGameRuleNodeDefinition> validated_game_rule_nodes;
    std::vector<KernelGameRuleEdgeDefinition> validated_game_rule_edges;
    std::vector<KernelGameRuleSpawnGroupEffectDefinition>
        validated_game_rule_effects;
    validated_entity_templates.reserve(catalog.entity_template_count);
    validated_actor_templates.reserve(catalog.actor_template_count);
    validated_projectile_templates.reserve(catalog.projectile_template_count);
    validated_collider_templates.reserve(catalog.collider_template_count);
    std::vector<KernelItemTemplateDefinition> validated_item_templates;
    validated_item_templates.reserve(catalog.item_template_count);
    std::vector<KernelStatusEffectDefinition> validated_status_effects;
    validated_status_effects.reserve(catalog.status_effect_count);
    const auto status_id_in_use =
        [&validated_status_effects](std::uint32_t status_effect_id) {
            return std::any_of(
                validated_status_effects.begin(), validated_status_effects.end(),
                [status_effect_id](const KernelStatusEffectDefinition& existing) {
                    return existing.status_effect_id == status_effect_id;
                });
        };
    for (std::uint32_t index = 0; index < catalog.status_effect_count; ++index) {
        const KernelStatusEffectDefinition& status = catalog.status_effects[index];
        const bool is_stack = status.replacement_policy ==
            KernelStatusEffectReplacementPolicy_Stack;
        if (status.struct_size < sizeof(KernelStatusEffectDefinition) ||
            status.status_effect_id == 0u || status.channel_id == 0u ||
            status.duration_ticks == 0u ||
            status.interval_ticks > status.duration_ticks ||
            status.replacement_policy >
                KernelStatusEffectReplacementPolicy_Stack ||
            (is_stack &&
             (status.max_stacks < 2u || status.max_stacks > 32u ||
              status.refresh_on_stack > 1u)) ||
            (!is_stack &&
             (status.max_stacks != 0u || status.refresh_on_stack != 0u)) ||
            status.reserved0 != 0u || status.reserved1 != 0u ||
            status.reserved2 != 0u ||
            status_id_in_use(status.status_effect_id)) {
            return false;
        }
        const std::uint32_t stack_scale = is_stack ? status.max_stacks : 1u;
        const auto validate_scaled_action =
            [&](std::uint8_t action_type,
                std::uint16_t damage_amount,
                std::int32_t health_change_amount,
                std::uint8_t modifier_operation,
                float modifier_value,
                bool scale_amount) {
                if (scale_amount &&
                    action_type ==
                        KernelEntityTriggerActionType_ApplyDamage &&
                    static_cast<std::uint64_t>(damage_amount) * stack_scale >
                        UINT16_MAX) {
                    return false;
                }
                if (scale_amount &&
                    action_type ==
                        KernelEntityTriggerActionType_ApplyHealthChange) {
                    const std::int64_t scaled =
                        static_cast<std::int64_t>(health_change_amount) *
                        stack_scale;
                    if (scaled < INT32_MIN || scaled > INT32_MAX) {
                        return false;
                    }
                }
                if (action_type ==
                    KernelEntityTriggerActionType_ApplySpeedModifier) {
                    const double scaled =
                        modifier_operation ==
                                KernelStatModifierOperation_Additive
                            ? static_cast<double>(modifier_value) * stack_scale
                            : std::pow(
                                  static_cast<double>(modifier_value),
                                  stack_scale);
                    if (!std::isfinite(scaled) ||
                        std::abs(scaled) >
                            std::numeric_limits<float>::max()) {
                        return false;
                    }
                }
                return true;
            };
        const auto validate_status_trigger = [&](const KernelActionTriggerDefinition& trigger,
                                                 TriggerEventType event_type,
                                                 bool allow_speed_modifier) {
            if (trigger.struct_size == 0u) {
                return true;
            }
            const std::optional<CompiledActionGraphBinding> binding =
                compile_action_trigger_definition(event_type, trigger);
            if (!binding.has_value()) {
                return false;
            }
            for (const ActionGraphAction& action : binding->graph.actions) {
                const bool damage_or_health =
                    std::holds_alternative<ActionApplyDamageDefinition>(action) ||
                    std::holds_alternative<ActionApplyHealthChangeDefinition>(action);
                const bool speed_modifier =
                    std::holds_alternative<ActionApplySpeedModifierDefinition>(action);
                if (!damage_or_health && !(allow_speed_modifier && speed_modifier)) {
                    return false;
                }
            }
            const std::uint32_t action_count = std::min<std::uint32_t>(
                trigger.action_count, KERNEL_MAX_ACTION_GRAPH_ACTIONS);
            const bool scale_amount =
                event_type == TriggerEventType::kStatusTick ||
                event_type == TriggerEventType::kStatusExpired;
            if (trigger.action_count == 0u &&
                !validate_scaled_action(
                    trigger.action_type,
                    trigger.damage_amount,
                    trigger.health_change_amount,
                    trigger.modifier_operation,
                    trigger.modifier_value,
                    scale_amount)) {
                return false;
            }
            if (trigger.action_count == 0u &&
                trigger.action_type ==
                    KernelEntityTriggerActionType_ApplySpeedModifier &&
                trigger.target_source != KernelEntityRefSource_Self &&
                trigger.target_source != KernelEntityRefSource_EventSubject) {
                return false;
            }
            for (std::uint32_t action_index = 0u;
                 action_index < action_count;
                 ++action_index) {
                const KernelActionDefinition& action = trigger.actions[action_index];
                if (!validate_scaled_action(
                        action.action_type,
                        action.damage_amount,
                        action.health_change_amount,
                        action.modifier_operation,
                        action.modifier_value,
                        scale_amount)) {
                    return false;
                }
                if (action.action_type ==
                        KernelEntityTriggerActionType_ApplySpeedModifier &&
                    action.target_source != KernelEntityRefSource_Self &&
                    action.target_source != KernelEntityRefSource_EventSubject) {
                    return false;
                }
            }
            return true;
        };
        if (!validate_status_trigger(
                status.on_apply_trigger, TriggerEventType::kStatusApplied, true) ||
            !validate_status_trigger(
                status.on_tick_trigger, TriggerEventType::kStatusTick, false) ||
            !validate_status_trigger(
                status.on_expire_trigger, TriggerEventType::kStatusExpired, false) ||
            (status.interval_ticks == 0u && status.on_tick_trigger.struct_size != 0u)) {
            return false;
        }
        validated_status_effects.push_back(status);
    }
    std::vector<KernelPropPopulationRuleDefinition>
        validated_prop_population_rules;
    validated_prop_population_rules.reserve(
        catalog.prop_population_rule_count);
    for (std::uint32_t index = 0;
         index < catalog.prop_population_rule_count;
         ++index) {
        const KernelPropPopulationRuleDefinition& rule =
            catalog.prop_population_rules[index];
        if (rule.struct_size <
                sizeof(KernelPropPopulationRuleDefinition) ||
            rule.population_group_id == 0u || rule.max_alive == 0u ||
            rule.max_alive > 256u ||
            std::any_of(
                validated_prop_population_rules.begin(),
                validated_prop_population_rules.end(),
                [&](const KernelPropPopulationRuleDefinition& candidate) {
                    return candidate.population_group_id ==
                        rule.population_group_id;
                })) {
            return false;
        }
        validated_prop_population_rules.push_back(rule);
    }
    std::string item_validation_error;
    for (std::uint32_t index = 0; index < catalog.item_template_count; ++index) {
        const KernelItemTemplateDefinition& item_template =
            catalog.item_templates[index];
        if (!validate_item_template(item_template, &item_validation_error) ||
            std::any_of(
                validated_item_templates.begin(),
                validated_item_templates.end(),
                [&](const KernelItemTemplateDefinition& candidate) {
                    return candidate.item_template_id ==
                        item_template.item_template_id;
                })) {
            return false;
        }
        validated_item_templates.push_back(item_template);
    }
    for (std::uint32_t index = 0; index < catalog.actor_template_count; ++index) {
        const KernelActorTemplateDefinition& actor_template =
            catalog.actor_templates[index];
        if (actor_template.struct_size < sizeof(KernelActorTemplateDefinition) ||
            actor_template.actor_template_id == 0 ||
            actor_template.entity_type !=
                static_cast<std::uint16_t>(EntityType::kActor) ||
            (actor_template.actor_type != KernelActorType_Player &&
             actor_template.actor_type != KernelActorType_Agent) ||
            actor_template.collider_template_id == 0 ||
            actor_template.vision.struct_size < sizeof(KernelAgentVisionConfig) ||
            !is_valid_agent_camp(actor_template.vision.camp)) {
            return false;
        }
        validated_actor_templates.push_back(actor_template);
    }
    for (std::uint32_t index = 0; index < catalog.entity_template_count; ++index) {
        const KernelEntityTemplateDefinition& entity_template =
            catalog.entity_templates[index];
        if (entity_template.struct_size < sizeof(KernelEntityTemplateDefinition) ||
            entity_template.entity_template_id == 0 ||
            (entity_template.entity_type != KernelEntityType_Actor &&
             entity_template.entity_type != KernelEntityType_Prop &&
             entity_template.entity_type != KernelEntityType_Director) ||
            entity_template.ai.struct_size < sizeof(KernelEntityAiDefinition) ||
            entity_template.ai.controller_type > KernelAiControllerType_Chaser) {
            return false;
        }
        const KernelSkeletonBindingDefinition& skeleton =
            entity_template.skeleton;
        const bool skeleton_enabled = skeleton.struct_size != 0u;
        if (skeleton_enabled !=
            ((entity_template.component_flags &
              KERNEL_ENTITY_COMPONENT_SKELETON) != 0u)) {
            return false;
        }
        if (skeleton_enabled) {
            const RuntimeSkeletonAsset* asset = find_skeleton_asset(
                validated_skeleton_assets,
                skeleton.skeleton_asset_id);
            if (skeleton.struct_size <
                    sizeof(KernelSkeletonBindingDefinition) ||
                asset == nullptr ||
                skeleton.skeleton_content_hash !=
                    asset->skeleton_content_hash ||
                skeleton.bone_count !=
                    static_cast<std::uint32_t>(asset->skeleton.num_joints()) ||
                skeleton.root_bone_index >= skeleton.bone_count ||
                skeleton.body_bone_index >= skeleton.bone_count ||
                skeleton.leg_count == 0u ||
                skeleton.leg_count > KERNEL_MAX_SKELETON_LEGS ||
                skeleton.processing_order_count != skeleton.leg_count ||
                !validate_locomotion_definition(skeleton) ||
                !std::isfinite(skeleton.input_deadzone) ||
                skeleton.input_deadzone < 0.0f ||
                skeleton.input_deadzone >= 1.0f ||
                !std::isfinite(skeleton.step_threshold_meters) ||
                skeleton.step_threshold_meters <= 0.0f ||
                skeleton.step_duration_ticks == 0u ||
                skeleton.max_swinging_legs == 0u ||
                skeleton.max_swinging_legs > skeleton.leg_count ||
                !std::isfinite(
                    entity_template.movement.max_yaw_degrees_per_second) ||
                entity_template.movement.max_yaw_degrees_per_second <= 0.0f) {
                return false;
            }
            std::array<bool, KERNEL_MAX_SKELETON_LEGS> ordered{};
            for (std::uint32_t leg_index = 0u;
                 leg_index < skeleton.leg_count;
                 ++leg_index) {
                const KernelSkeletonLegDefinition& leg =
                    skeleton.legs[leg_index];
                if (leg.leg_id != leg_index ||
                    leg.hip_bone_index >= skeleton.bone_count ||
                    leg.knee_bone_index >= skeleton.bone_count ||
                    leg.foot_bone_index >= skeleton.bone_count ||
                    leg.hip_bone_index == leg.knee_bone_index ||
                    leg.hip_bone_index == leg.foot_bone_index ||
                    leg.knee_bone_index == leg.foot_bone_index ||
                    leg.gait_group >= skeleton.leg_count ||
                    !std::isfinite(leg.pole_local.x) ||
                    !std::isfinite(leg.pole_local.y) ||
                    !std::isfinite(leg.pole_local.z) ||
                    !std::isfinite(leg.step_height_meters) ||
                    leg.step_height_meters < 0.0f ||
                    !std::isfinite(leg.max_reach_ratio) ||
                    leg.max_reach_ratio <= 0.0f ||
                    leg.max_reach_ratio > 1.0f ||
                    asset->skeleton.joint_parents()[leg.knee_bone_index] !=
                        static_cast<std::int16_t>(leg.hip_bone_index) ||
                    asset->skeleton.joint_parents()[leg.foot_bone_index] !=
                        static_cast<std::int16_t>(leg.knee_bone_index)) {
                    return false;
                }
                const std::uint32_t ordered_leg =
                    skeleton.processing_order[leg_index];
                if (ordered_leg >= skeleton.leg_count || ordered[ordered_leg]) {
                    return false;
                }
                ordered[ordered_leg] = true;
            }
            // Cross-check the authored legs against the rig's actual geometry,
            // so a knee hinge axis that does not match the bind pose is caught
            // here rather than quietly producing a limb that will not bend.
            std::uint32_t invalid_leg = 0u;
            if (!validate_locomotion_rig(
                    asset->skeleton, skeleton, &invalid_leg)) {
                spdlog::error(
                    "entity template {} skeleton leg {} does not match the rig: "
                    "mid_axis must be roughly parallel to the bind-pose knee "
                    "hinge cross(knee - hip, foot - knee)",
                    entity_template.entity_template_id,
                    invalid_leg);
                return false;
            }
            // Limb colliders take their size from the bone's rest scale, so the
            // rig is the only place that can say whether an authored bone
            // actually carries one.
            std::uint32_t invalid_limb = 0u;
            if (!validate_locomotion_colliders(
                    asset->bind_pose, skeleton, &invalid_limb)) {
                spdlog::error(
                    "entity template {} skeleton limb collider {} does not "
                    "match the rig: the bone must exist, be named once, and "
                    "carry its size as a non-unit rest scale",
                    entity_template.entity_template_id,
                    invalid_limb);
                return false;
            }
        }
        for (const KernelActionTriggerDefinition* trigger : {
                 &entity_template.activated_trigger,
                 &entity_template.collision_trigger,
                 &entity_template.health_depleted_trigger,
                 &entity_template.destroy_entity_trigger,
             }) {
            if (trigger->struct_size == 0u) {
                continue;
            }
            if (trigger->struct_size < sizeof(KernelActionTriggerDefinition)) {
                return false;
            }
            const std::uint32_t count = trigger->action_count == 0u
                ? (trigger->action_type == KernelEntityTriggerActionType_None
                       ? 0u
                       : 1u)
                : trigger->action_count;
            if (count > KERNEL_MAX_ACTION_GRAPH_ACTIONS) {
                return false;
            }
            for (std::uint32_t action_index = 0;
                 action_index < count;
                 ++action_index) {
                KernelActionDefinition action{};
                if (trigger->action_count == 0u) {
                    action.action_type = trigger->action_type;
                    action.target_source = trigger->target_source;
                    action.damage_amount = trigger->damage_amount;
                    action.spawn_entity_template_id =
                        trigger->spawn_entity_template_id;
                    // The projectile-side mirror has always carried this; this
                    // one did not, because nothing here read it until the
                    // SpawnProjectile branch below existed. The catalog loader
                    // always sets action_count, so only hand-built ABI input
                    // reaches this path -- which is exactly what the parity
                    // test drives.
                    action.spawn_projectile_template_id =
                        trigger->spawn_projectile_template_id;
                    // Same omission, found by the same test: the ApplyStatus /
                    // RemoveStatus branch below reads this and the mirror never
                    // set it, so a legacy-form status trigger was rejected for
                    // having status id 0 rather than for anything it said.
                    action.status_effect_id = trigger->status_effect_id;
                    // modifier_operation / modifier_value are deliberately not
                    // mirrored: no branch below reads them, because an entity
                    // trigger cannot carry a speed modifier at all. See the
                    // ApplySpeedModifier note at the end of the chain.
                    action.position_source = trigger->position_source;
                    action.direction_source = trigger->direction_source;
                    action.owner_source = trigger->owner_source;
                    action.health_change_amount =
                        trigger->health_change_amount;
                    action.impulse_strength = trigger->impulse_strength;
                    action.impulse_collision_mask =
                        trigger->impulse_collision_mask;
                    action.impulse_direction = trigger->impulse_direction;
                    action.impulse_lockout_ticks =
                        trigger->impulse_lockout_ticks;
                    action.impulse_strength_mode =
                        trigger->impulse_strength_mode;
                    action.impulse_strength_vertical =
                        trigger->impulse_strength_vertical;
                    action.condition_type = trigger->condition_type;
                } else {
                    action = trigger->actions[action_index];
                }
                if (action.condition_type >
                    KernelActionConditionType_EventHasTarget) {
                    return false;
                }
                if (action.action_type ==
                    KernelEntityTriggerActionType_ApplyDamage) {
                    if (action.target_source >
                            KernelEntityRefSource_EventInstigator ||
                        action.damage_amount == 0u) {
                        return false;
                    }
                    continue;
                }
                if (action.action_type ==
                    KernelEntityTriggerActionType_ApplyHealthChange) {
                    if (action.target_source >
                            KernelEntityRefSource_EventInstigator ||
                        action.health_change_amount == 0 ||
                        action.health_change_amount <
                            -static_cast<std::int32_t>(
                                std::numeric_limits<std::uint16_t>::max()) ||
                        action.health_change_amount >
                            static_cast<std::int32_t>(
                                std::numeric_limits<std::uint16_t>::max())) {
                        return false;
                    }
                    continue;
                }
                if (action.action_type ==
                    KernelEntityTriggerActionType_ApplyImpulse) {
                    if (action.target_source >
                            KernelEntityRefSource_EventInstigator ||
                        !impulse_strength_is_authorable(
                            action.impulse_strength_mode,
                            action.impulse_strength,
                            action.impulse_strength_vertical) ||
                        (action.direction_source !=
                             KernelEventVec3Source_Direction &&
                         action.direction_source !=
                             KernelEventVec3Source_SubjectDirection &&
                         action.direction_source !=
                             KernelEventVec3Source_Literal) ||
                        (action.direction_source ==
                             KernelEventVec3Source_Literal &&
                         (!std::isfinite(action.impulse_direction.x) ||
                          !std::isfinite(action.impulse_direction.y) ||
                          !std::isfinite(action.impulse_direction.z) ||
                          (action.impulse_direction.x == 0.0f &&
                           action.impulse_direction.y == 0.0f &&
                           action.impulse_direction.z == 0.0f))) ||
                        (action.impulse_collision_mask &
                         (KERNEL_COLLISION_MASK_ACTOR |
                          KERNEL_COLLISION_MASK_PROP)) == 0u ||
                        (action.impulse_collision_mask &
                         ~(KERNEL_COLLISION_MASK_ACTOR |
                           KERNEL_COLLISION_MASK_PROP)) != 0u ||
                        action.impulse_lockout_ticks >
                            KERNEL_MAX_IMPULSE_LOCKOUT_TICKS) {
                        return false;
                    }
                    continue;
                }
                if (action.action_type ==
                        KernelEntityTriggerActionType_ApplyStatus ||
                    action.action_type ==
                        KernelEntityTriggerActionType_RemoveStatus) {
                    if (action.target_source >
                            KernelEntityRefSource_EventInstigator ||
                        !status_id_in_use(action.status_effect_id)) {
                        return false;
                    }
                    continue;
                }
                // No ApplySpeedModifier branch, so one falls through to the
                // reject below. A speed modifier is not a standalone effect --
                // it is a part of a status effect's lifetime, and it is keyed
                // to a status instance in three separate places: the command's
                // status_instance_id comes from provenance, which only
                // prepare_status_lifecycle ever fills; the batch preflight
                // rejects an id of 0; applying it requires a matching *active*
                // status on the target; and expiry removes it by that same id.
                // An entity trigger has no status instance in scope, so it can
                // satisfy none of that.
                //
                // Accepting it here would let a catalog load a trigger that can
                // only fail at runtime -- and fail the whole batch with it,
                // since the preflight is all-or-nothing, so the other actions
                // in the same graph would silently stop working too. Rejecting
                // it at load instead matches the catalog loader, which already
                // refuses apply_speed_modifier outside a status on_apply.
                // "Collision slows the target" is authored as on_collision ->
                // apply_status, and that status's on_apply -> speed modifier.
                if (action.action_type ==
                    KernelEntityTriggerActionType_SpawnEntity) {
                    if (action.spawn_entity_template_id == 0u ||
                        action.position_source !=
                            KernelEventVec3Source_Position ||
                        action.owner_source >
                            KernelEntityRefSource_EventInstigator) {
                        return false;
                    }
                    continue;
                }
                // Same three checks the projectile-trigger validator makes;
                // entity-backed triggers accept this action too, and the
                // catalog loader has always compiled it for them
                // (gameplay_config.cc's entity trigger path). Missing here, the
                // loader accepted a prop that spawns a projectile and the
                // kernel then rejected the whole catalog -- which is a load
                // failure for every template, not just the offending one. That
                // is the third time these two tables have drifted apart; see
                // entity_trigger_action_parity_test.
                if (action.action_type ==
                    KernelEntityTriggerActionType_SpawnProjectile) {
                    if (action.spawn_projectile_template_id == 0u ||
                        action.position_source !=
                            KernelEventVec3Source_Position ||
                        action.direction_source !=
                            KernelEventVec3Source_Direction) {
                        return false;
                    }
                    continue;
                }
                return false;
            }
        }
        if ((entity_template.collision_trigger_mask &
             ~(KERNEL_COLLISION_MASK_ACTOR |
               KERNEL_COLLISION_MASK_STATIC_WORLD |
               KERNEL_COLLISION_LAYER_LIMB)) != 0u ||
            (entity_template.collision_trigger.struct_size == 0u &&
             entity_template.collision_trigger_mask != 0u)) {
            return false;
        }
        if (entity_template.entity_type == KernelEntityType_Director &&
            ((entity_template.component_flags &
             KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME) == 0u ||
             entity_template.ai.controller_type != KernelAiControllerType_Director ||
             entity_template.ai.tick_interval == 0u ||
             entity_template.ai.director_kind > KernelDirectorKind_GameRule ||
             (entity_template.ai.director_kind != KernelDirectorKind_GameRule &&
              entity_template.ai.spawn_actor_template_id == 0u &&
              entity_template.ai.spawn_entity_template_id == 0u) ||
             (entity_template.ai.director_kind != KernelDirectorKind_GameRule &&
              entity_template.ai.game_rule_definition_id != 0u) ||
             (entity_template.ai.director_kind == KernelDirectorKind_GameRule &&
              (entity_template.ai.game_rule_definition_id == 0u ||
               entity_template.ai.spawn_target_count != 0u ||
               entity_template.ai.spawn_entity_template_id != 0u ||
               entity_template.ai.spawn_actor_template_id != 0u ||
               entity_template.ai.spawn_position.x != 0.0f ||
               entity_template.ai.spawn_position.y != 0.0f ||
               entity_template.ai.spawn_position.z != 0.0f ||
               entity_template.ai.spawn_radius != 0.0f ||
               entity_template.ai.spawn_seed != 0u)))) {
            return false;
        }
        if (entity_template.entity_type != KernelEntityType_Prop &&
            (entity_template.prop.lifetime_ticks != 0u ||
             entity_template.prop.population_group_id != 0u)) {
            return false;
        }
        if (entity_template.entity_type == KernelEntityType_Prop &&
            entity_template.prop.struct_size != 0u) {
            const KernelPropInteractionDefinition& interaction =
                entity_template.prop.interaction;
            if (entity_template.prop.struct_size < sizeof(KernelPropDefinition) ||
                interaction.struct_size <
                    sizeof(KernelPropInteractionDefinition)) {
                return false;
            }
            constexpr std::uint32_t kPurePropCapabilities =
                KernelItemCapability_Carryable |
                KernelItemCapability_Throwable |
                KernelItemCapability_Interactable;
            if ((interaction.capability_flags & ~kPurePropCapabilities) != 0u) {
                return false;
            }
            constexpr std::uint32_t kRangedPropCapabilities =
                KernelItemCapability_Carryable |
                KernelItemCapability_Interactable;
            if (((interaction.capability_flags & kRangedPropCapabilities) != 0u &&
                 interaction.interaction_range <= 0.0f) ||
                ((interaction.capability_flags &
                  KernelItemCapability_Interactable) != 0u &&
                 entity_template.activated_trigger.struct_size <
                     sizeof(KernelActionTriggerDefinition))) {
                return false;
            }
            const bool throwable =
                (interaction.capability_flags &
                 KernelItemCapability_Throwable) != 0u;
            if (throwable !=
                (entity_template.prop
                     .throw_trajectory_projectile_template_id != 0u)) {
                return false;
            }
            if (entity_template.prop.population_group_id != 0u &&
                std::none_of(
                    validated_prop_population_rules.begin(),
                    validated_prop_population_rules.end(),
                    [&](const KernelPropPopulationRuleDefinition& rule) {
                        return rule.population_group_id ==
                            entity_template.prop.population_group_id;
                    })) {
                return false;
            }
        }
        validated_entity_templates.push_back(entity_template);
    }
    if (catalog.game_rule_node_count != 0u) {
        validated_game_rule_nodes.assign(
            catalog.game_rule_nodes,
            catalog.game_rule_nodes + catalog.game_rule_node_count);
    }
    if (catalog.game_rule_edge_count != 0u) {
        validated_game_rule_edges.assign(
            catalog.game_rule_edges,
            catalog.game_rule_edges + catalog.game_rule_edge_count);
    }
    if (catalog.game_rule_effect_count != 0u) {
        validated_game_rule_effects.assign(
            catalog.game_rule_effects,
            catalog.game_rule_effects + catalog.game_rule_effect_count);
    }
    for (std::uint32_t index = 0u; index < catalog.game_rule_count; ++index) {
        const KernelGameRuleDefinition& rule = catalog.game_rules[index];
        if (rule.struct_size < sizeof(KernelGameRuleDefinition) ||
            rule.game_rule_definition_id == 0u || rule.node_count == 0u ||
            rule.node_count > KERNEL_MAX_GAME_RULE_NODES ||
            rule.edge_count > KERNEL_MAX_GAME_RULE_EDGES ||
            rule.effect_count > KERNEL_MAX_GAME_RULE_EFFECTS ||
            rule.first_node > catalog.game_rule_node_count ||
            rule.node_count > catalog.game_rule_node_count - rule.first_node ||
            rule.first_edge > catalog.game_rule_edge_count ||
            rule.edge_count > catalog.game_rule_edge_count - rule.first_edge ||
            rule.first_effect > catalog.game_rule_effect_count ||
            rule.effect_count >
                catalog.game_rule_effect_count - rule.first_effect ||
            std::any_of(
                validated_game_rules.begin(),
                validated_game_rules.end(),
                [&](const KernelGameRuleDefinition& candidate) {
                    return candidate.game_rule_definition_id ==
                        rule.game_rule_definition_id;
                })) {
            return false;
        }
        std::unordered_set<std::uint32_t> node_ids;
        std::unordered_set<std::uint32_t> group_ids;
        std::uint32_t group_condition_count = 0u;
        for (std::uint32_t offset = 0u; offset < rule.node_count; ++offset) {
            const KernelGameRuleNodeDefinition& node =
                catalog.game_rule_nodes[rule.first_node + offset];
            const bool group_condition =
                node.condition_type ==
                KernelGameRuleConditionType_GroupEliminated;
            const bool player_count_condition =
                node.condition_type ==
                KernelGameRuleConditionType_PlayerCountAtLeast;
            if (node.struct_size < sizeof(KernelGameRuleNodeDefinition) ||
                node.node_id == 0u ||
                !node_ids.insert(node.node_id).second ||
                (!group_condition && !player_count_condition) ||
                (group_condition &&
                 (node.condition_group_id == 0u ||
                  node.condition_count != 0u ||
                  !group_ids.insert(node.condition_group_id).second)) ||
                (player_count_condition &&
                 (node.condition_group_id != 0u ||
                  node.condition_count == 0u))) {
                return false;
            }
            if (group_condition) {
                ++group_condition_count;
            }
        }
        std::unordered_set<std::uint64_t> edge_keys;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
        for (std::uint32_t offset = 0u; offset < rule.edge_count; ++offset) {
            const KernelGameRuleEdgeDefinition& edge =
                catalog.game_rule_edges[rule.first_edge + offset];
            const std::uint64_t key =
                (static_cast<std::uint64_t>(edge.source_node_id) << 32u) |
                edge.target_node_id;
            if (edge.struct_size < sizeof(KernelGameRuleEdgeDefinition) ||
                edge.source_node_id == edge.target_node_id ||
                !node_ids.contains(edge.source_node_id) ||
                !node_ids.contains(edge.target_node_id) ||
                !edge_keys.insert(key).second) {
                return false;
            }
            outgoing[edge.source_node_id].push_back(edge.target_node_id);
        }
        std::unordered_map<std::uint32_t, std::uint8_t> visit;
        const std::function<bool(std::uint32_t)> has_cycle =
            [&](std::uint32_t node_id) {
                std::uint8_t& state = visit[node_id];
                if (state == 1u) {
                    return true;
                }
                if (state == 2u) {
                    return false;
                }
                state = 1u;
                for (const std::uint32_t target : outgoing[node_id]) {
                    if (has_cycle(target)) {
                        return true;
                    }
                }
                state = 2u;
                return false;
            };
        for (const std::uint32_t node_id : node_ids) {
            if (has_cycle(node_id)) {
                return false;
            }
        }
        std::unordered_set<std::uint32_t> effect_node_ids;
        std::unordered_set<std::uint32_t> effect_group_ids;
        for (std::uint32_t offset = 0u; offset < rule.effect_count; ++offset) {
            const KernelGameRuleSpawnGroupEffectDefinition& effect =
                catalog.game_rule_effects[rule.first_effect + offset];
            const auto node = std::find_if(
                catalog.game_rule_nodes + rule.first_node,
                catalog.game_rule_nodes + rule.first_node + rule.node_count,
                [&](const KernelGameRuleNodeDefinition& candidate) {
                    return candidate.node_id == effect.node_id;
                });
            const auto entity = std::find_if(
                validated_entity_templates.begin(),
                validated_entity_templates.end(),
                [&](const KernelEntityTemplateDefinition& candidate) {
                    return candidate.entity_template_id ==
                        effect.entity_template_id;
                });
            if (effect.struct_size <
                    sizeof(KernelGameRuleSpawnGroupEffectDefinition) ||
                effect.effect_type != KernelGameRuleEffectType_SpawnGroup ||
                effect.count == 0u ||
                !effect_node_ids.insert(effect.node_id).second ||
                !effect_group_ids.insert(effect.group_id).second || node ==
                    catalog.game_rule_nodes + rule.first_node + rule.node_count ||
                node->condition_type !=
                    KernelGameRuleConditionType_GroupEliminated ||
                effect.group_id != node->condition_group_id ||
                entity == validated_entity_templates.end() ||
                entity->entity_type != KernelEntityType_Actor ||
                entity->actor_type != KernelActorType_Agent ||
                !std::isfinite(effect.position.x) ||
                !std::isfinite(effect.position.y) ||
                !std::isfinite(effect.position.z) ||
                !std::isfinite(effect.radius) || effect.radius < 0.0f) {
                return false;
            }
        }
        if (effect_node_ids.size() != group_condition_count) {
            return false;
        }
        validated_game_rules.push_back(rule);
    }
    for (const KernelEntityTemplateDefinition& entity_template :
         validated_entity_templates) {
        if (entity_template.entity_type == KernelEntityType_Director &&
            entity_template.ai.director_kind == KernelDirectorKind_GameRule &&
            std::none_of(
                validated_game_rules.begin(),
                validated_game_rules.end(),
                [&](const KernelGameRuleDefinition& rule) {
                    return rule.game_rule_definition_id ==
                        entity_template.ai.game_rule_definition_id;
                })) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < catalog.projectile_template_count; ++index) {
        const KernelProjectileTemplateDefinition& projectile_template =
            catalog.projectile_templates[index];
        if (projectile_template.struct_size <
                sizeof(KernelProjectileTemplateDefinition) ||
            projectile_template.projectile_template_id == 0 ||
            !validate_projectile_mechanics(projectile_template.mechanics)) {
            return false;
        }
        validated_projectile_templates.push_back(projectile_template);
    }
    for (std::uint32_t index = 0; index < catalog.collider_template_count; ++index) {
        const KernelColliderTemplateDefinition& collider_template =
            catalog.collider_templates[index];
        if (collider_template.struct_size <
                sizeof(KernelColliderTemplateDefinition) ||
            collider_template.template_id == 0 ||
            collider_template.shape_type > KernelColliderShapeType_Capsule ||
            (collider_template.shape_type == KernelColliderShapeType_Aabb &&
             (collider_template.shape_params.x <= 0.0f ||
              collider_template.shape_params.y <= 0.0f ||
              collider_template.shape_params.z <= 0.0f)) ||
            (collider_template.shape_type == KernelColliderShapeType_OrientedBox &&
             (collider_template.shape_params.x <= 0.0f ||
              collider_template.shape_params.y <= 0.0f ||
              collider_template.shape_params.z <= 0.0f)) ||
            (collider_template.shape_type == KernelColliderShapeType_Sphere &&
             collider_template.shape_params.x <= 0.0f) ||
            // A segment carries only an optional thickness. Its reach used to
            // be validated here as shape_params.x, back when the template
            // authored a `length` nothing read.
            (collider_template.shape_type == KernelColliderShapeType_Segment &&
             collider_template.shape_params.y < 0.0f) ||
            (collider_template.shape_type == KernelColliderShapeType_Cone &&
             ((collider_template.purpose_flags & KernelColliderPurpose_Vision) == 0u ||
              collider_template.shape_params.x <= 0.0f ||
              collider_template.shape_params.y <= 0.0f ||
              collider_template.shape_params.y > 360.0f)) ||
            (collider_template.shape_type == KernelColliderShapeType_Capsule &&
             (collider_template.shape_params.x <= 0.0f ||
              collider_template.shape_params.y <= 0.0f ||
              collider_template.lifetime_ticks != 0u ||
              (collider_template.purpose_flags &
               KernelColliderPurpose_Movement) == 0u))) {
            return false;
        }
        validated_collider_templates.push_back(collider_template);
    }
    for (const KernelProjectileTemplateDefinition& projectile_template :
         validated_projectile_templates) {
        const KernelProjectileMechanicsDefinition& mechanics =
            projectile_template.mechanics;
        const KernelColliderTemplateDefinition* projectile_collider =
            find_collider_template(
                validated_collider_templates,
                mechanics.collider_template_id);
        if (projectile_collider == nullptr ||
            projectile_collider->shape_type == KernelColliderShapeType_Cone ||
            ((mechanics.projectile_impact_trigger.struct_size != 0u ||
              mechanics.expired_trigger.struct_size != 0u) &&
             projectile_template_has_trigger_cycle(
                 validated_projectile_templates,
                 projectile_template.projectile_template_id)) ||
            !projectile_trigger_is_valid(
                mechanics.projectile_impact_trigger,
                validated_projectile_templates) ||
            !projectile_trigger_is_valid(
                mechanics.expired_trigger,
                validated_projectile_templates)) {
            return false;
        }
    }
    for (const KernelActorTemplateDefinition& actor_template :
         validated_actor_templates) {
        if (find_collider_template(
                validated_collider_templates,
                actor_template.collider_template_id) == nullptr) {
            return false;
        }
        if (actor_template.vision.vision_collider_template_id != 0u) {
            const KernelColliderTemplateDefinition* vision_collider =
                find_collider_template(
                    validated_collider_templates,
                    actor_template.vision.vision_collider_template_id);
            if (vision_collider == nullptr ||
                vision_collider->shape_type != KernelColliderShapeType_Cone ||
                (vision_collider->purpose_flags &
                 KernelColliderPurpose_Vision) == 0u) {
                return false;
            }
        }
    }
    for (const KernelEntityTemplateDefinition& entity_template :
         validated_entity_templates) {
        if (entity_template.movement.struct_size <
                sizeof(KernelMovementDefinition) ||
            entity_template.movement.controller_type >
                KernelMovementControllerType_Character ||
            (entity_template.entity_type == KernelEntityType_Actor &&
             entity_template.movement.controller_type ==
                 KernelMovementControllerType_None) ||
            // Zero is "engine default"; anything else must name movement layers
            // and nothing else. An unknown bit here would silently widen or
            // narrow what stops the actor.
            (entity_template.movement.movement_collision_mask &
             ~KERNEL_MOVEMENT_MASK_SUPPORTED) != 0u) {
            return false;
        }
        if (entity_template.movement.controller_type !=
            KernelMovementControllerType_None) {
            const KernelColliderTemplateDefinition* movement_collider =
                find_collider_template(
                    validated_collider_templates,
                    entity_template.movement.movement_collider_template_id);
            if (movement_collider == nullptr ||
                movement_collider->shape_type != KernelColliderShapeType_Capsule ||
                movement_collider->lifetime_ticks != 0u ||
                (movement_collider->purpose_flags &
                 KernelColliderPurpose_Movement) == 0u) {
                return false;
            }
        }
        if (entity_template.actor_template_id != 0u &&
            find_actor_template(
                validated_actor_templates,
                entity_template.actor_template_id) == nullptr) {
            return false;
        }
        if (entity_template.ai.spawn_actor_template_id != 0u &&
            find_actor_template(
                validated_actor_templates,
                entity_template.ai.spawn_actor_template_id) == nullptr) {
            return false;
        }
        if (entity_template.ai.spawn_entity_template_id != 0u &&
            find_entity_template(
                validated_entity_templates,
                entity_template.ai.spawn_entity_template_id) == nullptr) {
            return false;
        }
        for (const KernelActionTriggerDefinition* trigger : {
                 &entity_template.activated_trigger,
                 &entity_template.collision_trigger,
                 &entity_template.health_depleted_trigger,
                 &entity_template.destroy_entity_trigger,
             }) {
            const std::uint32_t count = trigger->action_count == 0u
                ? (trigger->action_type == KernelEntityTriggerActionType_None
                       ? 0u
                       : 1u)
                : trigger->action_count;
            for (std::uint32_t action_index = 0;
                 action_index < count;
                 ++action_index) {
                const std::uint8_t action_type = trigger->action_count == 0u
                    ? trigger->action_type
                    : trigger->actions[action_index].action_type;
                const std::uint32_t entity_template_id =
                    trigger->action_count == 0u
                    ? trigger->spawn_entity_template_id
                    : trigger->actions[action_index]
                          .spawn_entity_template_id;
                if (action_type == KernelEntityTriggerActionType_SpawnEntity &&
                    find_entity_template(
                        validated_entity_templates,
                        entity_template_id) == nullptr) {
                    return false;
                }
            }
        }
        if (entity_template.collider_template_id != 0u &&
            find_collider_template(
                validated_collider_templates,
                entity_template.collider_template_id) == nullptr) {
            return false;
        }
        if (entity_template.entity_type == KernelEntityType_Prop &&
            (entity_template.prop.interaction.capability_flags &
             KernelItemCapability_Throwable) != 0u &&
            (!valid_throw_trajectory(
                 validated_projectile_templates,
                 entity_template.prop
                     .throw_trajectory_projectile_template_id) ||
             !valid_throw_collider(find_collider_template(
                 validated_collider_templates,
                 entity_template.collider_template_id)))) {
            return false;
        }
    }
    for (const KernelItemTemplateDefinition& item_template :
         validated_item_templates) {
        if (item_template.throw_policy.mode ==
                KernelItemThrowMode_IdentityPreserving &&
            !valid_throw_trajectory(
                validated_projectile_templates,
                item_template.throw_policy
                    .trajectory_projectile_template_id)) {
            return false;
        }
        const KernelActionTriggerDefinition& item_trigger =
            item_template.item_used_trigger;
        const std::uint32_t item_action_count = item_trigger.action_count == 0u
            ? (item_trigger.action_type == KernelEntityTriggerActionType_None
                   ? 0u
                   : 1u)
            : item_trigger.action_count;
        for (std::uint32_t index = 0; index < item_action_count; ++index) {
            KernelActionDefinition action{};
            if (item_trigger.action_count == 0u) {
                action.action_type = item_trigger.action_type;
                action.spawn_entity_template_id =
                    item_trigger.spawn_entity_template_id;
                action.spawn_projectile_template_id =
                    item_trigger.spawn_projectile_template_id;
            } else {
                action = item_trigger.actions[index];
            }
            if ((action.action_type ==
                     KernelEntityTriggerActionType_SpawnEntity &&
                 find_entity_template(
                     validated_entity_templates,
                     action.spawn_entity_template_id) == nullptr) ||
                (action.action_type ==
                     KernelEntityTriggerActionType_SpawnProjectile &&
                 find_projectile_template(
                     validated_projectile_templates,
                     action.spawn_projectile_template_id) == nullptr)) {
                return false;
            }
        }
        const bool has_health_projection = std::any_of(
            item_template.portable_state_fields,
            item_template.portable_state_fields +
                item_template.portable_state_field_count,
            [](const KernelPortableStateFieldDefinition& field) {
                return field.world_projection ==
                    KernelPortableStateProjection_HealthCurrent;
            });
        if (item_template.entity_template_id == 0u) {
            if (has_health_projection) {
                return false;
            }
            continue;
        }
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(
                validated_entity_templates,
                item_template.entity_template_id);
        if (entity_template == nullptr ||
            entity_template->entity_type != KernelEntityType_Prop) {
            return false;
        }
        for (std::uint32_t index = 0;
             index < item_template.portable_state_field_count;
             ++index) {
            const KernelPortableStateFieldDefinition& field =
                item_template.portable_state_fields[index];
            if (field.world_projection !=
                KernelPortableStateProjection_HealthCurrent) {
                continue;
            }
            if (item_template.item_mode != KernelItemMode_Stateful ||
                (entity_template->component_flags &
                 KERNEL_ENTITY_COMPONENT_HEALTH) == 0u ||
                field.uint32_default > entity_template->combat.max_hp) {
                return false;
            }
        }
        const KernelPropInteractionDefinition& interaction =
            entity_template->prop.interaction;
        if (interaction.capability_flags != 0u ||
            entity_template->prop
                    .throw_trajectory_projectile_template_id != 0u ||
            entity_template->prop.lifetime_ticks != 0u ||
            entity_template->prop.population_group_id != 0u) {
            return false;
        }
        if (item_template.throw_policy.mode ==
                KernelItemThrowMode_IdentityPreserving &&
            !valid_throw_collider(find_collider_template(
                validated_collider_templates,
                entity_template->collider_template_id))) {
            return false;
        }
        if ((item_template.capability_flags &
             KernelItemCapability_Interactable) != 0u) {
            if (entity_template->activated_trigger.struct_size <
                sizeof(KernelActionTriggerDefinition)) {
                return false;
            }
        }
    }
    std::vector<RuntimeProjectileTemplate> runtime_projectile_templates;
    runtime_projectile_templates.reserve(validated_projectile_templates.size());
    for (const KernelProjectileTemplateDefinition& projectile_template :
         validated_projectile_templates) {
        runtime_projectile_templates.push_back(to_runtime_projectile_template(
            projectile_template,
            find_collider_template(
                validated_collider_templates,
                projectile_template.mechanics.collider_template_id)));
    }
    std::vector<RuntimeActionTemplate> runtime_action_templates;
    runtime_action_templates.reserve(validated_action_templates.size());
    for (const KernelActionTemplateDefinition& action_template :
         validated_action_templates) {
        runtime_action_templates.push_back(RuntimeActionTemplate{
            action_template.action_template_id,
            action_template.trigger_mode,
            action_template.flags,
            action_template.ammo_cost_per_commit,
            action_template.commit_offset_ticks,
            action_template.commit_interval_ticks,
            action_template.max_commit_count,
            action_template.recovery_ticks,
            action_template.hold_input_timeout_ticks,
        });
    }
    std::vector<RuntimeStatusEffectTemplate> runtime_status_effect_templates;
    runtime_status_effect_templates.reserve(validated_status_effects.size());
    for (const KernelStatusEffectDefinition& status : validated_status_effects) {
        RuntimeStatusEffectTemplate runtime_status;
        runtime_status.status_effect_id = status.status_effect_id;
        runtime_status.channel_id = status.channel_id;
        runtime_status.duration_ticks = status.duration_ticks;
        runtime_status.interval_ticks = status.interval_ticks;
        runtime_status.replacement_policy = status.replacement_policy;
        runtime_status.max_stacks =
            status.replacement_policy ==
                    KernelStatusEffectReplacementPolicy_Stack
                ? status.max_stacks
                : 1u;
        runtime_status.refresh_on_stack = status.refresh_on_stack != 0u;
        if (status.on_apply_trigger.struct_size != 0u) {
            runtime_status.on_apply_binding = compile_action_trigger_definition(
                TriggerEventType::kStatusApplied, status.on_apply_trigger);
        }
        if (status.on_tick_trigger.struct_size != 0u) {
            runtime_status.on_tick_binding = compile_action_trigger_definition(
                TriggerEventType::kStatusTick, status.on_tick_trigger);
        }
        if (status.on_expire_trigger.struct_size != 0u) {
            runtime_status.on_expire_binding = compile_action_trigger_definition(
                TriggerEventType::kStatusExpired, status.on_expire_trigger);
        }
        runtime_status_effect_templates.push_back(std::move(runtime_status));
    }
    entity_templates_ = std::move(validated_entity_templates);
    actor_templates_ = std::move(validated_actor_templates);
    projectile_templates_ = std::move(validated_projectile_templates);
    collider_templates_ = std::move(validated_collider_templates);
    action_templates_ = std::move(validated_action_templates);
    item_templates_ = std::move(validated_item_templates);
    prop_population_rules_ =
        std::move(validated_prop_population_rules);
    game_rule_definitions_ = std::move(validated_game_rules);
    game_rule_nodes_ = std::move(validated_game_rule_nodes);
    game_rule_edges_ = std::move(validated_game_rule_edges);
    game_rule_effects_ = std::move(validated_game_rule_effects);
    skeleton_assets_ = std::move(validated_skeleton_assets);
    locomotion_states_.clear();
    skeleton_pose_history_.clear();
    follower_locomotion_states_.clear();
    pending_follower_steps_.clear();
    if (!item_store_.set_templates(item_templates_, &item_validation_error)) {
        return false;
    }
    world_.set_projectile_templates(runtime_projectile_templates);
    world_.set_action_templates(runtime_action_templates);
    world_.set_status_effect_templates(runtime_status_effect_templates);
    if (running_ &&
        (catalog_version_ != catalog.catalog_version ||
         catalog_hash_ != catalog.catalog_hash)) {
        clear_client_action_sync_state();
    }
    catalog_version_ = catalog.catalog_version;
    catalog_hash_ = catalog.catalog_hash;
    return true;
}

bool KernelEngine::load_gameplay_catalog_with_static_collision_scene(
    const KernelGameplayCatalogDefinition& catalog,
    const KernelStaticCollisionSceneConfig& scene_config,
    bool* out_static_scene_rejected) {
    *out_static_scene_rejected = false;
    std::vector<std::uint8_t> scene;
    if (!prepare_static_collision_scene(scene_config, &scene)) {
        *out_static_scene_rejected = true;
        return false;
    }
    if (!load_gameplay_catalog(catalog)) {
        return false;
    }
    commit_static_collision_scene(std::move(scene), scene_config);
    return true;
}

std::uint32_t KernelEngine::get_render_states(
    RenderEntityState* out_states,
    std::uint32_t max_states) {
    return get_render_states_at_time(client_local_time_us_, out_states, max_states);
}

std::uint32_t KernelEngine::get_render_states_at_time(
    std::uint64_t client_render_time_us,
    RenderEntityState* out_states,
    std::uint32_t max_states) {
    if (out_states == nullptr || max_states == 0) {
        return 0;
    }
    rebuild_render_states_at_time(client_render_time_us);
    const std::uint32_t count =
        std::min(max_states, static_cast<std::uint32_t>(render_states_.size()));
    std::memcpy(out_states, render_states_.data(), sizeof(RenderEntityState) * count);
    return count;
}

std::uint32_t KernelEngine::get_skeleton_render_states(
    KernelSkeletonRenderState* out_states,
    std::uint32_t max_states,
    KernelBoneLocalTransform* out_bone_transforms,
    std::uint32_t max_bone_transforms,
    KernelSkeletonRenderStateResult* out_result) {
    rebuild_skeleton_presentation_at_time(client_local_time_us_);
    return copy_skeleton_render_states(
        skeleton_presentation_poses_,
        0u,
        client_local_time_us_,
        out_states,
        max_states,
        out_bone_transforms,
        max_bone_transforms,
        out_result);
}

std::uint32_t KernelEngine::get_skeleton_render_states_at_time(
    std::uint64_t client_render_time_us,
    KernelSkeletonRenderState* out_states,
    std::uint32_t max_states,
    KernelBoneLocalTransform* out_bone_transforms,
    std::uint32_t max_bone_transforms,
    KernelSkeletonRenderStateResult* out_result) {
    rebuild_skeleton_presentation_at_time(client_render_time_us);
    return copy_skeleton_render_states(
        skeleton_presentation_poses_,
        KERNEL_SKELETON_RENDER_RESULT_FLAG_AT_TIME,
        client_render_time_us,
        out_states,
        max_states,
        out_bone_transforms,
        max_bone_transforms,
        out_result);
}

std::uint32_t KernelEngine::get_skeleton_bind_pose(
    std::uint32_t skeleton_asset_id,
    std::uint64_t skeleton_content_hash,
    KernelBoneLocalTransform* out_bone_transforms,
    std::uint32_t max_bone_transforms) {
    const RuntimeSkeletonAsset* asset =
        find_skeleton_asset(skeleton_assets_, skeleton_asset_id);
    if (asset == nullptr ||
        asset->skeleton_content_hash != skeleton_content_hash) {
        return 0u;
    }
    const std::uint32_t bone_count =
        static_cast<std::uint32_t>(asset->bind_pose.size());
    if (out_bone_transforms != nullptr && max_bone_transforms != 0u) {
        std::copy_n(
            asset->bind_pose.begin(),
            std::min(bone_count, max_bone_transforms),
            out_bone_transforms);
    }
    return bone_count;
}

std::uint32_t KernelEngine::poll_events(KernelEvent* out_events, std::uint32_t max_events) {
    if (out_events == nullptr || max_events == 0) {
        return 0;
    }
    release_presentable_events();
    const std::uint32_t count =
        std::min(max_events, static_cast<std::uint32_t>(events_.size()));
    std::memcpy(out_events, events_.data(), sizeof(KernelEvent) * count);
    events_.erase(events_.begin(), events_.begin() + count);
    return count;
}

std::uint32_t KernelEngine::poll_entity_lifecycle_events(
    KernelEntityLifecycleEvent* out_events,
    std::uint32_t max_events) {
    if (out_events == nullptr || max_events == 0) {
        return 0;
    }
    const std::uint32_t count =
        std::min(max_events, static_cast<std::uint32_t>(lifecycle_events_.size()));
    std::memcpy(
        out_events,
        lifecycle_events_.data(),
        sizeof(KernelEntityLifecycleEvent) * count);
    lifecycle_events_.erase(lifecycle_events_.begin(), lifecycle_events_.begin() + count);
    return count;
}

std::uint32_t KernelEngine::poll_local_action_results(
    KernelLocalActionResult* out_results,
    std::uint32_t max_results) {
    if (out_results == nullptr || max_results == 0) {
        return 0;
    }
    const std::uint32_t count = std::min(
        max_results,
        static_cast<std::uint32_t>(local_action_results_.size()));
    std::memcpy(
        out_results,
        local_action_results_.data(),
        sizeof(KernelLocalActionResult) * count);
    local_action_results_.erase(
        local_action_results_.begin(),
        local_action_results_.begin() + count);
    return count;
}

std::uint32_t KernelEngine::poll_remote_action_presentation_events(
    KernelRemoteActionPresentationEvent* out_events,
    std::uint32_t max_events) {
    if (out_events == nullptr || max_events == 0) {
        return 0;
    }
    release_remote_action_presentation_events();
    const std::uint32_t count = std::min(
        max_events,
        static_cast<std::uint32_t>(remote_action_presentation_events_.size()));
    std::memcpy(
        out_events,
        remote_action_presentation_events_.data(),
        sizeof(KernelRemoteActionPresentationEvent) * count);
    remote_action_presentation_events_.erase(
        remote_action_presentation_events_.begin(),
        remote_action_presentation_events_.begin() + count);
    return count;
}

std::uint32_t KernelEngine::query_status_effects(
    NetId entity_net_id,
    KernelStatusEffectView* out_effects,
    std::uint32_t max_effects) const {
    const StatusEffectState* state = nullptr;
    if (is_server_mode(config_.mode)) {
        const std::optional<entt::entity> entity = world_.find_entity(entity_net_id);
        if (entity.has_value()) {
            state = world_.registry().try_get<StatusEffectState>(*entity);
        }
    } else {
        const auto found = client_status_effect_states_.find(entity_net_id);
        if (found != client_status_effect_states_.end()) {
            state = &found->second;
        }
    }
    if (state == nullptr) {
        return 0u;
    }
    std::vector<const ActiveStatusEffect*> sorted;
    sorted.reserve(state->active.size());
    for (const ActiveStatusEffect& active : state->active) {
        sorted.push_back(&active);
    }
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const ActiveStatusEffect* lhs, const ActiveStatusEffect* rhs) {
            return lhs->instance_id < rhs->instance_id;
        });
    if (out_effects != nullptr) {
        const std::uint32_t write_count = std::min<std::uint32_t>(
            max_effects, static_cast<std::uint32_t>(sorted.size()));
        for (std::uint32_t index = 0u; index < write_count; ++index) {
            const ActiveStatusEffect& active = *sorted[index];
            out_effects[index] = KernelStatusEffectView{
                sizeof(KernelStatusEffectView),
                active.status_effect_id,
                active.instance_id,
                active.channel_id,
                active.source,
                active.applied_tick,
                active.expire_tick,
                active.stack_count,
                static_cast<std::uint16_t>(
                    world_.find_status_effect_template(active.status_effect_id) != nullptr
                        ? world_.find_status_effect_template(active.status_effect_id)->max_stacks
                        : 1u),
            };
        }
    }
    return static_cast<std::uint32_t>(sorted.size());
}

bool KernelEngine::get_benchmark_stats(KernelBenchmarkStats* out_stats) const {
    if (out_stats == nullptr || out_stats->struct_size < sizeof(KernelBenchmarkStats)) {
        return false;
    }
    KernelBenchmarkStats stats = benchmark_stats_;
    stats.struct_size = sizeof(KernelBenchmarkStats);
    stats.catalog_version = catalog_version_;
    stats.catalog_hash = catalog_hash_;

    auto entity_view = world_.registry().view<NetworkIdentity, EntityKind>();
    stats.total_entity_count = static_cast<std::uint32_t>(entity_view.size_hint());
    stats.total_entity_count += static_cast<std::uint32_t>(std::count_if(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [](const PredictedProjectile& projectile) {
            return !projectile.locally_terminated;
        }));

    auto add_projectile_sync_mode = [&stats](std::uint8_t sync_mode) {
        ++stats.projectile_count;
        if (sync_mode == KernelProjectileSyncMode_LocalPredictedDeterministic) {
            ++stats.event_spawn_projectile_count;
        } else if (sync_mode == KernelProjectileSyncMode_ServerSnapshotOnly) {
            ++stats.snapshot_only_projectile_count;
        } else {
            ++stats.hybrid_projectile_count;
        }
    };
    auto projectile_view =
        world_.registry().view<NetworkIdentity, ProjectileState, ProjectileTag>();
    for (const entt::entity entity : projectile_view) {
        const ProjectileState& projectile = projectile_view.get<ProjectileState>(entity);
        std::uint8_t sync_mode =
            KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
        if (world_.registry().all_of<HomingState>(entity)) {
            sync_mode = to_kernel_projectile_sync_mode(
                world_.registry().get<HomingState>(entity).sync_mode);
        } else {
            const auto found = std::find_if(
                projectile_templates_.begin(),
                projectile_templates_.end(),
                [&projectile](const KernelProjectileTemplateDefinition& definition) {
                    return definition.weapon_id == projectile.weapon_id;
                });
            if (found != projectile_templates_.end()) {
                sync_mode = found->mechanics.sync_mode;
            }
        }
        add_projectile_sync_mode(sync_mode);
    }
    for (const PredictedProjectile& projectile : predicted_projectiles_) {
        if (projectile.locally_terminated) {
            continue;
        }
        add_projectile_sync_mode(projectile.sync_mode);
    }
    if (stats.projectile_count != 0) {
        const float total = static_cast<float>(stats.projectile_count);
        stats.event_spawn_ratio =
            static_cast<float>(stats.event_spawn_projectile_count) / total;
        stats.snapshot_only_ratio =
            static_cast<float>(stats.snapshot_only_projectile_count) / total;
        stats.hybrid_ratio =
            static_cast<float>(stats.hybrid_projectile_count) / total;
    }
    *out_stats = stats;
    return true;
}

bool KernelEngine::get_network_stats(KernelNetworkStats* out_stats) const {
    if (out_stats == nullptr || out_stats->struct_size < sizeof(KernelNetworkStats)) {
        return false;
    }
    KernelNetworkStats stats = network_stats_;
    stats.struct_size = sizeof(KernelNetworkStats);
    const std::uint64_t total_bytes =
        stats.reliable_bytes_sent + stats.unreliable_bytes_sent;
    if (stats.packet_count_sent != 0) {
        stats.average_packet_size = static_cast<std::uint32_t>(
            total_bytes / stats.packet_count_sent);
    }
    if (stats.local_action_result_batch_count != 0) {
        stats.average_local_action_result_batch_size =
            static_cast<std::uint32_t>(
                stats.local_action_result_batch_record_count /
                stats.local_action_result_batch_count);
    }
    if (stats.remote_presentation_batch_count != 0) {
        stats.average_remote_presentation_batch_size =
            static_cast<std::uint32_t>(
                stats.remote_presentation_batch_record_count /
                stats.remote_presentation_batch_count);
    }
    *out_stats = stats;
    return true;
}

bool KernelEngine::network_stats_enabled() const {
    return config_.network_stats.mode != KernelNetworkStatsMode_Off;
}

bool KernelEngine::detailed_network_stats_enabled() const {
    return config_.network_stats.mode == KernelNetworkStatsMode_Detailed;
}

void KernelEngine::refill_byte_token_bucket(
    ByteTokenBucket* bucket,
    std::uint64_t bytes_per_second,
    std::uint64_t now_us) const {
    if (bucket == nullptr || bytes_per_second == 0) {
        return;
    }
    if (!bucket->initialized) {
        bucket->tokens = bytes_per_second;
        bucket->last_refill_time_us = now_us;
        bucket->initialized = true;
        return;
    }
    if (now_us <= bucket->last_refill_time_us) {
        return;
    }
    const std::uint64_t elapsed_us = now_us - bucket->last_refill_time_us;
    const std::uint64_t refill_numerator =
        elapsed_us * bytes_per_second + bucket->refill_remainder;
    const std::uint64_t refill_bytes = refill_numerator / UINT64_C(1000000);
    bucket->refill_remainder = refill_numerator % UINT64_C(1000000);
    bucket->tokens = std::min(bytes_per_second, bucket->tokens + refill_bytes);
    bucket->last_refill_time_us = now_us;
}

std::uint32_t KernelEngine::poll_debug_records(
    const KernelDebugRecordFilter* filter,
    KernelDebugInfo* out_records,
    std::uint32_t max_records) {
    if (out_records == nullptr || max_records == 0 ||
        (filter != nullptr &&
         filter->struct_size < sizeof(KernelDebugRecordFilter))) {
        return 0;
    }
    std::uint32_t copied = 0;
    auto cursor = debug_records_.begin();
    while (cursor != debug_records_.end() && copied < max_records) {
        if (!debug_filter_matches(filter, *cursor)) {
            ++cursor;
            continue;
        }
        out_records[copied] = *cursor;
        out_records[copied].struct_size = sizeof(KernelDebugInfo);
        ++copied;
        cursor = debug_records_.erase(cursor);
    }
    return copied;
}

std::uint32_t KernelEngine::query_collider_shapes(
    const KernelColliderShapeQuery* query,
    KernelColliderShapeView* out_shapes,
    std::uint32_t max_shapes) const {
    if (out_shapes == nullptr || max_shapes == 0 ||
        (query != nullptr &&
         query->struct_size < sizeof(KernelColliderShapeQuery))) {
        return 0;
    }

    std::uint32_t copied = 0;
    for (const ColliderInstance& collider :
         world_.collider_registry().instances()) {
        if (copied >= max_shapes) {
            break;
        }
        if (query != nullptr) {
            if (query->entity_net_id != 0 &&
                query->entity_net_id != collider.entity_net_id) {
                continue;
            }
            if (query->entity_type_filter != 0 &&
                query->entity_type_filter !=
                    static_cast<std::uint16_t>(collider.entity_type)) {
                continue;
            }
            if (query->actor_type_filter != 0 &&
                query->actor_type_filter !=
                    static_cast<std::uint16_t>(collider.actor_type)) {
                continue;
            }
            if (query->purpose_mask != 0 &&
                (collider.purpose_flags & query->purpose_mask) == 0) {
                continue;
            }
        }

        KernelColliderShapeView shape{};
        shape.struct_size = sizeof(KernelColliderShapeView);
        shape.entity_net_id = collider.entity_net_id;
        shape.entity_type = static_cast<std::uint16_t>(collider.entity_type);
        shape.actor_type = static_cast<std::uint16_t>(collider.actor_type);
        shape.collider_template_id = collider.collider_template_id;
        shape.shape_type = to_kernel_collider_shape_type(collider.shape_type);
        shape.world_center = to_kernel_vec3(collider.world_center);
        shape.shape_params = collider_instance_shape_params(collider);
        shape.purpose_flags = collider.purpose_flags;
        shape.layer_mask = collider.layer_mask;
        shape.collider_id = collider.collider_id;
        shape.owner_net_id = collider.owner_net_id;
        shape.world_rotation = to_kernel_quat(collider.world_rotation);
        shape.segment_start = to_kernel_vec3(collider.segment_start);
        shape.segment_end = to_kernel_vec3(collider.segment_end);
        shape.lifetime_ticks = collider.lifetime_ticks;
        shape.remaining_ticks = collider.remaining_ticks;
        shape.has_resolved_damage = collider.has_resolved_damage ? 1u : 0u;
        out_shapes[copied++] = shape;
    }
    return copied;
}

std::uint32_t KernelEngine::query_vision_state(
    const KernelVisionStateQuery* query,
    KernelVisionStateView* out_states,
    std::uint32_t max_states) const {
    if (out_states == nullptr || max_states == 0 ||
        (query != nullptr &&
         query->struct_size < sizeof(KernelVisionStateQuery))) {
        return 0;
    }

    std::uint32_t copied = 0;
    for (const auto& [agent_net_id, runtime_state] : vision_states_) {
        if (copied >= max_states) {
            break;
        }
        const KernelVisionStateView& view = runtime_state.view;
        if (view.valid == 0u) {
            continue;
        }
        if (query != nullptr) {
            if (query->agent_net_id != 0 &&
                query->agent_net_id != agent_net_id) {
                continue;
            }
            if (query->entity_type_filter != 0 &&
                query->entity_type_filter != view.entity_type) {
                continue;
            }
            if (query->actor_type_filter != 0 &&
                query->actor_type_filter != view.actor_type) {
                continue;
            }
        }
        out_states[copied] = view;
        out_states[copied].struct_size = sizeof(KernelVisionStateView);
        ++copied;
    }
    return copied;
}

std::uint32_t KernelEngine::get_projectile_templates(
    KernelProjectileTemplateDefinition* out_templates,
    std::uint32_t max_templates) const {
    const std::uint32_t count =
        static_cast<std::uint32_t>(projectile_templates_.size());
    if (out_templates == nullptr || max_templates == 0) {
        return count;
    }
    const std::uint32_t copied = std::min(count, max_templates);
    std::memcpy(
        out_templates,
        projectile_templates_.data(),
        sizeof(KernelProjectileTemplateDefinition) * copied);
    return copied;
}

bool KernelEngine::get_action_template(
    std::uint32_t action_template_id,
    KernelActionTemplateDefinition* out_definition) const {
    if (action_template_id == 0u || out_definition == nullptr ||
        out_definition->struct_size < sizeof(KernelActionTemplateDefinition)) {
        return false;
    }
    const KernelActionTemplateDefinition* definition =
        find_action_template(action_templates_, action_template_id);
    if (definition == nullptr) {
        return false;
    }
    *out_definition = *definition;
    out_definition->struct_size = sizeof(KernelActionTemplateDefinition);
    return true;
}

std::uint32_t KernelEngine::get_collider_templates(
    KernelColliderTemplateDefinition* out_templates,
    std::uint32_t max_templates) const {
    const std::uint32_t count =
        static_cast<std::uint32_t>(collider_templates_.size());
    if (out_templates == nullptr || max_templates == 0) {
        return count;
    }
    const std::uint32_t copied = std::min(count, max_templates);
    std::memcpy(
        out_templates,
        collider_templates_.data(),
        sizeof(KernelColliderTemplateDefinition) * copied);
    return copied;
}

std::uint32_t KernelEngine::get_actor_templates(
    KernelActorTemplateDefinition* out_templates,
    std::uint32_t max_templates) const {
    const std::uint32_t count =
        static_cast<std::uint32_t>(actor_templates_.size());
    if (out_templates == nullptr || max_templates == 0) {
        return count;
    }
    const std::uint32_t copied = std::min(count, max_templates);
    std::memcpy(
        out_templates,
        actor_templates_.data(),
        sizeof(KernelActorTemplateDefinition) * copied);
    return copied;
}

std::uint32_t KernelEngine::collider_template_id_for_actor_template(
    std::uint32_t actor_template_id) const {
    const KernelActorTemplateDefinition* actor_template =
        find_actor_template(actor_templates_, actor_template_id);
    return actor_template == nullptr ? 0u : actor_template->collider_template_id;
}

std::uint32_t KernelEngine::get_collider_bindings(
    KernelColliderBindingDefinition*,
    std::uint32_t) const {
    return 0;
}

void KernelEngine::materialize_entity_collider(NetId net_id) {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, EntityKind, Transform, Hitbox>(
            *entity)) {
        return;
    }

    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const EntityKind& kind = world_.registry().get<EntityKind>(*entity);
    if (kind.type == EntityType::kProjectile) {
        return;
    }
    const Transform& transform = world_.registry().get<Transform>(*entity);
    const Hitbox& hitbox = world_.registry().get<Hitbox>(*entity);
    if (hitbox.collider_template_id == 0) {
        return;
    }
    const KernelColliderTemplateDefinition* collider_template =
        find_collider_template(collider_templates_, hitbox.collider_template_id);
    if (collider_template == nullptr) {
        return;
    }

    ColliderInstance collider{};
    collider.collider_template_id = collider_template->template_id;
    collider.owner_net_id = identity.net_id;
    collider.entity_net_id = identity.net_id;
    collider.entity_type = kind.type;
    collider.actor_type = kind.actor_type;
    collider.shape_type = to_collider_shape_type(collider_template->shape_type);
    collider.purpose_flags = collider_template->purpose_flags;
    collider.layer_mask = collider_template->layer_mask;
    collider.hit_zone = hitbox.hit_zone;
    collider.local_center = from_kernel_vec3(collider_template->center);
    collider.local_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    collider.world_rotation = transform.rotation * collider.local_rotation;
    collider.world_center =
        transform.position + transform.rotation * collider.local_center;
    collider.half_extents = collider_template_half_extents(*collider_template);
    collider.radius = collider_template_radius(*collider_template);
    collider.world_bounds = collider_world_bounds(collider);
    world_.collider_registry().upsert_entity_collider(
        identity.net_id,
        collider_template->template_id,
        collider);
    materialize_entity_movement_collider(net_id);
}

void KernelEngine::materialize_entity_movement_collider(NetId net_id) {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<
            NetworkIdentity,
            EntityKind,
            Transform,
            MovementState>(*entity)) {
        return;
    }
    MovementState& movement =
        world_.registry().get<MovementState>(*entity);
    if (movement.controller_type == MovementState::ControllerType::kNone ||
        movement.movement_collider_template_id == 0u) {
        return;
    }
    const KernelColliderTemplateDefinition* collider_template =
        find_collider_template(
            collider_templates_, movement.movement_collider_template_id);
    if (collider_template == nullptr ||
        collider_template->shape_type != KernelColliderShapeType_Capsule ||
        (collider_template->purpose_flags & KernelColliderPurpose_Movement) == 0u) {
        return;
    }

    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const EntityKind& kind = world_.registry().get<EntityKind>(*entity);
    const Transform& transform = world_.registry().get<Transform>(*entity);
    ColliderInstance collider{};
    collider.collider_template_id = collider_template->template_id;
    collider.owner_net_id = identity.net_id;
    collider.entity_net_id = identity.net_id;
    collider.entity_type = kind.type;
    collider.actor_type = kind.actor_type;
    collider.shape_type = ColliderShapeType::kCapsule;
    collider.purpose_flags = collider_template->purpose_flags;
    collider.layer_mask = collider_template->layer_mask;
    collider.local_center = from_kernel_vec3(collider_template->center);
    collider.world_rotation = transform.rotation;
    collider.world_center =
        transform.position + transform.rotation * collider.local_center;
    collider.capsule_half_height = collider_template->shape_params.x;
    collider.radius = collider_template->shape_params.y;
    collider.world_bounds = collider_world_bounds(collider);
    ColliderInstance& stored = world_.collider_registry().upsert_entity_collider(
        identity.net_id,
        collider_template->template_id,
        collider);
    movement.movement_collider_id = stored.collider_id;
}

void KernelEngine::materialize_entity_limb_colliders(
    NetId net_id,
    const KernelSkeletonBindingDefinition& skeleton,
    const RuntimeSkeletonAsset& skeleton_asset,
    const LocomotionState& locomotion_state) {
    // A rig that declares no colliders, or a tick whose solve failed, leaves
    // whatever was registered before in place for exactly one tick and then
    // loses it below -- there is no half-posed limb to publish.
    if (skeleton.collider_count == 0u ||
        locomotion_state.solved_collider_poses.size() !=
            skeleton.collider_count) {
        world_.collider_registry().remove_bone_colliders(net_id);
        return;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, EntityKind>(*entity)) {
        world_.collider_registry().remove_bone_colliders(net_id);
        return;
    }
    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const EntityKind& kind = world_.registry().get<EntityKind>(*entity);

    for (std::uint32_t index = 0u; index < skeleton.collider_count; ++index) {
        world_.collider_registry().upsert_bone_collider(
            identity.net_id,
            /*collider_template_id=*/0u,
            skeleton.colliders[index].bone_index,
            make_limb_collider(
                identity.net_id,
                kind.type,
                kind.actor_type,
                skeleton.colliders[index],
                locomotion_state.solved_collider_poses[index],
                skeleton_asset.bind_pose,
                locomotion_state.last_root_position,
                locomotion_state.applied_root_rotation));
    }
}

void KernelEngine::materialize_projectile_collider(NetId net_id) {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<
            NetworkIdentity,
            EntityKind,
            Transform,
            ProjectileState>(*entity) ||
        world_.collider_registry().has_persistent_entity_collider(net_id)) {
        return;
    }

    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const EntityKind& kind = world_.registry().get<EntityKind>(*entity);
    const Transform& transform = world_.registry().get<Transform>(*entity);
    const ProjectileState& projectile =
        world_.registry().get<ProjectileState>(*entity);
    const KernelProjectileTemplateDefinition* projectile_template =
        find_projectile_template(
            projectile_templates_,
            projectile.projectile_template_id);
    if (projectile_template == nullptr) {
        return;
    }
    const KernelColliderTemplateDefinition* collider_template =
        find_collider_template(
            collider_templates_,
            projectile_template->mechanics.collider_template_id);
    if (collider_template == nullptr) {
        return;
    }

    ColliderInstance collider{};
    collider.collider_template_id = collider_template->template_id;
    collider.owner_net_id = projectile.shooter_net_id;
    collider.entity_net_id = identity.net_id;
    collider.entity_type = kind.type;
    collider.actor_type = kind.actor_type;
    collider.shape_type = to_collider_shape_type(collider_template->shape_type);
    collider.purpose_flags = collider_template->purpose_flags;
    collider.layer_mask = collider_template->layer_mask;
    collider.local_center = from_kernel_vec3(collider_template->center);
    collider.local_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    collider.world_rotation = transform.rotation;
    collider.world_center = transform.position + collider.local_center;
    collider.half_extents = collider_template_half_extents(*collider_template);
    collider.radius = collider_template_radius(*collider_template);
    if (world_.registry().all_of<ProjectileBeamRuntime>(*entity)) {
        apply_beam_collider_geometry(
            world_.registry().get<ProjectileBeamRuntime>(*entity),
            &collider);
    }
    collider.world_bounds = collider_world_bounds(collider);
    world_.collider_registry().upsert_entity_collider(
        identity.net_id,
        collider_template->template_id,
        collider);
}

void KernelEngine::sync_entity_colliders_from_world() {
    auto projectile_view =
        world_.registry().view<NetworkIdentity, ProjectileState>();
    for (const entt::entity entity : projectile_view) {
        const NetworkIdentity& identity =
            projectile_view.get<NetworkIdentity>(entity);
        materialize_projectile_collider(identity.net_id);
    }

    for (ColliderInstance& collider :
         world_.collider_registry().mutable_instances()) {
        if (collider.lifetime_ticks != 0 || collider.entity_net_id == 0) {
            continue;
        }
        const std::optional<entt::entity> entity =
            world_.find_entity(collider.entity_net_id);
        if (!entity.has_value() ||
            !world_.registry().all_of<Transform>(*entity)) {
            continue;
        }
        // A beam is re-aimed every tick it is refreshed, and its endpoints do
        // not follow from the transform the way a rigid offset does.
        if (world_.registry().all_of<ProjectileBeamRuntime>(*entity)) {
            apply_beam_collider_geometry(
                world_.registry().get<ProjectileBeamRuntime>(*entity),
                &collider);
            collider.world_bounds = collider_world_bounds(collider);
            continue;
        }
        const Transform& transform = world_.registry().get<Transform>(*entity);
        collider.world_rotation = transform.rotation * collider.local_rotation;
        collider.world_center =
            transform.position + transform.rotation * collider.local_center;
        collider.world_bounds = collider_world_bounds(collider);
    }

    if (physics_world_ == nullptr) {
        return;
    }
    std::unordered_set<std::uint32_t> current_collider_ids;
    // Rebuilt from scratch on every call, and this runs twice per tick, so it is
    // worth sizing up front. The loop below filters, so the instance count is an
    // upper bound rather than the exact size -- which is what reserve wants.
    current_collider_ids.reserve(world_.collider_registry().instances().size());
    for (const ColliderInstance& collider :
         world_.collider_registry().instances()) {
        if (collider.lifetime_ticks != 0 || collider.entity_net_id == 0 ||
            collider.entity_type == EntityType::kProjectile ||
            collider.shape_type == ColliderShapeType::kSegment ||
            collider.shape_type == ColliderShapeType::kCone) {
            continue;
        }
        const bool movement_collider =
            (collider.purpose_flags & KernelColliderPurpose_Movement) != 0u;
        // A rig's per-bone collider. Kept out of the actor-hitbox mapping below
        // deliberately: routing a leg segment through kActorHitbox would make it
        // a damage volume for every existing weapon query, on a world AABB that
        // does not contain the rotated box. Its own kind and layer keep it
        // invisible to those queries until something asks for limbs.
        const bool limb_collider =
            (collider.purpose_flags & KernelColliderPurpose_Limb) != 0u;
        const std::optional<entt::entity> entity =
            world_.find_entity(collider.entity_net_id);
        if (movement_collider && entity.has_value() &&
            world_.registry().all_of<MovementState>(*entity) &&
            !movement_capsule_blocks_other_actors(
                world_.registry()
                    .get<MovementState>(*entity)
                    .movement_collision_mask)) {
            // Left out of current_collider_ids, so a capsule that was registered
            // before the mask changed is removed by the sweep below.
            continue;
        }
        physics::CollisionObjectDescriptor object{};
        object.identity.entity_net_id = collider.entity_net_id;
        object.identity.collider_id = collider.collider_id;
        object.identity.hit_zone = collider.hit_zone;
        object.identity.kind = limb_collider
            ? physics::CollisionObjectKind::kActorLimb
            : movement_collider
                ? physics::CollisionObjectKind::kActorMovement
                : collider.entity_type == EntityType::kActor
                    ? physics::CollisionObjectKind::kActorHitbox
                    : physics::CollisionObjectKind::kStaticObstacle;
        object.identity.layer = limb_collider
            ? physics::CollisionLayer::kActorLimb
            : movement_collider
                ? physics::CollisionLayer::kActorMovement
                : collider.entity_type == EntityType::kActor
                    ? physics::CollisionLayer::kDamageable
                    : physics::CollisionLayer::kStaticObstacle;
        object.identity.gameplay_category = collider.layer_mask;
        object.shape.type = collider.shape_type == ColliderShapeType::kSphere
            ? physics::CollisionShapeType::kSphere
            : collider.shape_type == ColliderShapeType::kCapsule
                ? physics::CollisionShapeType::kCapsule
                : physics::CollisionShapeType::kBox;
        object.shape.half_extents = collider.half_extents;
        object.shape.radius = collider.radius;
        object.shape.capsule_half_height = collider.capsule_half_height;
        object.position = collider.world_center;
        object.rotation = collider.world_rotation;
        object.enabled = collider.enabled;
        if (entity.has_value() && world_.registry().all_of<Health>(*entity) &&
            world_.registry().get<Health>(*entity).hp == 0) {
            object.enabled = false;
        }
        std::string error;
        if (physics_entity_collider_ids_.contains(collider.collider_id)) {
            physics_world_->set_object_transform(
                collider.collider_id,
                object.position,
                object.rotation);
            physics_world_->set_object_enabled(
                collider.collider_id,
                object.enabled);
        } else if (!physics_world_->upsert_object(object, &error)) {
            spdlog::error(
                "failed to materialize collider_id={} in physics world: {}",
                collider.collider_id,
                error);
            continue;
        }
        current_collider_ids.insert(collider.collider_id);
    }
    for (std::uint32_t collider_id : physics_entity_collider_ids_) {
        if (!current_collider_ids.contains(collider_id)) {
            physics_world_->remove_object(collider_id);
        }
    }
    physics_entity_collider_ids_ = std::move(current_collider_ids);

    // Entity spawns and despawns are the only broad phase churn here (existing
    // colliders are moved in place above), but the authoritative world is just
    // as query-only as the prediction world, so its tree still needs a rebuild
    // to hand retired nodes back to Jolt's allocator. No-ops when nothing was
    // added or removed this tick.
    physics_world_->optimize_broad_phase();
}

std::uint32_t KernelEngine::collider_template_id_for_projectile_template(
    std::uint32_t projectile_template_id) const {
    const KernelProjectileTemplateDefinition* projectile_template =
        find_projectile_template(projectile_templates_, projectile_template_id);
    return projectile_template == nullptr
               ? 0u
               : projectile_template->mechanics.collider_template_id;
}

void KernelEngine::sync_client_follower_limb_colliders() {
    for (const auto& [net_id, state] : follower_locomotion_states_) {
        if (!state.pose_valid || state.solved_collider_poses.empty()) {
            continue;
        }
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [net_id = net_id](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(
                entity_templates_,
                replicated->type == EntityType::kActor
                    ? replicated->actor_template_id
                    : replicated->entity_template_id);
        if (entity_template == nullptr ||
            entity_template->skeleton.struct_size <
                sizeof(KernelSkeletonBindingDefinition) ||
            entity_template->skeleton.collider_count !=
                state.solved_collider_poses.size()) {
            continue;
        }
        const RuntimeSkeletonAsset* skeleton_asset = find_skeleton_asset(
            skeleton_assets_,
            entity_template->skeleton.skeleton_asset_id);
        if (skeleton_asset == nullptr) {
            continue;
        }
        for (std::uint32_t index = 0u;
             index < entity_template->skeleton.collider_count;
             ++index) {
            // The root is the follower's own, in the snapshot tick space the
            // follower solve ran in -- deliberately NOT the render-time
            // transform these render colliders otherwise use. Composing a pose
            // solved at one instant onto a root interpolated at another is the
            // time-base split that slides feet; the limbs must ride the root
            // their own solve was given.
            world_.collider_registry().upsert_bone_collider(
                net_id,
                /*collider_template_id=*/0u,
                entity_template->skeleton.colliders[index].bone_index,
                make_limb_collider(
                    net_id,
                    replicated->type,
                    replicated->actor_type,
                    entity_template->skeleton.colliders[index],
                    state.solved_collider_poses[index],
                    skeleton_asset->bind_pose,
                    state.last_root_position,
                    state.applied_root_rotation));
        }
    }
}

void KernelEngine::remove_prediction_limb_proxies(NetId net_id) {
    const auto proxies = prediction_limb_collider_ids_.find(net_id);
    if (proxies == prediction_limb_collider_ids_.end()) {
        return;
    }
    if (prediction_physics_world_ != nullptr) {
        for (const std::uint32_t collider_id : proxies->second) {
            prediction_physics_world_->remove_object(collider_id);
        }
    }
    prediction_limb_collider_ids_.erase(proxies);
}

// The prediction world's copy of a followed rig's legs.
//
// sync_client_follower_limb_colliders is the render-time twin of this: it says
// where the limbs are, this is what makes anything able to walk into them. They
// are separate because they run on different clocks -- that one per rendered
// frame, this one per prediction tick -- and because the collider registry is
// cleared and refilled by the render pass, which is exactly the churn a physics
// body set must not inherit.
void KernelEngine::sync_prediction_limb_proxies() {
    if (prediction_physics_world_ == nullptr) {
        return;
    }
    std::unordered_set<NetId> current;
    for (const auto& [net_id, state] : follower_locomotion_states_) {
        if (!state.pose_valid || state.solved_collider_poses.empty()) {
            continue;
        }
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [net_id = net_id](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(
                entity_templates_,
                replicated->type == EntityType::kActor
                    ? replicated->actor_template_id
                    : replicated->entity_template_id);
        if (entity_template == nullptr ||
            entity_template->skeleton.struct_size <
                sizeof(KernelSkeletonBindingDefinition) ||
            entity_template->skeleton.collider_count !=
                state.solved_collider_poses.size()) {
            continue;
        }
        const RuntimeSkeletonAsset* skeleton_asset = find_skeleton_asset(
            skeleton_assets_,
            entity_template->skeleton.skeleton_asset_id);
        if (skeleton_asset == nullptr) {
            continue;
        }

        auto proxies = prediction_limb_collider_ids_.find(net_id);
        const bool first_sync = proxies == prediction_limb_collider_ids_.end();
        if (first_sync) {
            std::vector<std::uint32_t> ids;
            ids.reserve(entity_template->skeleton.collider_count);
            for (std::uint32_t index = 0u;
                 index < entity_template->skeleton.collider_count;
                 ++index) {
                ids.push_back(next_prediction_proxy_collider_id_++);
            }
            proxies =
                prediction_limb_collider_ids_.emplace(net_id, std::move(ids))
                    .first;
        }

        for (std::uint32_t index = 0u;
             index < entity_template->skeleton.collider_count;
             ++index) {
            // Same derivation as the registry path, from the same root the
            // solve published. Composing a pose solved at one instant onto a
            // root taken at another is the time-base split that slides feet.
            const ColliderInstance limb = make_limb_collider(
                net_id,
                replicated->type,
                replicated->actor_type,
                entity_template->skeleton.colliders[index],
                state.solved_collider_poses[index],
                skeleton_asset->bind_pose,
                state.last_root_position,
                state.applied_root_rotation);
            const std::uint32_t collider_id = proxies->second[index];
            if (!first_sync) {
                // Legs move every tick but never come and go, so they are moved
                // in place: upserting them instead would hand Jolt a fresh body
                // set to churn through on every prediction step.
                prediction_physics_world_->set_object_transform(
                    collider_id, limb.world_center, limb.world_rotation);
                continue;
            }
            physics::CollisionObjectDescriptor object{};
            object.identity.entity_net_id = net_id;
            object.identity.collider_id = collider_id;
            object.identity.hit_zone = limb.hit_zone;
            object.identity.kind = physics::CollisionObjectKind::kActorLimb;
            object.identity.layer = physics::CollisionLayer::kActorLimb;
            object.identity.gameplay_category = limb.layer_mask;
            object.shape.type =
                limb.shape_type == ColliderShapeType::kSphere
                    ? physics::CollisionShapeType::kSphere
                    : limb.shape_type == ColliderShapeType::kCapsule
                        ? physics::CollisionShapeType::kCapsule
                        : physics::CollisionShapeType::kBox;
            object.shape.half_extents = limb.half_extents;
            object.shape.radius = limb.radius;
            object.shape.capsule_half_height = limb.capsule_half_height;
            object.position = limb.world_center;
            object.rotation = limb.world_rotation;
            object.enabled = limb.enabled;
            std::string error;
            if (!prediction_physics_world_->upsert_object(object, &error)) {
                spdlog::error(
                    "failed to register prediction limb proxy net_id={}: {}",
                    net_id,
                    error);
            }
        }
        current.insert(net_id);
    }

    for (auto entry = prediction_limb_collider_ids_.begin();
         entry != prediction_limb_collider_ids_.end();) {
        if (current.contains(entry->first)) {
            ++entry;
            continue;
        }
        for (const std::uint32_t collider_id : entry->second) {
            prediction_physics_world_->remove_object(collider_id);
        }
        entry = prediction_limb_collider_ids_.erase(entry);
    }
}

void KernelEngine::sync_client_render_colliders() {
    if (config_.mode != KernelMode_Client) {
        return;
    }

    world_.collider_registry().mutable_instances().clear();
    std::unordered_set<NetId> current_prediction_obstacles;
    for (const RenderEntityState& state : render_states_) {
        const EntityType entity_type =
            static_cast<EntityType>(state.entity_type);
        const std::uint32_t collider_template_id = state.collider_template_id;
        if (collider_template_id == 0u) {
            continue;
        }
        const KernelColliderTemplateDefinition* collider_template =
            find_collider_template(collider_templates_, collider_template_id);
        if (collider_template == nullptr) {
            continue;
        }

        glm::vec3 local_center = from_kernel_vec3(collider_template->center);
        glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        const glm::quat render_rotation = from_kernel_quat(state.rotation);
        ColliderInstance collider{};
        collider.collider_template_id = collider_template->template_id;
        collider.owner_net_id =
            entity_type == EntityType::kProjectile ? 0u : state.net_id;
        collider.entity_net_id = state.net_id;
        collider.entity_type = entity_type;
        collider.actor_type = static_cast<ActorType>(state.actor_type);
        collider.shape_type = to_collider_shape_type(collider_template->shape_type);
        collider.purpose_flags = collider_template->purpose_flags;
        collider.layer_mask = collider_template->layer_mask;
        collider.local_center = local_center;
        collider.local_rotation = local_rotation;
        collider.world_rotation = render_rotation * local_rotation;
        collider.world_center =
            from_kernel_vec3(state.position) + render_rotation * local_center;
        collider.half_extents = collider_template_half_extents(*collider_template);
        collider.radius = collider_template_radius(*collider_template);
        collider.lifetime_ticks = collider_template->lifetime_ticks;
        collider.remaining_ticks = collider_template->lifetime_ticks;
        // A beam's replicated endpoint is the whole shape. Without this the
        // client rebuilds the authored oriented box instead, centred on the
        // muzzle and aligned to the world axes -- half of it behind the shooter
        // and pointing nowhere near the aim. Mirrors what
        // apply_beam_collider_geometry does on a server.
        if (entity_type == EntityType::kProjectile &&
            (state.beam_end.x != 0.0f || state.beam_end.y != 0.0f ||
             state.beam_end.z != 0.0f)) {
            collider.shape_type = ColliderShapeType::kSegment;
            collider.segment_start = from_kernel_vec3(state.position);
            collider.segment_end = from_kernel_vec3(state.beam_end);
            collider.radius = std::max(
                collider.half_extents.x,
                collider.half_extents.y);
            collider.half_extents = glm::vec3{0.0f, 0.0f, 0.0f};
            collider.local_center = glm::vec3{0.0f, 0.0f, 0.0f};
            collider.local_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            collider.world_rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            collider.world_center =
                (collider.segment_start + collider.segment_end) * 0.5f;
        }
        collider.world_bounds = collider_world_bounds(collider);
        if (state.net_id == 0) {
            world_.collider_registry().add_ephemeral_collider(collider);
        } else {
            world_.collider_registry().upsert_entity_collider(
                state.net_id,
                collider_template->template_id,
                collider);
        }
        if (prediction_physics_world_ == nullptr ||
            entity_type != EntityType::kProp ||
            state.item_instance_id != 0u ||
            state.net_id == 0u ||
            (collider.purpose_flags & KernelColliderPurpose_Hit) == 0u ||
            collider.shape_type == ColliderShapeType::kSegment ||
            collider.shape_type == ColliderShapeType::kCone) {
            continue;
        }

        auto proxy = prediction_obstacle_collider_ids_.find(state.net_id);
        if (proxy == prediction_obstacle_collider_ids_.end()) {
            proxy = prediction_obstacle_collider_ids_
                        .emplace(
                            state.net_id,
                            next_prediction_proxy_collider_id_++)
                        .first;
        }
        physics::CollisionObjectDescriptor object{};
        object.identity.entity_net_id = state.net_id;
        object.identity.collider_id = proxy->second;
        object.identity.kind = physics::CollisionObjectKind::kStaticObstacle;
        object.identity.layer = physics::CollisionLayer::kStaticObstacle;
        object.identity.gameplay_category = collider.layer_mask;
        object.shape.type = collider.shape_type == ColliderShapeType::kSphere
            ? physics::CollisionShapeType::kSphere
            : collider.shape_type == ColliderShapeType::kCapsule
                ? physics::CollisionShapeType::kCapsule
                : physics::CollisionShapeType::kBox;
        object.shape.half_extents = collider.half_extents;
        object.shape.radius = collider.radius;
        object.shape.capsule_half_height = collider.capsule_half_height;
        object.position = collider.world_center;
        object.rotation = collider.world_rotation;
        object.enabled = collider.enabled;
        std::string error;
        if (!prediction_physics_world_->upsert_object(object, &error)) {
            spdlog::error(
                "failed to update prediction obstacle proxy net_id={}: {}",
                state.net_id,
                error);
            continue;
        }
        current_prediction_obstacles.insert(state.net_id);
    }

    for (auto proxy = prediction_obstacle_collider_ids_.begin();
         proxy != prediction_obstacle_collider_ids_.end();) {
        if (current_prediction_obstacles.contains(proxy->first)) {
            ++proxy;
            continue;
        }
        if (prediction_physics_world_ != nullptr) {
            prediction_physics_world_->remove_object(proxy->second);
        }
        proxy = prediction_obstacle_collider_ids_.erase(proxy);
    }

    // After the render pass, because that pass clears the registry: a rig's
    // per-bone colliders have no collider template and so are not reachable
    // from render_states_ at all. They are rebuilt from the follower solve
    // instead, which is the point -- nothing about them travels on the wire.
    sync_client_follower_limb_colliders();

    // This runs once per rendered frame, so it is where the prediction world
    // accumulates broad phase churn. Jolt only reclaims broad phase nodes during
    // a tree rebuild, and a query-only world never gets one on its own -- see
    // PhysicsWorld::optimize_broad_phase(). The call no-ops on frames where no
    // proxy was added or removed.
    if (prediction_physics_world_ != nullptr) {
        prediction_physics_world_->optimize_broad_phase();
    }
}

KernelLocalPlayerInfo KernelEngine::local_player_info() const {
    return KernelLocalPlayerInfo{
        local_client_peer_id_,
        local_player_net_id_,
        has_welcome_ ? 1u : 0u,
        running_ && has_welcome_ && local_client_peer_id_ != 0 &&
                local_player_net_id_ != 0
            ? 1u
            : 0u,
    };
}

bool KernelEngine::server_create_entity(
    const KernelServerEntityCreateInfo& create_info,
    NetId* out_net_id) {
    return EntityLifecycleSystem{}.create_entity(*this, create_info, out_net_id);
}

bool KernelEngine::server_activate_entity(
    const KernelServerEntityActivateInfo& activate_info) {
    return ActivationSystem{}.activate_entity(*this, activate_info);
}

bool KernelEngine::server_create_inventory_container(
    std::uint32_t owner_entity_id,
    std::uint32_t slot_capacity,
    KernelInventoryContainerId* out_container_id) {
    if (!is_server_mode(config_.mode) || out_container_id == nullptr ||
        !world_.find_entity(owner_entity_id).has_value()) {
        return false;
    }
    const auto created = item_store_.create_container(
        owner_entity_id,
        slot_capacity);
    if (!created.has_value()) return false;
    *out_container_id = *created;
    return true;
}

bool KernelEngine::server_create_inventory_item(
    std::uint32_t item_template_id,
    std::uint32_t quantity,
    KernelInventoryContainerId container_id,
    KernelItemInstanceId* out_item_instance_id) {
    if (!is_server_mode(config_.mode) || out_item_instance_id == nullptr) {
        return false;
    }
    const auto created = item_store_.create_inventory_item(
        item_template_id,
        quantity,
        container_id);
    if (!created.has_value()) return false;
    *out_item_instance_id = *created;
    return true;
}

bool KernelEngine::server_create_world_item(
    std::uint32_t item_template_id,
    std::uint32_t quantity,
    const KernelVec3& position,
    KernelItemInstanceId* out_item_instance_id,
    std::uint32_t* out_prop_entity_id) {
    if (!is_server_mode(config_.mode) || out_item_instance_id == nullptr ||
        out_prop_entity_id == nullptr) {
        return false;
    }
    const KernelItemTemplateDefinition* definition =
        item_store_.find_template(item_template_id);
    if (definition == nullptr || definition->entity_template_id == 0) {
        return false;
    }
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_template_id = definition->entity_template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t prop_id = 0;
    if (!EntityLifecycleSystem{}.create_entity(
            *this, create, &prop_id, false)) {
        return false;
    }
    const auto item = item_store_.create_world_item(
        item_template_id,
        quantity,
        prop_id,
        KernelWorldItemMode_Placed);
    if (!item.has_value()) {
        server_destroy_entity(prop_id, KernelDespawnReason_Destroyed);
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(prop_id);
    world_.registry().emplace_or_replace<ItemTemplateRef>(
        *entity,
        ItemTemplateRef{item_template_id});
    world_.registry().emplace_or_replace<ItemInstanceRef>(
        *entity,
        ItemInstanceRef{*item});
    const ItemInstanceRecord* item_record = item_store_.find_item(*item);
    if (item_record == nullptr ||
        !ItemGameplaySystem{}.decorate_item_prop(*this, prop_id, *item_record)) {
        server_destroy_entity(prop_id, KernelDespawnReason_Destroyed);
        return false;
    }
    *out_item_instance_id = *item;
    *out_prop_entity_id = prop_id;
    queue_prop_state_change(prop_id);
    publish_snapshot();
    return true;
}

bool KernelEngine::server_submit_gameplay_request(
    const KernelGameplayRequest& request) {
    if (!is_server_mode(config_.mode)) return false;
    return ItemGameplaySystem{}.submit_request(*this, request);
}

bool KernelEngine::submit_gameplay_request(
    const KernelGameplayRequest& authored_request) {
    if (config_.mode != KernelMode_Client) {
        return server_submit_gameplay_request(authored_request);
    }
    if (!has_welcome_ || transport_ == nullptr ||
        authored_request.struct_size < sizeof(KernelGameplayRequest)) {
        return false;
    }
    KernelGameplayRequest request = authored_request;
    request.struct_size = sizeof(request);
    request.requester_peer = local_client_peer_id_;
    if (request.instigator_net_id == 0u) {
        request.instigator_net_id = local_player_net_id_;
    }
    const std::vector<std::uint8_t> packet =
        encode_gameplay_request_packet(request, next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        return false;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kReliableEvent);
    return true;
}

bool KernelEngine::get_item_instance(
    KernelItemInstanceId id,
    KernelItemInstanceView* out_view) const {
    if (out_view == nullptr ||
        out_view->struct_size < sizeof(KernelItemInstanceView) ||
        item_store_.find_item(id) == nullptr) {
        return false;
    }
    *out_view = item_store_.item_view(id);
    return true;
}

bool KernelEngine::get_inventory_container(
    KernelInventoryContainerId id,
    KernelInventoryContainerView* out_view) const {
    if (out_view == nullptr ||
        out_view->struct_size < sizeof(KernelInventoryContainerView) ||
        item_store_.find_container(id) == nullptr) {
        return false;
    }
    *out_view = item_store_.container_view(id);
    if (config_.mode == KernelMode_Client) {
        const auto sync = client_inventory_sync_states_.find(id);
        out_view->sync_state = sync == client_inventory_sync_states_.end()
            ? KernelInventorySyncState_NotAvailable
            : static_cast<std::uint8_t>(sync->second);
    }
    return true;
}

std::uint32_t KernelEngine::copy_owned_inventory_containers(
    std::uint32_t owner_entity_id,
    KernelInventoryContainerView* out_containers,
    std::uint32_t max_containers) const {
    if (owner_entity_id == 0u) {
        return 0u;
    }
    std::vector<KernelInventoryContainerId> ids =
        item_store_.containers_for_owner(owner_entity_id);
    if (config_.mode == KernelMode_Client) {
        for (const auto& [id, assembly] : client_inventory_snapshot_assemblies_) {
            if (assembly.container.owner_entity_id == owner_entity_id &&
                std::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
        std::sort(ids.begin(), ids.end());
    }
    if (out_containers == nullptr || max_containers == 0u) {
        return static_cast<std::uint32_t>(ids.size());
    }
    const std::uint32_t count = std::min<std::uint32_t>(
        max_containers, static_cast<std::uint32_t>(ids.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto assembly = client_inventory_snapshot_assemblies_.find(ids[index]);
        out_containers[index] = item_store_.find_container(ids[index]) != nullptr
            ? item_store_.container_view(ids[index])
            : assembly->second.container;
        if (config_.mode == KernelMode_Client) {
            const auto sync = client_inventory_sync_states_.find(ids[index]);
            out_containers[index].sync_state =
                sync == client_inventory_sync_states_.end()
                ? KernelInventorySyncState_NotAvailable
                : static_cast<std::uint8_t>(sync->second);
        }
    }
    return count;
}

std::uint32_t KernelEngine::copy_inventory_slots(
    KernelInventoryContainerId id,
    KernelItemInstanceView* out_items,
    std::uint32_t max_items) const {
    const InventoryContainerRecord* container = item_store_.find_container(id);
    if (container == nullptr) {
        return 0;
    }
    if (out_items == nullptr || max_items == 0u) {
        return static_cast<std::uint32_t>(std::count_if(
            container->slots.begin(),
            container->slots.end(),
            [](KernelItemInstanceId item) { return item != 0u; }));
    }
    std::uint32_t copied = 0;
    for (const KernelItemInstanceId item : container->slots) {
        if (item == 0 || copied >= max_items) continue;
        out_items[copied++] = item_store_.item_view(item);
    }
    return copied;
}

std::uint32_t KernelEngine::poll_gameplay_request_outcomes(
    KernelGameplayRequestOutcome* out_outcomes,
    std::uint32_t max_outcomes) {
    if (out_outcomes == nullptr || max_outcomes == 0) return 0;
    std::uint32_t copied = 0;
    while (copied < max_outcomes &&
           !pending_gameplay_request_outcomes_.empty()) {
        out_outcomes[copied++] = pending_gameplay_request_outcomes_.front();
        pending_gameplay_request_outcomes_.pop_front();
    }
    return copied;
}

bool KernelEngine::get_gameplay_request_outcome(
    std::uint32_t requester_peer,
    std::uint64_t request_id,
    KernelGameplayRequestOutcome* out_outcome) const {
    if (request_id == 0u || out_outcome == nullptr) return false;
    const auto outcome = std::find_if(
        processed_gameplay_requests_.begin(),
        processed_gameplay_requests_.end(),
        [&](const KernelGameplayRequestOutcome& candidate) {
            return candidate.requester_peer == requester_peer &&
                candidate.request_id == request_id;
        });
    if (outcome == processed_gameplay_requests_.end()) return false;
    *out_outcome = *outcome;
    return true;
}

std::uint32_t KernelEngine::poll_inventory_deltas(
    KernelInventoryContainerId id,
    KernelInventoryDelta* out_deltas,
    std::uint32_t max_deltas) {
    if (out_deltas == nullptr || max_deltas == 0) return 0;
    std::vector<KernelInventoryDelta> deltas =
        item_store_.take_inventory_deltas(id, max_deltas);
    std::copy(deltas.begin(), deltas.end(), out_deltas);
    return static_cast<std::uint32_t>(deltas.size());
}

bool KernelEngine::server_set_entity_actor_template(
    NetId net_id,
    std::uint32_t actor_template_id) {
    return EntityStateSystem{}.set_actor_template(
        *this,
        net_id,
        actor_template_id);
}

bool KernelEngine::server_destroy_entity(NetId net_id, std::uint32_t reason) {
    return EntityLifecycleSystem{}.destroy_entity(*this, net_id, reason);
}

bool KernelEngine::server_enqueue_entity_lifecycle(
    std::uint32_t command_source,
    const KernelEntityLifecycleCommand& command) {
    if (!running_ || !is_server_mode(config_.mode) ||
        command.struct_size < sizeof(KernelEntityLifecycleCommand)) {
        return false;
    }
    simulation::CommandSource source{};
    if (!to_simulation_command_source(command_source, &source)) {
        return false;
    }
    if (command.command_type != KernelEntityLifecycleCommandType_Destroy) {
        return false;
    }

    simulation::Command queued_command{};
    queued_command.id = simulation::CommandId::kDestroyEntity;
    queued_command.source = source;
    queued_command.destroy_entity.net_id = command.net_id;
    queued_command.destroy_entity.reason = command.reason;
    return enqueue_simulation_command(queued_command);
}

bool KernelEngine::server_set_entity_transform(
    NetId net_id,
    const KernelVec3& position,
    const KernelQuat& rotation) {
    return EntityStateSystem{}.set_transform(*this, net_id, position, rotation);
}

bool KernelEngine::server_set_entity_velocity(
    NetId net_id,
    const KernelVec3& velocity) {
    return EntityStateSystem{}.set_velocity(*this, net_id, velocity);
}

bool KernelEngine::server_set_entity_state(
    NetId net_id,
    std::uint16_t animation_state,
    std::uint32_t visual_flags) {
    return EntityStateSystem{}.set_state(
        *this,
        net_id,
        animation_state,
        visual_flags);
}

bool KernelEngine::server_set_entity_health(NetId net_id, std::uint16_t hp) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Health>(*entity)) {
        return false;
    }
    Health& health = world_.registry().get<Health>(*entity);
    if (health.hp == hp) {
        return true;
    }
    health.hp = hp;
    if (world_.registry().all_of<EntityKind>(*entity) &&
        world_.registry().get<EntityKind>(*entity).type == EntityType::kProp) {
        queue_prop_state_change(net_id);
    }
    return true;
}

bool KernelEngine::server_submit_entity_input(NetId net_id, const KernelPlayerInput& input) {
    return MovementSystem{}.submit_player_input(*this, net_id, input);
}

bool KernelEngine::server_enqueue_entity_transform(
    std::uint32_t command_source,
    NetId net_id,
    const KernelVec3& position,
    const KernelQuat& rotation) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    simulation::CommandSource source{};
    if (!to_simulation_command_source(command_source, &source)) {
        return false;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityTransform;
    command.source = source;
    command.set_entity_transform.net_id = net_id;
    command.set_entity_transform.position = position;
    command.set_entity_transform.rotation = rotation;
    return enqueue_simulation_command(command);
}

bool KernelEngine::server_enqueue_entity_velocity(
    std::uint32_t command_source,
    NetId net_id,
    const KernelVec3& velocity) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    simulation::CommandSource source{};
    if (!to_simulation_command_source(command_source, &source)) {
        return false;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityVelocity;
    command.source = source;
    command.set_entity_velocity.net_id = net_id;
    command.set_entity_velocity.velocity = velocity;
    return enqueue_simulation_command(command);
}

bool KernelEngine::server_enqueue_entity_state(
    std::uint32_t command_source,
    NetId net_id,
    std::uint16_t animation_state,
    std::uint32_t visual_flags) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    simulation::CommandSource source{};
    if (!to_simulation_command_source(command_source, &source)) {
        return false;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kSetEntityState;
    command.source = source;
    command.set_entity_state.net_id = net_id;
    command.set_entity_state.animation_state = animation_state;
    command.set_entity_state.visual_flags = visual_flags;
    return enqueue_simulation_command(command);
}

bool KernelEngine::server_enqueue_entity_input(
    std::uint32_t command_source,
    NetId net_id,
    const KernelPlayerInput& input) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    simulation::CommandSource source{};
    if (!to_simulation_command_source(command_source, &source)) {
        return false;
    }
    simulation::Command command{};
    command.id = simulation::CommandId::kSubmitPlayerInput;
    command.source = source;
    command.submit_player_input.net_id = net_id;
    command.submit_player_input.input = input;
    return enqueue_simulation_command(command);
}

bool KernelEngine::server_set_entity_combat_state(
    NetId net_id,
    const KernelCombatStateDefinition& combat_state) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0 ||
        combat_state.struct_size < sizeof(KernelCombatStateDefinition) ||
        combat_state.weapon_slot_count == 0u ||
        combat_state.weapon_slot_count > KERNEL_MAX_WEAPON_SLOTS ||
        combat_state.active_weapon_slot >= combat_state.weapon_slot_count ||
        combat_state.collider_template_id == 0 ||
        find_collider_template(
            collider_templates_,
            combat_state.collider_template_id) == nullptr) {
        return false;
    }
    for (std::size_t slot = 0; slot < combat_state.weapon_slot_count; ++slot) {
        if (combat_state.weapon_ids[slot] > UINT8_MAX) {
            return false;
        }
        for (std::size_t previous = 0; previous < slot; ++previous) {
            if (combat_state.weapon_ids[previous] ==
                combat_state.weapon_ids[slot]) {
                return false;
            }
        }
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }

    Health& health = world_.registry().get_or_emplace<Health>(*entity);
    health.hp = combat_state.hp;
    health.max_hp = combat_state.max_hp;

    Hitbox& hitbox = world_.registry().get_or_emplace<Hitbox>(*entity);
    hitbox.center = from_kernel_vec3(combat_state.hitbox_center);
    hitbox.half_extents = from_kernel_vec3(combat_state.hitbox_half_extents);
    hitbox.collider_template_id = combat_state.collider_template_id;

    MovementState& movement = world_.registry().get_or_emplace<MovementState>(*entity);
    movement.base_speed_meters_per_second = combat_state.move_speed_meters_per_second;
    movement.speed_meters_per_second = combat_state.move_speed_meters_per_second;

    WeaponState& weapon = world_.registry().get_or_emplace<WeaponState>(*entity);
    weapon.active_weapon_slot = combat_state.active_weapon_slot;
    weapon.weapon_slot_count = combat_state.weapon_slot_count;
    for (std::size_t slot = 0; slot < kWeaponSlotCount; ++slot) {
        weapon.weapon_ids[slot] = combat_state.weapon_ids[slot];
        weapon.ammo[slot] = combat_state.ammo[slot];
        weapon.reserve_magazines[slot] = combat_state.reserve_magazines[slot];
    }
    if (net_id == local_player_net_id_) {
        local_player_move_speed_meters_per_second_ =
            combat_state.move_speed_meters_per_second;
    }
    materialize_entity_collider(net_id);
    publish_snapshot();
    rebuild_render_states();
    return true;
}

bool KernelEngine::server_set_entity_vision_config(
    NetId net_id,
    const KernelAgentVisionConfig& vision_config) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0 ||
        vision_config.struct_size < sizeof(KernelAgentVisionConfig) ||
        !is_valid_agent_camp(vision_config.camp)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    if (vision_config.vision_collider_template_id != 0u) {
        const KernelColliderTemplateDefinition* vision_collider =
            find_collider_template(
                collider_templates_,
                vision_config.vision_collider_template_id);
        if (vision_collider == nullptr ||
            vision_collider->shape_type != KernelColliderShapeType_Cone ||
            (vision_collider->purpose_flags & KernelColliderPurpose_Vision) == 0u) {
            return false;
        }
    }

    KernelAgentVisionConfig stored = vision_config;
    stored.struct_size = sizeof(KernelAgentVisionConfig);
    if (stored.max_visible_hostiles > KERNEL_MAX_VISIBLE_HOSTILES) {
        stored.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    }
    if (stored.max_visible_allies > KERNEL_MAX_VISIBLE_ALLIES) {
        stored.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    }
    if (stored.max_visible_neutrals > KERNEL_MAX_VISIBLE_NEUTRALS) {
        stored.max_visible_neutrals = KERNEL_MAX_VISIBLE_NEUTRALS;
    }
    vision_configs_[net_id] = stored;
    vision_states_[net_id].view.struct_size = sizeof(KernelVisionStateView);
    return true;
}

bool KernelEngine::server_clear_entity_vision_config(NetId net_id) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0) {
        return false;
    }
    const bool had_config = vision_configs_.erase(net_id) > 0;
    vision_states_.erase(net_id);
    return had_config;
}

bool KernelEngine::server_set_entity_weapon_mechanics(
    NetId net_id,
    const KernelWeaponMechanicsDefinition& weapon_mechanics) {
    if (!running_ || !is_server_mode(config_.mode) ||
        !validate_weapon_mechanics(weapon_mechanics) ||
        find_action_template(
            action_templates_,
            weapon_mechanics.fire_action_template_id) == nullptr ||
        find_action_template(
            action_templates_,
            weapon_mechanics.reload_action_template_id) == nullptr) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    WeaponTuning& tuning = world_.registry().get_or_emplace<WeaponTuning>(*entity);
    const std::size_t index = static_cast<std::size_t>(weapon_mechanics.weapon_id);
    tuning.configured[index] = true;
    tuning.definitions[index] = to_weapon_mechanics(weapon_mechanics);
    return true;
}

bool KernelEngine::server_clear_entity_weapon_mechanics(
    NetId net_id,
    std::uint8_t weapon_id) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<WeaponTuning>(*entity)) {
        return false;
    }
    WeaponTuning& tuning = world_.registry().get<WeaponTuning>(*entity);
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    tuning.configured[index] = false;
    tuning.definitions[index] = WeaponMechanicsDefinition{};
    return true;
}

bool KernelEngine::server_get_entity_weapon_mechanics(
    NetId net_id,
    std::uint8_t weapon_id,
    KernelWeaponMechanicsDefinition* out_weapon_mechanics) const {
    if (!running_ || !is_server_mode(config_.mode) ||
        out_weapon_mechanics == nullptr ||
        out_weapon_mechanics->struct_size < sizeof(KernelWeaponMechanicsDefinition)) {
        return false;
    }
    const WeaponMechanicsDefinition* mechanics =
        entity_weapon_mechanics(world_, net_id, weapon_id);
    if (mechanics == nullptr) {
        return false;
    }
    *out_weapon_mechanics = to_kernel_weapon_mechanics(*mechanics);
    return true;
}

bool KernelEngine::server_get_homing_state(
    NetId net_id,
    KernelHomingState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < sizeof(KernelHomingState)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, ProjectileState, HomingState, ProjectileTag>(
            *entity)) {
        return false;
    }
    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const ProjectileState& projectile =
        world_.registry().get<ProjectileState>(*entity);
    const HomingState& homing = world_.registry().get<HomingState>(*entity);
    std::memset(out_state, 0, sizeof(KernelHomingState));
    out_state->struct_size = sizeof(KernelHomingState);
    out_state->net_id = identity.net_id;
    out_state->owner_peer = identity.owner_peer;
    out_state->shooter_net_id = projectile.shooter_net_id;
    out_state->target_net_id = homing.target_net_id;
    out_state->homing_mode = to_kernel_homing_mode(homing.homing_mode);
    out_state->sync_mode = to_kernel_projectile_sync_mode(homing.sync_mode);
    out_state->guidance_phase = to_kernel_guidance_phase(homing.phase);
    out_state->boost_ticks = homing.boost_ticks;
    out_state->guidance_start_tick = homing.guidance_start_tick;
    out_state->lock_on_range = homing.lock_on_range;
    out_state->lose_target_range = homing.lose_target_range;
    out_state->lock_cone_degrees = homing.lock_cone_degrees;
    out_state->max_turn_degrees_per_tick =
        homing.max_turn_degrees_per_tick;
    out_state->acceleration = homing.acceleration;
    out_state->max_speed = homing.max_speed;
    out_state->valid = true;
    return true;
}

bool KernelEngine::server_get_entity_state(
    NetId net_id,
    KernelServerEntityState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < kKernelServerEntityStateBaseSize) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    return write_server_entity_state(
        world_,
        *entity,
        tick_loop_.current_tick(),
        out_state);
}

bool KernelEngine::server_get_projectile_launch_position(
    NetId net_id,
    KernelVec3* out_position) const {
    if (!running_ || !is_server_mode(config_.mode) || out_position == nullptr) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<Transform>(*entity)) {
        return false;
    }
    const glm::vec3 position = projectile_launch_position(
        world_.registry().get<const Transform>(*entity));
    *out_position = KernelVec3{position.x, position.y, position.z};
    return true;
}

bool KernelEngine::server_get_entity_aim_point(
    NetId net_id,
    KernelVec3* out_position) const {
    if (!running_ || !is_server_mode(config_.mode) || out_position == nullptr) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<Transform>(*entity)) {
        return false;
    }
    glm::vec3 position =
        world_.registry().get<const Transform>(*entity).position;
    if (world_.registry().all_of<Hitbox>(*entity)) {
        const Transform& transform =
            world_.registry().get<const Transform>(*entity);
        position +=
            transform.rotation * world_.registry().get<const Hitbox>(*entity).center;
    }
    *out_position = KernelVec3{position.x, position.y, position.z};
    return true;
}

std::uint32_t KernelEngine::server_query_entities(
    EntityType entity_type_filter,
    KernelServerEntityState* out_states,
    std::uint32_t max_states) const {
    if (!running_ || !is_server_mode(config_.mode) || out_states == nullptr ||
        max_states == 0) {
        return 0;
    }

    std::uint32_t count = 0;
    auto view =
        world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
    for (const entt::entity entity : view) {
        const EntityKind& kind = view.get<const EntityKind>(entity);
        if (entity_type_filter != EntityType::kUnknown &&
            kind.type != entity_type_filter) {
            continue;
        }
        if (count >= max_states) {
            break;
        }
        if (!write_server_entity_state(
                world_,
                entity,
                tick_loop_.current_tick(),
                &out_states[count])) {
            break;
        }
        ++count;
    }
    return count;
}

void KernelEngine::push_event(
    KernelEventType type,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code) {
    events_.push_back(KernelEvent{type, tick_loop_.current_tick(), net_id, peer_id, code});
}

void KernelEngine::queue_health_changed_event(
    NetId net_id,
    PeerId source_peer,
    std::int32_t health_delta,
    std::uint64_t event_time_us) {
    if (health_delta == 0) {
        return;
    }
    events_.push_back(KernelEvent{
        KernelEventType_HealthChanged,
        tick_loop_.current_tick(),
        net_id,
        source_peer,
        0u,
        event_time_us,
        event_time_us,
        health_delta,
    });
}

void KernelEngine::register_actor_for_first_physics(NetId net_id) {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<EntityKind>(*entity) ||
        world_.registry().get<EntityKind>(*entity).type != EntityType::kActor ||
        world_.registry().all_of<ServerOnly>(*entity)) {
        return;
    }
    pending_first_physics_actors_.try_emplace(
        net_id,
        PendingFirstPhysicsActor{tick_loop_.current_tick(), false});
}

bool KernelEngine::is_actor_pending_first_physics(NetId net_id) const {
    return pending_first_physics_actors_.find(net_id) !=
        pending_first_physics_actors_.end();
}

void KernelEngine::filter_pending_first_physics_actors(
    WorldSnapshot* snapshot) const {
    if (snapshot == nullptr || pending_first_physics_actors_.empty()) {
        return;
    }
    snapshot->entities.erase(
        std::remove_if(
            snapshot->entities.begin(),
            snapshot->entities.end(),
            [this](const EntitySnapshot& entity) {
                return is_actor_pending_first_physics(entity.net_id);
            }),
        snapshot->entities.end());
}

void KernelEngine::reset_runtime_state(KernelMode mode) {
    config_.mode = mode;
    tick_loop_ = TickLoop(config_.tick);
    world_ = World{false};
    world_.set_action_graph_dedup_retention_ticks(
        action_graph_dedup_retention_ticks(config_.tick));
    item_store_ = ItemStore{};
    client_inventory_snapshot_assemblies_.clear();
    client_inventory_sync_states_.clear();
    client_inventory_resync_pending_.clear();
    pending_prop_state_changes_.clear();
    claimed_item_instances_.clear();
    claimed_prop_entities_.clear();
    std::string item_validation_error;
    item_store_.set_templates(item_templates_, &item_validation_error);
    processed_gameplay_requests_.clear();
    pending_gameplay_request_outcomes_.clear();
    pending_network_gameplay_outcomes_.clear();
    physics_entity_collider_ids_.clear();
    prediction_proxy_collider_ids_.clear();
    prediction_obstacle_collider_ids_.clear();
    history_buffer_ = HistoryBuffer(history_frame_count(config_.tick));
    damage_pipeline_.clear();
    next_action_graph_sequence_ = 1;
    active_prop_collision_pairs_.clear();
    command_queue_.clear();
    rpc_response_store_.clear();
    pending_inputs_.clear();
    events_.clear();
    lifecycle_events_.clear();
    pending_presentation_events_.clear();
    local_action_results_.clear();
    remote_action_presentation_events_.clear();
    pending_server_remote_presentations_.clear();
    pending_remote_action_presentation_events_.clear();
    remote_presentation_dedup_.clear();
    render_states_.clear();
    locomotion_states_.clear();
    skeleton_pose_history_.clear();
    follower_locomotion_states_.clear();
    pending_follower_steps_.clear();
    outgoing_locomotion_steps_.clear();
    follower_locomotion_tick_ = 0u;
    has_follower_locomotion_tick_ = false;
    latest_snapshot_ = WorldSnapshot{};
    latest_client_snapshot_ = WorldSnapshot{};
    client_snapshot_buffer_.clear();
    peer_sessions_.clear();
    local_listen_session_ = PeerSession{};
    client_replicated_entities_.clear();
    client_metadata_timeout_reported_entities_.clear();
    client_despawned_entities_.clear();
    pending_prediction_inputs_.clear();
    latest_client_input_ = KernelPlayerInput{};
    pending_client_action_intents_.clear();
    latest_client_input_time_us_ = 0;
    next_client_input_seq_ = 1;
    latest_client_input_peer_ = 0;
    has_latest_client_input_ = false;
    predicted_projectiles_.clear();
    predicted_projectile_collision_warning_emitted_ = false;
    outstanding_predicted_actions_.clear();
    applied_local_action_results_.clear();
    debug_records_.clear();
    vision_configs_.clear();
    vision_states_.clear();
    pending_first_physics_actors_.clear();
    pending_director_intents_.clear();
    network_stats_ = KernelNetworkStats{};
    network_stats_.struct_size = sizeof(KernelNetworkStats);
    network_stats_.collection_mode = config_.network_stats.mode;
    server_remote_presentation_budget_ = ByteTokenBucket{};
    benchmark_stats_ = KernelBenchmarkStats{};
    rejected_simulation_command_count_ = 0;
    failed_simulation_command_count_ = 0;
    command_queue_capacity_warning_count_ = 0;
    last_command_queue_capacity_warning_tick_ = 0;
    last_simulation_command_queue_depth_ = 0;
    last_simulation_command_processed_count_ = 0;
    last_director_intent_processed_count_ = 0;
    last_director_intent_created_count_ = 0;
    last_director_intent_failed_count_ = 0;
    last_director_intent_unsupported_count_ = 0;
    simulation_tick_cost_samples_us_.fill(0);
    simulation_tick_cost_sample_index_ = 0;
    simulation_tick_cost_sample_count_ = 0;
    simulation_tick_cost_sample_sum_us_ = 0;
    last_simulation_tick_cost_us_ = 0;
    average_simulation_tick_cost_us_ = 0;
    simulation_tick_cost_warning_threshold_us_ = 0;
    simulation_tick_cost_warning_count_ = 0;
    last_simulation_tick_cost_warning_tick_ = 0;
    entity_ids_by_net_id_.clear();
    predicted_local_entity_ = EntitySnapshot{};
    has_authoritative_local_entity_ = false;
    predicted_character_state_ = movement_solver::CharacterMovementState{};
    predicted_character_tick_ = 0;
    predicted_impulse_lockout_until_tick_ = 0u;
    predicted_impulse_lockout_armed_tick_ = 0u;
    predicted_action_buttons_ = 0u;
    predicted_action_binding_id_ = 0u;
    predicted_action_weapon_id_ = 0u;
    predicted_action_next_commit_tick_ = 0u;
    predicted_action_recovery_end_tick_ = 0u;
    predicted_next_primary_commit_tick_.fill(0u);
    local_presentation_position_ = glm::vec3{0.0f, 0.0f, 0.0f};
    local_presentation_velocity_ = glm::vec3{0.0f, 0.0f, 0.0f};
    has_local_presentation_position_ = false;
    predicted_local_state_time_us_ = 0;
    next_entity_id_ = 1;
    next_predicted_entity_id_ = UINT64_C(0x8000000000000000);
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    next_packet_sequence_ = 1;
    next_server_presentation_instance_id_ = 1;
    last_remote_presentation_sequence_ = 0;
    next_clock_sync_nonce_ = 1;
    received_sequences_by_peer_.clear();
    received_packet_count_ = 0;
    lost_packet_count_ = 0;
    client_local_time_us_ = 0;
    client_clock_offset_us_ = 0;
    local_player_move_speed_meters_per_second_ = 0.0f;
    current_render_time_us_ = 0;
    local_client_peer_id_ = 0;
    client_handshake_sent_ = false;
    gameplay_catalog_transfers_.clear();
    downloaded_gameplay_catalog_bundle_.clear();
    gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Idle;
    gameplay_catalog_sync_error_ = KernelGameplayCatalogSyncError_None;
    gameplay_catalog_sync_elapsed_us_ = 0;
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    prediction_failed_ = false;
    has_client_clock_sync_ = false;
    has_client_render_time_ = false;
    has_remote_presentation_sequence_ = false;
    running_ = true;
}

void KernelEngine::poll_transport() {
    TransportEvent transport_event;
    while (transport_->PollEvent(transport_event)) {
        if (transport_event.type == TransportEventType::kConnected) {
            push_event(KernelEventType_Connected, 0, transport_event.peer);
            if (config_.mode == KernelMode_Client) {
                if (gameplay_catalog_sync_state_ ==
                    KernelGameplayCatalogSyncState_Connecting) {
                    send_gameplay_catalog_manifest_request();
                } else {
                    send_client_handshake();
                }
            }
        } else if (transport_event.type == TransportEventType::kDisconnected) {
            if (is_server_mode(config_.mode)) {
                handle_server_disconnect(transport_event);
            } else if (config_.mode == KernelMode_Client) {
                if (gameplay_catalog_sync_state_ !=
                        KernelGameplayCatalogSyncState_Idle &&
                    gameplay_catalog_sync_state_ !=
                        KernelGameplayCatalogSyncState_Ready) {
                    fail_gameplay_catalog_sync(
                        KernelGameplayCatalogSyncError_Disconnected);
                }
                handle_client_disconnect(transport_event.peer);
            } else {
                const PeerSession* session = find_session(transport_event.peer);
                if (session != nullptr) {
                    push_event(KernelEventType_PlayerLeft, session->player, transport_event.peer);
                    remove_session(transport_event.peer);
                }
                push_event(KernelEventType_Disconnected, 0, transport_event.peer);
            }
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSession &&
            is_server_mode(config_.mode)) {
            record_received_packet_sequence(transport_event);
            handle_server_session_message(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSession &&
            config_.mode == KernelMode_Client) {
            record_received_packet_sequence(transport_event);
            handle_client_session_message(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kReliableEvent &&
            is_server_mode(config_.mode)) {
            record_received_packet_sequence(transport_event);
            const PeerSession* session = find_session(transport_event.peer);
            InventorySnapshotRequestPacket inventory_request;
            if (session != nullptr && session->welcomed &&
                decode_inventory_snapshot_request_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &inventory_request)) {
                const InventoryContainerRecord* container =
                    item_store_.find_container(
                        inventory_request.inventory_container_id);
                if (container == nullptr ||
                    container->owner_entity_id != session->player) {
                    push_event(KernelEventType_Error, 0, transport_event.peer, 33);
                    continue;
                }
                ++network_stats_.inventory_resync_request_count;
                PeerSession* mutable_session = find_session(transport_event.peer);
                send_inventory_snapshot(
                    mutable_session,
                    inventory_request.inventory_container_id);
                continue;
            }
            KernelGameplayRequest request{};
            if (session == nullptr || !session->welcomed ||
                !decode_gameplay_request_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &request)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 31);
                continue;
            }
            request.requester_peer = transport_event.peer;
            if (request.instigator_net_id != session->player ||
                !server_submit_gameplay_request(request)) {
                KernelGameplayRequestOutcome rejected{};
                rejected.struct_size = sizeof(rejected);
                rejected.requester_peer = transport_event.peer;
                rejected.request_id = request.request_id;
                rejected.status = KernelGameplayRequestStatus_Rejected;
                rejected.graph_outcome = KernelGameplayGraphOutcome_NotSubmitted;
                rejected.rejection_reason =
                    KernelGameplayRequestRejection_NotAuthorized;
                pending_network_gameplay_outcomes_.push_back(
                    {transport_event.peer, rejected});
                continue;
            }
            const auto outcome = std::find_if(
                processed_gameplay_requests_.rbegin(),
                processed_gameplay_requests_.rend(),
                [&](const KernelGameplayRequestOutcome& candidate) {
                    return candidate.requester_peer == transport_event.peer &&
                        candidate.request_id == request.request_id;
                });
            if (outcome != processed_gameplay_requests_.rend()) {
                pending_network_gameplay_outcomes_.push_back(
                    {transport_event.peer, *outcome});
            }
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kReliableEvent &&
            config_.mode == KernelMode_Client) {
            record_received_packet_sequence(transport_event);
            handle_client_reliable_event(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kPresentation &&
            config_.mode == KernelMode_Client) {
            record_received_packet_sequence(transport_event);
            handle_client_remote_action_presentation(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSnapshot &&
            config_.mode == KernelMode_Client) {
            record_received_packet_sequence(transport_event);
            // The snapshot channel also carries replicated locomotion steps, so
            // a payload that is not a snapshot is tried as one of those before
            // it counts as a decode failure.
            LocomotionStepBatchPacket locomotion_steps;
            if (decode_locomotion_step_batch_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &locomotion_steps)) {
                handle_client_locomotion_step_batch(locomotion_steps);
                continue;
            }
            WorldSnapshot snapshot;
            const auto decode_start = std::chrono::steady_clock::now();
            if (!decode_snapshot_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &snapshot)) {
                record_packet_deserialization_cost(elapsed_cost_us(decode_start));
                log_snapshot_decode_failure(transport_event);
                push_event(KernelEventType_Error, 0, transport_event.peer, 6);
                continue;
            }
            record_packet_deserialization_cost(elapsed_cost_us(decode_start));
            handle_client_snapshot(std::move(snapshot));
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kInput &&
            (config_.mode == KernelMode_DedicatedServer ||
             config_.mode == KernelMode_ListenServer)) {
            record_received_packet_sequence(transport_event);
            const PeerSession* session = find_session(transport_event.peer);
            const bool is_local_listen_peer =
                config_.mode == KernelMode_ListenServer &&
                transport_event.peer == kLocalListenPeerId &&
                local_listen_session_.welcomed;
            if (!is_local_listen_peer &&
                (session == nullptr || !session->welcomed)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 10);
                continue;
            }

            PeerId player_id = 0;
            KernelPlayerInput input{};
            const auto decode_start = std::chrono::steady_clock::now();
            if (!decode_player_input_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &player_id,
                    &input)) {
                record_packet_deserialization_cost(elapsed_cost_us(decode_start));
                push_event(KernelEventType_Error, 0, transport_event.peer, 5);
                continue;
            }
            record_packet_deserialization_cost(elapsed_cost_us(decode_start));
            if (player_id != transport_event.peer) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 11);
                continue;
            }
            PeerSession* mutable_session = result_session_for_peer(
                transport_event.peer);
            const std::uint64_t received_server_time_us = current_server_time_us();
            if (!cache_server_movement_input(
                    mutable_session, input, received_server_time_us)) {
                continue;
            }
            prepare_server_action_intent(mutable_session, &input);
            const std::uint64_t action_server_time_us =
                convert_client_action_time_to_server_time(
                    player_id,
                    input.client_action_time_us,
                    received_server_time_us);
            pending_inputs_.push_back(QueuedInput{
                player_id,
                input,
                tick_loop_.current_tick(),
                action_server_time_us,
                true,
            });
        }
    }
}

void KernelEngine::handle_server_disconnect(const TransportEvent& transport_event) {
    gameplay_catalog_transfers_.erase(transport_event.peer);
    const PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr) {
        push_event(KernelEventType_Disconnected, 0, transport_event.peer);
        return;
    }

    const KernelEvent player_left{
        KernelEventType_PlayerLeft,
        tick_loop_.current_tick(),
        session->player,
        transport_event.peer,
        0,
    };
    events_.push_back(player_left);

    // Current server mechanism removes the disconnected player immediately.
    // A later policy may preserve selected entities while clearing transient state.
    if (world_.destroy(session->player)) {
        push_event(KernelEventType_EntityDestroyed, session->player, transport_event.peer);
    }

    remove_session(transport_event.peer);
    broadcast_reliable_event(player_left);
    publish_snapshot();
    push_event(KernelEventType_Disconnected, 0, transport_event.peer);
}

void KernelEngine::handle_client_disconnect(PeerId peer) {
    clear_client_session();
    push_event(KernelEventType_Disconnected, 0, peer);
}

void KernelEngine::handle_client_inventory_snapshot_page(
    const InventorySnapshotPagePacket& packet) {
    ClientInventorySnapshotAssembly& assembly =
        client_inventory_snapshot_assemblies_[packet.inventory_container_id];
    if (assembly.page_count == 0u ||
        assembly.container.revision != packet.revision ||
        assembly.page_count != packet.page_count) {
        assembly = ClientInventorySnapshotAssembly{};
        assembly.container.struct_size = sizeof(KernelInventoryContainerView);
        assembly.container.inventory_container_id =
            packet.inventory_container_id;
        assembly.container.owner_entity_id = packet.owner_entity_id;
        assembly.container.slot_capacity = packet.slot_capacity;
        assembly.container.revision = packet.revision;
        assembly.container.sync_state = KernelInventorySyncState_Syncing;
        assembly.page_count = packet.page_count;
        assembly.received_pages.assign(packet.page_count, false);
    }
    if (packet.page_index >= assembly.received_pages.size() ||
        assembly.received_pages[packet.page_index]) {
        return;
    }
    for (const InventorySnapshotEntry& entry : packet.entries) {
        KernelItemInstanceView item{};
        if (!inventory_view_from_wire(
                item_store_,
                entry.item,
                packet.inventory_container_id,
                entry.slot,
                &item)) {
            client_inventory_sync_states_[packet.inventory_container_id] =
                KernelInventorySyncState_Desynced;
            ++network_stats_.inventory_revision_gap_count;
            request_inventory_snapshot(packet.inventory_container_id, 0u);
            return;
        }
        assembly.items.push_back(item);
    }
    assembly.received_pages[packet.page_index] = true;
    client_inventory_sync_states_[packet.inventory_container_id] =
        KernelInventorySyncState_Syncing;
    if (!std::all_of(
            assembly.received_pages.begin(),
            assembly.received_pages.end(),
            [](bool received) { return received; })) {
        return;
    }
    assembly.container.occupied_slot_count =
        static_cast<std::uint32_t>(assembly.items.size());
    assembly.container.sync_state = KernelInventorySyncState_Ready;
    if (!item_store_.apply_replica_snapshot(
            assembly.container, assembly.items)) {
        client_inventory_sync_states_[packet.inventory_container_id] =
            KernelInventorySyncState_Desynced;
        request_inventory_snapshot(packet.inventory_container_id, 0u);
        return;
    }
    client_inventory_sync_states_[packet.inventory_container_id] =
        KernelInventorySyncState_Ready;
    client_inventory_resync_pending_.erase(packet.inventory_container_id);
    client_inventory_snapshot_assemblies_.erase(packet.inventory_container_id);
}

void KernelEngine::handle_client_inventory_delta_batch(
    const InventoryDeltaBatchPacket& packet) {
    const InventoryContainerRecord* container =
        item_store_.find_container(packet.inventory_container_id);
    if (container == nullptr) {
        client_inventory_sync_states_[packet.inventory_container_id] =
            KernelInventorySyncState_Desynced;
        ++network_stats_.inventory_revision_gap_count;
        request_inventory_snapshot(packet.inventory_container_id, 0u);
        return;
    }
    const std::uint64_t last_revision = packet.first_revision +
        static_cast<std::uint64_t>(packet.records.size()) - 1u;
    if (last_revision <= container->revision) return;
    if (packet.first_revision != container->revision + 1u) {
        client_inventory_sync_states_[packet.inventory_container_id] =
            KernelInventorySyncState_Desynced;
        ++network_stats_.inventory_revision_gap_count;
        request_inventory_snapshot(
            packet.inventory_container_id, container->revision);
        return;
    }
    std::vector<KernelInventoryDelta> deltas;
    deltas.reserve(packet.records.size());
    for (std::size_t index = 0; index < packet.records.size(); ++index) {
        const InventoryDeltaRecord& record = packet.records[index];
        KernelInventoryDelta delta{};
        delta.struct_size = sizeof(delta);
        delta.inventory_container_id = packet.inventory_container_id;
        delta.revision = packet.first_revision + index;
        delta.type = static_cast<std::uint8_t>(record.type);
        delta.slot = record.slot;
        delta.previous_slot = record.previous_slot;
        delta.changed_fields = record.changed_fields;
        if (record.type == KernelInventoryDeltaType_Add) {
            if (!inventory_view_from_wire(
                    item_store_,
                    record.item,
                    packet.inventory_container_id,
                    record.slot,
                    &delta.item)) {
                request_inventory_snapshot(
                    packet.inventory_container_id, container->revision);
                return;
            }
        } else {
            delta.item = item_store_.item_view(record.item.item_instance_id);
            if (delta.item.item_instance_id == 0u) {
                request_inventory_snapshot(
                    packet.inventory_container_id, container->revision);
                return;
            }
            if (record.type == KernelInventoryDeltaType_Update) {
                InventoryWireItem merged = inventory_wire_item(delta.item);
                if ((record.changed_fields & kInventoryChangeQuantity) != 0u) {
                    merged.quantity = record.item.quantity;
                }
                if ((record.changed_fields & kInventoryChangeCooldown) != 0u) {
                    merged.next_use_tick = record.item.next_use_tick;
                }
                if ((record.changed_fields &
                     kInventoryChangePortableState) != 0u) {
                    merged.portable_values = record.item.portable_values;
                }
                if (!inventory_view_from_wire(
                        item_store_,
                        merged,
                        packet.inventory_container_id,
                        record.slot,
                        &delta.item)) {
                    request_inventory_snapshot(
                        packet.inventory_container_id, container->revision);
                    return;
                }
            }
        }
        deltas.push_back(delta);
    }
    if (!item_store_.apply_replica_deltas(
            packet.inventory_container_id, deltas)) {
        client_inventory_sync_states_[packet.inventory_container_id] =
            KernelInventorySyncState_Desynced;
        request_inventory_snapshot(
            packet.inventory_container_id, container->revision);
        return;
    }
    client_inventory_sync_states_[packet.inventory_container_id] =
        KernelInventorySyncState_Ready;
}

void KernelEngine::handle_client_status_effect_state(
    const StatusEffectStatePacket& packet) {
    if (packet.target_net_id == 0u || packet.target_net_id != local_player_net_id_) {
        return;
    }
    const auto existing = client_status_effect_states_.find(packet.target_net_id);
    if (existing != client_status_effect_states_.end() &&
        static_cast<std::int32_t>(packet.revision - existing->second.revision) <= 0) {
        return;
    }
    StatusEffectState next;
    next.revision = packet.revision;
    std::unordered_set<std::uint32_t> channels;
    next.active.reserve(packet.records.size());
    for (const StatusEffectStateRecord& record : packet.records) {
        const RuntimeStatusEffectTemplate* status_template =
            world_.find_status_effect_template(record.status_effect_id);
        if (status_template == nullptr || status_template->channel_id == 0u ||
            record.stack_count == 0u ||
            record.stack_count > status_template->max_stacks ||
            !channels.insert(status_template->channel_id).second) {
            return;
        }
        next.active.push_back(ActiveStatusEffect{
            record.status_instance_id,
            record.status_effect_id,
            status_template->channel_id,
            record.instigator_net_id,
            0u,
            record.applied_tick,
            record.expire_tick,
            0u,
            record.stack_count,
        });
    }
    const StatusEffectState empty;
    const StatusEffectState& old = existing == client_status_effect_states_.end()
        ? empty
        : existing->second;
    const auto emit_transition = [&](const ActiveStatusEffect& active,
                                     std::uint8_t event_type) {
        const RuntimeStatusEffectTemplate* status_template =
            world_.find_status_effect_template(active.status_effect_id);
        if (status_template == nullptr) {
            return;
        }
        remote_action_presentation_events_.push_back(
            KernelRemoteActionPresentationEvent{
                packet.target_net_id,
                0u,
                0u,
                1u,
                1u,
                event_type,
                0u,
                0u,
                active.status_effect_id,
                active.instance_id,
                active.channel_id,
                status_template->duration_ticks,
                active.stack_count,
                0u,
            });
    };
    for (const ActiveStatusEffect& active : old.active) {
        if (std::none_of(
                next.active.begin(),
                next.active.end(),
                [&](const ActiveStatusEffect& candidate) {
                    return candidate.instance_id == active.instance_id;
                })) {
            emit_transition(
                active, KernelRemoteActionPresentationEventType_StatusRemoved);
        }
    }
    for (const ActiveStatusEffect& active : next.active) {
        const auto previous = std::find_if(
            old.active.begin(),
            old.active.end(),
            [&](const ActiveStatusEffect& candidate) {
                return candidate.instance_id == active.instance_id;
            });
        if (previous == old.active.end()) {
            emit_transition(
                active, KernelRemoteActionPresentationEventType_StatusApplied);
        } else if (previous->stack_count != active.stack_count ||
                   previous->expire_tick != active.expire_tick ||
                   previous->source != active.source) {
            emit_transition(
                active, KernelRemoteActionPresentationEventType_StatusUpdated);
        }
    }
    client_status_effect_states_[packet.target_net_id] = std::move(next);
}

void KernelEngine::handle_client_reliable_event(const TransportEvent& transport_event) {
    StatusEffectStatePacket status_effect_state;
    auto decode_start = std::chrono::steady_clock::now();
    if (decode_status_effect_state_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &status_effect_state)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_status_effect_state(status_effect_state);
        return;
    }
    PropStateChangeBatchPacket prop_state_batch;
    decode_start = std::chrono::steady_clock::now();
    if (decode_prop_state_change_batch_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &prop_state_batch)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_prop_state_change_batch(prop_state_batch);
        return;
    }
    InventorySnapshotPagePacket inventory_snapshot;
    decode_start = std::chrono::steady_clock::now();
    if (decode_inventory_snapshot_page_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &inventory_snapshot)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_inventory_snapshot_page(inventory_snapshot);
        return;
    }
    InventoryDeltaBatchPacket inventory_delta;
    decode_start = std::chrono::steady_clock::now();
    if (decode_inventory_delta_batch_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &inventory_delta)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_inventory_delta_batch(inventory_delta);
        return;
    }
    KernelGameplayRequestOutcome gameplay_outcome{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_gameplay_request_outcome_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &gameplay_outcome)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        pending_gameplay_request_outcomes_.push_back(gameplay_outcome);
        return;
    }

    LocalActionResultBatchPacket local_action_results{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_local_action_result_batch_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &local_action_results)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_local_action_results(local_action_results);
        return;
    }

    KernelEvent event{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_reliable_event_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &event)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        if (event.presentation_time_us != 0) {
            pending_presentation_events_.push_back(event);
            return;
        }
        events_.push_back(event);
        return;
    }

    EntitySpawnPacket spawn{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_entity_spawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &spawn)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_spawn(spawn);
        return;
    }

    EntityDespawnPacket despawn{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_entity_despawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &despawn)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_despawn(despawn);
        return;
    }

    EntityTemplateUpdatePacket template_update{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_entity_template_update_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &template_update)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_template_update(template_update);
        return;
    }

    ProjectileSpawnBatchPacket projectile_batch{};
    decode_start = std::chrono::steady_clock::now();
    if (decode_projectile_spawn_batch_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &projectile_batch)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_projectile_spawn_batch(projectile_batch);
        return;
    }

    record_packet_deserialization_cost(elapsed_cost_us(decode_start));
    push_event(KernelEventType_Error, 0, transport_event.peer, 14);
}

void KernelEngine::handle_client_local_action_results(
    const LocalActionResultBatchPacket& packet) {
    for (const KernelLocalActionResult& result : packet.records) {
        const auto applied = applied_local_action_results_.find(
            result.action_instance_id);
        if (applied != applied_local_action_results_.end() &&
            (result.confirmed_commit_count <
                 applied->second.confirmed_commit_count ||
             (result.confirmed_commit_count ==
                  applied->second.confirmed_commit_count &&
              result.authoritative_tick <=
                  applied->second.authoritative_tick))) {
            if (network_stats_enabled()) {
                ++network_stats_.local_action_result_client_duplicates_dropped;
            }
            continue;
        }
        auto outstanding = outstanding_predicted_actions_.find(result.action_instance_id);
        if (outstanding != outstanding_predicted_actions_.end()) {
            if (result.confirmed_commit_count <
                    outstanding->second.confirmed_commit_count ||
                (result.confirmed_commit_count ==
                     outstanding->second.confirmed_commit_count &&
                 result.authoritative_tick == 0u)) {
                if (network_stats_enabled()) {
                    ++network_stats_.local_action_result_client_duplicates_dropped;
                }
                continue;
            }
            if (detailed_network_stats_enabled() &&
                client_local_time_us_ >= outstanding->second.last_activity_us) {
                const std::uint64_t latency =
                    client_local_time_us_ - outstanding->second.last_activity_us;
                ++network_stats_.local_action_result_latency_sample_count;
                network_stats_.local_action_result_latency_us_total += latency;
                network_stats_.local_action_result_latency_us_max = std::max(
                    network_stats_.local_action_result_latency_us_max,
                    latency);
            }
            outstanding->second.confirmed_commit_count =
                result.confirmed_commit_count;
            outstanding->second.last_activity_us = client_local_time_us_;
            const KernelActionTemplateDefinition* action_template =
                find_action_template(
                    action_templates_,
                    outstanding->second.action_template_id);
            std::size_t result_weapon_slot = kWeaponSlotCount;
            if (const auto actor = world_.find_entity(local_player_net_id_);
                actor.has_value() &&
                world_.registry().all_of<WeaponState>(*actor)) {
                result_weapon_slot = find_weapon_slot(
                    world_.registry().get<WeaponState>(*actor),
                    outstanding->second.weapon_id);
            }
            if (outstanding->second.binding_id ==
                    KernelActionBinding_PrimaryFire &&
                result_weapon_slot <
                    predicted_next_primary_commit_tick_.size()) {
                if (result.result == KernelLocalActionResultType_Accepted &&
                    action_template != nullptr) {
                    outstanding->second.last_authoritative_commit_tick =
                        result.authoritative_tick;
                    predicted_next_primary_commit_tick_[
                        result_weapon_slot] =
                        result.authoritative_tick +
                        action_template->commit_interval_ticks;
                } else if (
                    result.result == KernelLocalActionResultType_Rejected ||
                    result.result == KernelLocalActionResultType_Corrected) {
                    if (result.confirmed_commit_count == 0u) {
                        predicted_next_primary_commit_tick_[
                            result_weapon_slot] =
                            outstanding->second.primary_gate_before;
                    } else if (
                        action_template != nullptr &&
                        outstanding->second.last_authoritative_commit_tick !=
                            0u) {
                        predicted_next_primary_commit_tick_[
                            result_weapon_slot] =
                            outstanding->second.last_authoritative_commit_tick +
                            action_template->commit_interval_ticks;
                    }
                }
            }
        }

        const auto duplicate = std::find_if(
            local_action_results_.begin(),
            local_action_results_.end(),
            [&result](const KernelLocalActionResult& existing) {
                return existing.action_instance_id == result.action_instance_id &&
                       existing.confirmed_commit_count ==
                           result.confirmed_commit_count &&
                       existing.result == result.result &&
                       existing.authoritative_tick == result.authoritative_tick;
            });
        if (duplicate != local_action_results_.end()) {
            if (network_stats_enabled()) {
                ++network_stats_.local_action_result_client_duplicates_dropped;
            }
            continue;
        }

        const bool terminal =
            result.result == KernelLocalActionResultType_Rejected ||
            result.result == KernelLocalActionResultType_Corrected;
        const bool baseline_covers_result =
            has_client_snapshot_ &&
            latest_client_snapshot_.header.server_tick >= result.authoritative_tick;
        if (terminal) {
            for (PendingPredictionInput& pending : pending_prediction_inputs_) {
                KernelPlayerInput& input = pending.input;
                if (input.action_intent.action_instance_id ==
                    result.action_instance_id) {
                    input.action_intent = KernelActionIntent{};
                }
                if (input.action_input.action_instance_id ==
                    result.action_instance_id) {
                    input.action_input = KernelActionInput{};
                }
            }
            if (!baseline_covers_result &&
                outstanding != outstanding_predicted_actions_.end()) {
                if (const auto actor = world_.find_entity(local_player_net_id_);
                    actor.has_value() &&
                    world_.registry().all_of<WeaponState>(*actor)) {
                    WeaponState& weapon = world_.registry().get<WeaponState>(*actor);
                    const std::size_t slot =
                        find_weapon_slot(weapon, outstanding->second.weapon_id);
                    if (slot < weapon.weapon_slot_count) {
                        weapon.ammo[slot] =
                            outstanding->second.ammo_before;
                    }
                    weapon.active_effect_net_id =
                        outstanding->second.active_effect_before;
                }
            }
            predicted_projectiles_.erase(
                std::remove_if(
                    predicted_projectiles_.begin(),
                    predicted_projectiles_.end(),
                    [&result](const PredictedProjectile& projectile) {
                        return projectile.action_instance_id ==
                               result.action_instance_id;
                    }),
                predicted_projectiles_.end());
            if (predicted_local_entity_.action_instance_id ==
                result.action_instance_id) {
                if (!baseline_covers_result) {
                    predicted_local_entity_.action_template_id = 0u;
                    predicted_local_entity_.action_instance_id = 0u;
                    predicted_local_entity_.action_start_tick = 0u;
                    predicted_local_entity_.action_commit_count =
                        result.confirmed_commit_count;
                    predicted_local_entity_.action_phase = KernelActionPhase_None;
                    predicted_action_next_commit_tick_ = 0u;
                    predicted_action_recovery_end_tick_ = 0u;
                }
            }
            if (outstanding != outstanding_predicted_actions_.end()) {
                outstanding->second.pending_authoritative_tick =
                    result.authoritative_tick;
                outstanding->second.terminal_correction = true;
            }
        }
        local_action_results_.push_back(result);
        applied_local_action_results_[result.action_instance_id] = result;

        outstanding = outstanding_predicted_actions_.find(result.action_instance_id);
        if (outstanding == outstanding_predicted_actions_.end()) {
            continue;
        }
        bool completed_finite_action = false;
        if (result.result == KernelLocalActionResultType_Accepted) {
            const KernelActionTemplateDefinition* action_template =
                find_action_template(
                    action_templates_,
                    outstanding->second.action_template_id);
            completed_finite_action =
                action_template != nullptr &&
                action_template->max_commit_count != 0u &&
                result.confirmed_commit_count >=
                    action_template->max_commit_count;
        }
        if (completed_finite_action ||
            (terminal && baseline_covers_result)) {
            outstanding_predicted_actions_.erase(outstanding);
        }
    }
}

void KernelEngine::handle_client_remote_action_presentation(
    const TransportEvent& transport_event) {
    PacketHeader header{};
    if (!decode_packet_header(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &header)) {
        return;
    }
    if (has_remote_presentation_sequence_ &&
        static_cast<std::int32_t>(
            header.sequence - last_remote_presentation_sequence_) <= 0) {
        if (network_stats_enabled()) {
            ++network_stats_.remote_presentation_duplicate_dropped;
        }
        return;
    }
    RemoteActionPresentationBatchPacket packet{};
    const auto decode_start = std::chrono::steady_clock::now();
    if (!decode_remote_action_presentation_batch_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &packet)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        push_event(KernelEventType_Error, 0, transport_event.peer, 28);
        return;
    }
    record_packet_deserialization_cost(elapsed_cost_us(decode_start));
    has_remote_presentation_sequence_ = true;
    last_remote_presentation_sequence_ = header.sequence;

    const std::uint32_t expiry_ticks = std::max(
        1u,
        (config_.tick.server_tick_rate *
             config_.network_stats.remote_presentation_expiry_ms +
         999u) /
            1000u);
    remote_presentation_dedup_.erase(
        std::remove_if(
            remote_presentation_dedup_.begin(),
            remote_presentation_dedup_.end(),
            [packet](const RemotePresentationDedup& entry) {
                return entry.expire_tick < packet.server_tick;
            }),
        remote_presentation_dedup_.end());

    for (KernelRemoteActionPresentationEvent event : packet.records) {
        const RuntimeStatusEffectTemplate* status_template = nullptr;
        const bool status_transition =
            event.event_type == KernelRemoteActionPresentationEventType_StatusApplied ||
            event.event_type == KernelRemoteActionPresentationEventType_StatusRemoved ||
            event.event_type == KernelRemoteActionPresentationEventType_StatusUpdated;
        if (status_transition) {
            status_template =
                world_.find_status_effect_template(event.status_effect_id);
            if (status_template == nullptr || status_template->channel_id == 0u ||
                status_template->duration_ticks == 0u ||
                event.stack_count == 0u ||
                event.stack_count > status_template->max_stacks) {
                continue;
            }
            event.status_channel_id = status_template->channel_id;
            event.duration_ticks = status_template->duration_ticks;
        }
        const std::uint32_t event_tick =
            packet.server_tick >= event.server_tick_delta
                ? packet.server_tick - event.server_tick_delta
                : 0u;
        if (has_client_snapshot_ &&
            latest_client_snapshot_.header.server_tick > event_tick + expiry_ticks) {
            if (network_stats_enabled()) {
                network_stats_.remote_presentation_stale_dropped +=
                    event.commit_count;
            }
            continue;
        }
        if (status_transition && event.actor_net_id != local_player_net_id_) {
            StatusEffectState& state =
                client_status_effect_states_[event.actor_net_id];
            if (event.event_type ==
                KernelRemoteActionPresentationEventType_StatusRemoved) {
                state.active.erase(
                    std::remove_if(
                        state.active.begin(),
                        state.active.end(),
                        [&](const ActiveStatusEffect& active) {
                            return active.instance_id == event.status_instance_id;
                        }),
                    state.active.end());
            } else if (event.event_type ==
                       KernelRemoteActionPresentationEventType_StatusApplied) {
                state.active.erase(
                    std::remove_if(
                        state.active.begin(),
                        state.active.end(),
                        [&](const ActiveStatusEffect& active) {
                            return active.channel_id == event.status_channel_id ||
                                active.instance_id == event.status_instance_id;
                        }),
                    state.active.end());
                if (state.active.size() < kMaxActiveStatusEffects) {
                    state.active.push_back(ActiveStatusEffect{
                        event.status_instance_id,
                        event.status_effect_id,
                        event.status_channel_id,
                        0u,
                        0u,
                        event_tick,
                        event_tick + event.duration_ticks,
                        0u,
                        event.stack_count,
                    });
                }
            } else {
                auto active = std::find_if(
                    state.active.begin(),
                    state.active.end(),
                    [&](const ActiveStatusEffect& candidate) {
                        return candidate.instance_id == event.status_instance_id;
                    });
                if (active == state.active.end()) {
                    state.active.erase(
                        std::remove_if(
                            state.active.begin(),
                            state.active.end(),
                            [&](const ActiveStatusEffect& candidate) {
                                return candidate.channel_id ==
                                    event.status_channel_id;
                            }),
                        state.active.end());
                    if (state.active.size() < kMaxActiveStatusEffects) {
                        state.active.push_back(ActiveStatusEffect{
                            event.status_instance_id,
                            event.status_effect_id,
                            event.status_channel_id,
                            0u,
                            0u,
                            event_tick,
                            event_tick + event.duration_ticks,
                            0u,
                            event.stack_count,
                        });
                    }
                } else {
                    active->status_effect_id = event.status_effect_id;
                    active->channel_id = event.status_channel_id;
                    active->stack_count = event.stack_count;
                    if (status_template->replacement_policy ==
                            KernelStatusEffectReplacementPolicy_Refresh ||
                        (status_template->replacement_policy ==
                             KernelStatusEffectReplacementPolicy_Stack &&
                         status_template->refresh_on_stack)) {
                        active->expire_tick =
                            event_tick + event.duration_ticks;
                    }
                }
            }
        }
        KernelRemoteActionPresentationEvent unseen_range = event;
        unseen_range.commit_count = 0u;
        auto flush_unseen_range = [this, &packet, event_tick, expiry_ticks](
            const KernelRemoteActionPresentationEvent& range) {
            if (range.commit_count != 0u) {
                pending_remote_action_presentation_events_.push_back(
                    PendingRemotePresentation{
                        packet.server_tick,
                        event_tick + expiry_ticks,
                        range});
            }
        };
        for (std::uint32_t offset = 0; offset < event.commit_count; ++offset) {
            const std::uint16_t commit_index = static_cast<std::uint16_t>(
                event.first_commit_index + offset);
            const auto found = std::find_if(
                remote_presentation_dedup_.begin(),
                remote_presentation_dedup_.end(),
                [&event, commit_index, event_tick](
                    const RemotePresentationDedup& entry) {
                    return entry.actor_net_id == event.actor_net_id &&
                           entry.action_instance_id == event.action_instance_id &&
                           entry.status_instance_id == event.status_instance_id &&
                           entry.stack_count == event.stack_count &&
                           entry.commit_index == commit_index &&
                           entry.event_type == event.event_type &&
                           entry.event_tick == event_tick;
                });
            if (found != remote_presentation_dedup_.end()) {
                if (network_stats_enabled()) {
                    ++network_stats_.remote_presentation_duplicate_dropped;
                }
                flush_unseen_range(unseen_range);
                unseen_range.first_commit_index =
                    static_cast<std::uint16_t>(commit_index + 1u);
                unseen_range.commit_count = 0u;
                continue;
            }
            if (unseen_range.commit_count == 0u) {
                unseen_range.first_commit_index = commit_index;
            }
            ++unseen_range.commit_count;
            if (remote_presentation_dedup_.size() >=
                kRemotePresentationDedupCapacity) {
                remote_presentation_dedup_.erase(
                    remote_presentation_dedup_.begin());
            }
            remote_presentation_dedup_.push_back(RemotePresentationDedup{
                event.actor_net_id,
                event.action_instance_id,
                event.status_instance_id,
                event.stack_count,
                commit_index,
                event.event_type,
                event_tick,
                event_tick + expiry_ticks,
            });
        }
        flush_unseen_range(unseen_range);
    }
    release_remote_action_presentation_events();
}

void KernelEngine::handle_client_projectile_spawn_batch(
    const ProjectileSpawnBatchPacket& packet) {
    if (packet.catalog_hash != catalog_hash_) {
        push_event(KernelEventType_Error, 0, kServerPeerId, 22);
        return;
    }
    for (const ProjectileSpawnGroup& group : packet.groups) {
        const KernelProjectileTemplateDefinition* projectile_template =
            find_projectile_template(
                projectile_templates_,
                group.projectile_template_id);
        if (projectile_template == nullptr) {
            push_event(KernelEventType_Error, 0, kServerPeerId, 23);
            continue;
        }
        for (const ProjectileSpawnRecord& record : group.records) {
            if (record.projectile_net_id == 0) {
                continue;
            }
            auto replicated = std::find_if(
                client_replicated_entities_.begin(),
                client_replicated_entities_.end(),
                [&record](const ClientReplicatedEntity& entity) {
                    return entity.net_id == record.projectile_net_id;
                });
            if (replicated == client_replicated_entities_.end()) {
                ClientReplicatedEntity entity{};
                entity.net_id = record.projectile_net_id;
                entity.type = EntityType::kProjectile;
                entity.owner_peer = record.owner_peer;
                entity.owner_net_id = record.owner_net_id;
                entity.projectile_template_id =
                    projectile_template->projectile_template_id;
                entity.collider_template_id =
                    projectile_template->mechanics.collider_template_id;
                entity.position = record.spawn_position;
                entity.velocity = record.initial_velocity;
                entity.snapshot_tick = packet.server_tick;
                client_replicated_entities_.push_back(entity);
            } else {
                replicated->type = EntityType::kProjectile;
                replicated->actor_type = ActorType::kUnknown;
                replicated->owner_peer = record.owner_peer;
                replicated->owner_net_id = record.owner_net_id;
                replicated->projectile_template_id =
                    projectile_template->projectile_template_id;
                replicated->collider_template_id =
                    projectile_template->mechanics.collider_template_id;
                replicated->position = record.spawn_position;
                replicated->rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            }
            client_metadata_timeout_reported_entities_.erase(record.projectile_net_id);
            if (projectile_template->mechanics.sync_mode ==
                KernelProjectileSyncMode_ServerSnapshotOnly) {
                continue;
            }
            PredictedProjectile* predicted = find_predicted_projectile(
                record.owner_peer, record.action_instance_id);
            if (predicted != nullptr && !predicted->bound) {
                predicted->entity_id = entity_id_for_net_id(record.projectile_net_id);
                predicted->net_id = record.projectile_net_id;
                predicted->spawn_tick = packet.server_tick;
                predicted->projectile_template_id =
                    projectile_template->projectile_template_id;
                predicted->collider_template_id =
                    projectile_template->mechanics.collider_template_id;
                predicted->weapon_id = projectile_template->weapon_id;
                predicted->sync_mode = projectile_template->mechanics.sync_mode;
                predicted->bound = true;
            } else if (has_predicted_projectile_net_id(record.projectile_net_id)) {
                continue;
            } else {
                const glm::vec3 spawn_position = record.spawn_position;
                const glm::vec3 initial_velocity = record.initial_velocity;
                predicted_projectiles_.push_back(PredictedProjectile{
                    entity_id_for_net_id(record.projectile_net_id),
                    record.projectile_net_id,
                    record.owner_peer,
                    0,
                    record.action_instance_id,
                    packet.server_tick,
                    0,
                    spawn_position,
                    glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                    initial_velocity,
                    spawn_position,
                    initial_velocity,
                    from_kernel_vec3(projectile_template->mechanics.gravity),
                    to_projectile_motion_model(
                        projectile_template->mechanics.motion_model),
                    projectile_template->mechanics.lifetime_ticks,
                    projectile_template->projectile_template_id,
                    projectile_template->mechanics.collider_template_id,
                    projectile_template->weapon_id,
                    projectile_template->mechanics.sync_mode,
                    glm::vec3{0.0f, 0.0f, 0.0f},
                    false,
                    false,
                });
            }

            const glm::vec3 spawn_position = record.spawn_position;
            const glm::vec3 initial_velocity = record.initial_velocity;

            KernelDebugInfo debug_info{};
            debug_info.struct_size = sizeof(KernelDebugInfo);
            debug_info.tick = packet.server_tick;
            debug_info.record_type = KernelDebugRecordType_Projectile;
            debug_info.data.projectile.projectile_net_id = record.projectile_net_id;
            debug_info.data.projectile.owner_net_id = record.owner_net_id;
            debug_info.data.projectile.owner_peer = record.owner_peer;
            debug_info.data.projectile.weapon_id = projectile_template->weapon_id;
            debug_info.data.projectile.motion_model =
                projectile_template->mechanics.motion_model;
            debug_info.data.projectile.sync_mode =
                projectile_template->mechanics.sync_mode;
            debug_info.data.projectile.position = to_kernel_vec3(spawn_position);
            debug_info.data.projectile.velocity = to_kernel_vec3(initial_velocity);
            debug_records_.push_back(debug_info);
        }
    }
    rebuild_render_states();
}

void KernelEngine::handle_client_ping_pong(const TransportEvent& transport_event) {
    PingPongPacket ping;
    if (!decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &ping)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 18);
        return;
    }

    ping.client_receive_time_us = client_local_action_time_us();
    apply_client_clock_offset_sample(
        time_delta_us(ping.server_send_time_us, ping.client_receive_time_us));
    if (ping.server_rtt_us != 0) {
        if (network_stats_enabled()) {
            network_stats_.rtt_us = ping.server_rtt_us;
            network_stats_.jitter_us = ping.server_jitter_us;
        }
    }
    ping.client_send_time_us = client_local_action_time_us();
    const std::vector<std::uint8_t> packet =
        encode_ping_pong_packet(ping, next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, kServerPeerId, 19);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession);
    }
}

bool KernelEngine::apply_welcome(const WelcomePacket& welcome) {
    if (welcome.actor_blocking_mode == KernelActorBlockingMode_Predicted) {
        if (!prepare_prediction_physics()) {
            spdlog::error(
                "rejecting predicted actor-blocking session before input: "
                "collision catalog artifact is not ready");
            return false;
        }
    } else if (!static_collision_scene_.empty() &&
               !prepare_prediction_physics()) {
        spdlog::error(
            "rejecting session: configured prediction collision artifact "
            "could not be loaded");
        return false;
    }
    if (has_welcome_ && local_player_net_id_ != 0u &&
        local_player_net_id_ != welcome.assigned_player_net_id) {
        clear_client_action_sync_state();
    }
    local_client_peer_id_ = welcome.assigned_peer_id;
    local_player_net_id_ = welcome.assigned_player_net_id;
    catalog_version_ = welcome.catalog_version;
    catalog_hash_ = welcome.catalog_hash;
    session_rules_.actor_blocking_mode = welcome.actor_blocking_mode;
    if (gameplay_catalog_sync_state_ ==
        KernelGameplayCatalogSyncState_Handshaking) {
        gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Ready;
        gameplay_catalog_sync_error_ = KernelGameplayCatalogSyncError_None;
        downloaded_gameplay_catalog_bundle_.clear();
    }

    TickConfig server_tick = config_.tick;
    if (welcome.server_tick_rate != 0) {
        server_tick.server_tick_rate = welcome.server_tick_rate;
    }
    if (welcome.snapshot_rate != 0) {
        server_tick.snapshot_rate = welcome.snapshot_rate;
    }
    config_.tick = with_tick_defaults(server_tick);
    tick_loop_.reset(config_.tick, welcome.server_tick);
    history_buffer_ = HistoryBuffer(history_frame_count(config_.tick));
    has_welcome_ = true;
    return true;
}

void KernelEngine::apply_client_clock_offset_sample(std::int64_t sample_offset_us) {
    if (!has_client_clock_sync_) {
        client_clock_offset_us_ = sample_offset_us;
        has_client_clock_sync_ = true;
        return;
    }

    const std::int64_t delta = sample_offset_us - client_clock_offset_us_;
    client_clock_offset_us_ += static_cast<std::int64_t>(
        static_cast<double>(delta) * kClientClockOffsetSmoothingFactor);
}

void KernelEngine::handle_server_ping_pong(const TransportEvent& transport_event) {
    PingPongPacket pong;
    if (!decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &pong)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 18);
        return;
    }

    PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr || session->pending_clock_sync_nonce == 0 ||
        session->pending_clock_sync_nonce != pong.nonce) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 20);
        return;
    }

    const std::uint64_t server_receive_time_us = current_server_time_us();
    const auto server_send_to_client_receive =
        static_cast<std::int64_t>(session->pending_clock_sync_server_time_us) -
        static_cast<std::int64_t>(pong.client_receive_time_us);
    const auto server_receive_to_client_send =
        static_cast<std::int64_t>(server_receive_time_us) -
        static_cast<std::int64_t>(pong.client_send_time_us);
    session->clock_offset_us =
        (server_send_to_client_receive + server_receive_to_client_send) / 2;
    const std::uint64_t server_elapsed =
        server_receive_time_us >= session->pending_clock_sync_server_time_us
            ? server_receive_time_us - session->pending_clock_sync_server_time_us
            : 0;
    const std::uint64_t client_elapsed =
        pong.client_send_time_us >= pong.client_receive_time_us
            ? pong.client_send_time_us - pong.client_receive_time_us
            : 0;
    const std::uint64_t rtt_us =
        server_elapsed >= client_elapsed ? server_elapsed - client_elapsed : 0;
    if (session->last_clock_sync_rtt_us != 0) {
        session->last_clock_sync_jitter_us =
            session->last_clock_sync_rtt_us > rtt_us
                ? session->last_clock_sync_rtt_us - rtt_us
                : rtt_us - session->last_clock_sync_rtt_us;
    }
    session->last_clock_sync_rtt_us = rtt_us;
    if (network_stats_enabled()) {
        network_stats_.rtt_us = session->last_clock_sync_rtt_us;
        network_stats_.jitter_us = session->last_clock_sync_jitter_us;
    }
    session->pending_clock_sync_nonce = 0;
    session->pending_clock_sync_server_time_us = 0;
    session->has_clock_sync = true;
}

void KernelEngine::handle_client_spawn(const EntitySpawnPacket& packet) {
    const auto tombstone = client_despawned_entities_.find(packet.net_id);
    if (tombstone != client_despawned_entities_.end() &&
        (tombstone->second.reason == KernelDespawnReason_OutOfRange ||
         packet.server_tick > tombstone->second.tick)) {
        client_despawned_entities_.erase(tombstone);
    } else if (tombstone != client_despawned_entities_.end()) {
        return;
    }
    auto found = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&packet](const ClientReplicatedEntity& entity) {
            return entity.net_id == packet.net_id;
        });
    if (found == client_replicated_entities_.end()) {
        ClientReplicatedEntity entity{};
        entity.net_id = packet.net_id;
        entity.type = packet.entity_type;
        entity.actor_type = packet.actor_type;
        entity.owner_peer = packet.owner_peer;
        entity.actor_template_id = packet.actor_template_id;
        entity.entity_template_id = packet.entity_template_id;
        entity.collider_template_id = packet.collider_template_id;
        entity.item_template_id = packet.item_template_id;
        entity.item_instance_id = packet.item_instance_id;
        entity.world_item_mode = packet.world_item_mode;
        entity.carrier_entity_id = packet.carrier_entity_id;
        entity.position = packet.position;
        entity.rotation = packet.rotation;
        entity.snapshot_tick = packet.server_tick;
        client_replicated_entities_.push_back(entity);
    } else {
        found->type = packet.entity_type;
        found->actor_type = packet.actor_type;
        found->owner_peer = packet.owner_peer;
        found->actor_template_id = packet.actor_template_id;
        found->entity_template_id = packet.entity_template_id;
        found->collider_template_id = packet.collider_template_id;
        found->item_template_id = packet.item_template_id;
        found->item_instance_id = packet.item_instance_id;
        found->world_item_mode = packet.world_item_mode;
        found->carrier_entity_id = packet.carrier_entity_id;
        found->position = packet.position;
        found->rotation = packet.rotation;
        found->active = false;
    }
    client_metadata_timeout_reported_entities_.erase(packet.net_id);
    if (has_client_snapshot_) {
        sync_client_vision_states_from_snapshot(latest_client_snapshot_);
        rebuild_render_states();
    }
    events_.push_back(KernelEvent{
        KernelEventType_EntitySpawned,
        packet.server_tick,
        packet.net_id,
        packet.owner_peer,
        static_cast<std::uint32_t>(packet.entity_type),
    });
}

void KernelEngine::handle_client_template_update(
    const EntityTemplateUpdatePacket& packet) {
    if (packet.net_id == 0 ||
        packet.actor_template_id == 0u ||
        find_actor_template(actor_templates_, packet.actor_template_id) == nullptr) {
        push_event(KernelEventType_Error, packet.net_id, kServerPeerId, 26);
        return;
    }
    auto found = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&packet](const ClientReplicatedEntity& entity) {
            return entity.net_id == packet.net_id;
        });
    if (found == client_replicated_entities_.end()) {
        ClientReplicatedEntity entity{};
        entity.net_id = packet.net_id;
        entity.type = EntityType::kActor;
        entity.actor_template_id = packet.actor_template_id;
        entity.snapshot_tick = packet.server_tick;
        client_replicated_entities_.push_back(entity);
    } else {
        found->actor_template_id = packet.actor_template_id;
    }
    client_metadata_timeout_reported_entities_.erase(packet.net_id);
    if (has_client_snapshot_) {
        sync_client_vision_states_from_snapshot(latest_client_snapshot_);
        rebuild_render_states();
    }
}

void KernelEngine::handle_client_despawn(const EntityDespawnPacket& packet) {
    client_status_effect_states_.erase(packet.net_id);
    client_despawned_entities_[packet.net_id] = ClientEntityTombstone{
        packet.server_tick,
        packet.reason,
    };
    const KernelEntityLifecycleEventType lifecycle_type =
        lifecycle_type_for_despawn_reason(packet.reason);
    std::uint16_t entity_type = 0;
    std::uint16_t actor_type = 0;
    const auto replicated = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&packet](const ClientReplicatedEntity& entity) {
            return entity.net_id == packet.net_id;
        });
    if (replicated != client_replicated_entities_.end()) {
        entity_type = static_cast<std::uint16_t>(replicated->type);
        actor_type = static_cast<std::uint16_t>(replicated->actor_type);
    }
    client_replicated_entities_.erase(
        std::remove_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&packet](const ClientReplicatedEntity& entity) {
                return entity.net_id == packet.net_id;
            }),
        client_replicated_entities_.end());
    const auto proxy = prediction_proxy_collider_ids_.find(packet.net_id);
    if (proxy != prediction_proxy_collider_ids_.end()) {
        if (prediction_physics_world_ != nullptr) {
            prediction_physics_world_->remove_object(proxy->second);
        }
        prediction_proxy_collider_ids_.erase(proxy);
    }
    const auto obstacle =
        prediction_obstacle_collider_ids_.find(packet.net_id);
    if (obstacle != prediction_obstacle_collider_ids_.end()) {
        if (prediction_physics_world_ != nullptr) {
            prediction_physics_world_->remove_object(obstacle->second);
        }
        prediction_obstacle_collider_ids_.erase(obstacle);
    }
    remove_prediction_limb_proxies(packet.net_id);
    client_metadata_timeout_reported_entities_.erase(packet.net_id);
    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [&packet](const PredictedProjectile& projectile) {
                if (projectile.net_id == packet.net_id &&
                    packet.reason == KernelDespawnReason_OutOfRange &&
                    projectile.sync_mode ==
                        KernelProjectileSyncMode_LocalPredictedDeterministic) {
                    return false;
                }
                return projectile.net_id == packet.net_id;
            }),
        predicted_projectiles_.end());
    events_.push_back(KernelEvent{
        KernelEventType_EntityDestroyed,
        packet.server_tick,
        packet.net_id,
        0,
        packet.reason,
    });
    lifecycle_events_.push_back(KernelEntityLifecycleEvent{
        lifecycle_type,
        packet.server_tick,
        packet.net_id,
        packet.reason,
        entity_type,
        actor_type,
        0,
    });
}

void KernelEngine::clear_client_action_sync_state() {
    pending_prediction_inputs_.clear();
    predicted_projectiles_.clear();
    outstanding_predicted_actions_.clear();
    applied_local_action_results_.clear();
    pending_presentation_events_.clear();
    local_action_results_.clear();
    remote_action_presentation_events_.clear();
    pending_remote_action_presentation_events_.clear();
    remote_presentation_dedup_.clear();
    client_status_effect_states_.clear();
    predicted_local_entity_.action_template_id = 0u;
    predicted_local_entity_.action_instance_id = 0u;
    predicted_local_entity_.action_start_tick = 0u;
    predicted_local_entity_.action_commit_count = 0u;
    predicted_local_entity_.action_phase = KernelActionPhase_None;
    predicted_action_buttons_ = 0u;
    predicted_action_binding_id_ = 0u;
    predicted_action_weapon_id_ = 0u;
    predicted_action_next_commit_tick_ = 0u;
    predicted_action_recovery_end_tick_ = 0u;
    predicted_next_primary_commit_tick_.fill(0u);
    last_remote_presentation_sequence_ = 0;
    has_remote_presentation_sequence_ = false;
}

void KernelEngine::clear_client_session() {
    if (prediction_physics_world_ != nullptr && local_player_net_id_ != 0u) {
        prediction_physics_world_->remove_character(local_player_net_id_);
    }
    for (const auto& [net_id, collider_id] : prediction_proxy_collider_ids_) {
        (void)net_id;
        if (prediction_physics_world_ != nullptr) {
            prediction_physics_world_->remove_object(collider_id);
        }
    }
    prediction_proxy_collider_ids_.clear();
    for (const auto& [net_id, collider_id] :
         prediction_obstacle_collider_ids_) {
        (void)net_id;
        if (prediction_physics_world_ != nullptr) {
            prediction_physics_world_->remove_object(collider_id);
        }
    }
    prediction_obstacle_collider_ids_.clear();
    while (!prediction_limb_collider_ids_.empty()) {
        remove_prediction_limb_proxies(
            prediction_limb_collider_ids_.begin()->first);
    }
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    clear_client_action_sync_state();
    predicted_projectile_collision_warning_emitted_ = false;
    lifecycle_events_.clear();
    client_snapshot_buffer_.clear();
    client_replicated_entities_.clear();
    client_metadata_timeout_reported_entities_.clear();
    client_despawned_entities_.clear();
    latest_client_snapshot_ = WorldSnapshot{};
    predicted_local_entity_ = EntitySnapshot{};
    has_authoritative_local_entity_ = false;
    latest_client_input_ = KernelPlayerInput{};
    pending_client_action_intents_.clear();
    latest_client_input_time_us_ = 0;
    next_client_input_seq_ = 1;
    latest_client_input_peer_ = 0;
    has_latest_client_input_ = false;
    predicted_character_state_ = movement_solver::CharacterMovementState{};
    predicted_character_tick_ = 0;
    predicted_impulse_lockout_until_tick_ = 0u;
    predicted_impulse_lockout_armed_tick_ = 0u;
    local_presentation_position_ = glm::vec3{0.0f, 0.0f, 0.0f};
    local_presentation_velocity_ = glm::vec3{0.0f, 0.0f, 0.0f};
    has_local_presentation_position_ = false;
    predicted_local_state_time_us_ = 0;
    client_clock_offset_us_ = 0;
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    has_client_clock_sync_ = false;
    has_client_render_time_ = false;
    current_render_time_us_ = 0;
    render_states_.clear();
}

void KernelEngine::release_presentable_events() {
    if (!has_client_render_time_ || pending_presentation_events_.empty()) {
        return;
    }

    std::vector<KernelEvent> still_pending;
    still_pending.reserve(pending_presentation_events_.size());
    for (const KernelEvent& event : pending_presentation_events_) {
        if (event.presentation_time_us <= current_render_time_us_) {
            events_.push_back(event);
        } else {
            still_pending.push_back(event);
        }
    }
    pending_presentation_events_ = std::move(still_pending);
}

void KernelEngine::release_remote_action_presentation_events() {
    if (has_client_snapshot_) {
        const std::uint32_t current_tick =
            latest_client_snapshot_.header.server_tick;
        for (auto& [net_id, state] : client_status_effect_states_) {
            if (net_id == local_player_net_id_) {
                continue;
            }
            for (std::size_t index = 0u; index < state.active.size();) {
                const ActiveStatusEffect active = state.active[index];
                if (current_tick < active.expire_tick) {
                    ++index;
                    continue;
                }
                const RuntimeStatusEffectTemplate* status_template =
                    world_.find_status_effect_template(active.status_effect_id);
                remote_action_presentation_events_.push_back(
                    KernelRemoteActionPresentationEvent{
                        net_id,
                        0u,
                        0u,
                        1u,
                        1u,
                        KernelRemoteActionPresentationEventType_StatusRemoved,
                        0u,
                        0u,
                        active.status_effect_id,
                        active.instance_id,
                        active.channel_id,
                        status_template == nullptr
                            ? 0u
                            : status_template->duration_ticks,
                        active.stack_count,
                        0u,
                    });
                state.active.erase(state.active.begin() + index);
            }
        }
    }
    if (pending_remote_action_presentation_events_.empty()) {
        return;
    }
    std::vector<PendingRemotePresentation> still_pending;
    still_pending.reserve(pending_remote_action_presentation_events_.size());
    const std::uint64_t render_server_time_us =
        client_clock_offset_us_ >= 0
            ? current_render_time_us_ +
                  static_cast<std::uint64_t>(client_clock_offset_us_)
            : current_render_time_us_ >
                      static_cast<std::uint64_t>(-client_clock_offset_us_)
                  ? current_render_time_us_ -
                        static_cast<std::uint64_t>(-client_clock_offset_us_)
                  : 0u;
    for (const PendingRemotePresentation& pending :
         pending_remote_action_presentation_events_) {
        const std::uint64_t tick_duration_us = std::max<std::uint64_t>(
            1u,
            tick_time_us(1u, tick_loop_.fixed_delta_seconds()));
        const std::uint32_t render_tick = static_cast<std::uint32_t>(
            render_server_time_us / tick_duration_us);
        if (has_client_render_time_ && render_tick > pending.expire_tick) {
            if (network_stats_enabled()) {
                network_stats_.remote_presentation_stale_dropped +=
                    pending.event.commit_count;
            }
            continue;
        }
        const std::uint32_t event_tick =
            pending.batch_server_tick >= pending.event.server_tick_delta
                ? pending.batch_server_tick - pending.event.server_tick_delta
                : 0u;
        const std::uint64_t event_time_us =
            tick_time_us(event_tick, tick_loop_.fixed_delta_seconds());
        if (!has_client_render_time_ || event_time_us <= render_server_time_us) {
            remote_action_presentation_events_.push_back(pending.event);
        } else {
            still_pending.push_back(pending);
        }
    }
    pending_remote_action_presentation_events_ = std::move(still_pending);
}

void KernelEngine::poll_client_transport() {
    if (listen_server_transport_ == nullptr) {
        return;
    }

    TransportEvent transport_event;
    while (listen_server_transport_->PollLocalClientEvent(transport_event)) {
        if (transport_event.type != TransportEventType::kMessage) {
            continue;
        }
        record_received_packet_sequence(transport_event);
        if (transport_event.channel == ChannelId::kReliableEvent) {
            handle_client_reliable_event(transport_event);
            continue;
        }
        if (transport_event.channel == ChannelId::kPresentation) {
            handle_client_remote_action_presentation(transport_event);
            continue;
        }
        if (transport_event.channel != ChannelId::kSnapshot) {
            continue;
        }

        LocomotionStepBatchPacket locomotion_steps;
        if (decode_locomotion_step_batch_packet(
                transport_event.payload.data(),
                transport_event.payload.size(),
                &locomotion_steps)) {
            handle_client_locomotion_step_batch(locomotion_steps);
            continue;
        }

        WorldSnapshot snapshot;
        const auto decode_start = std::chrono::steady_clock::now();
        if (!decode_snapshot_packet(
                transport_event.payload.data(),
                transport_event.payload.size(),
                &snapshot)) {
            record_packet_deserialization_cost(elapsed_cost_us(decode_start));
            log_snapshot_decode_failure(transport_event);
            push_event(KernelEventType_Error, 0, transport_event.peer, 6);
            continue;
        }
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_snapshot(std::move(snapshot));
    }
}

void KernelEngine::handle_client_snapshot(WorldSnapshot snapshot) {
    // Before anything reads or stores it: beams arrive as a bare reach and are
    // not renderable until they have been hung off their shooter. Both the
    // interpolation buffer and latest_client_snapshot_ are fed from here, and
    // interpolation needs every frame it blends to be resolved already.
    resolve_client_beam_geometry(&snapshot);
    if (has_client_snapshot_ &&
        snapshot.header.server_tick < latest_client_snapshot_.header.server_tick) {
        store_client_snapshot(std::move(snapshot));
        return;
    }
    latest_client_snapshot_ = snapshot;
    has_client_snapshot_ = true;
    for (const EntitySnapshot& entity : latest_client_snapshot_.entities) {
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == entity.net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        const bool newer_than_prop_state = !replicated->has_prop_state ||
            static_cast<std::int32_t>(
                latest_client_snapshot_.header.server_tick -
                replicated->prop_state_tick) > 0;
        if (newer_than_prop_state) {
            replicated->position = entity.position;
            replicated->rotation = entity.rotation;
            replicated->velocity = entity.velocity;
        }
        if (entity.type == EntityType::kActor) {
            replicated->aim_direction = entity.aim_direction;
        }
        replicated->snapshot_tick = latest_client_snapshot_.header.server_tick;
        replicated->active = true;
    }
    store_client_snapshot(std::move(snapshot));
    diagnose_client_snapshot_metadata_waits();
    if (prediction_failed_) {
        return;
    }
    sync_client_vision_states_from_snapshot(latest_client_snapshot_);
    reconcile_local_prediction(latest_client_snapshot_);
    reconcile_predicted_projectiles(latest_client_snapshot_);
}

void KernelEngine::sync_client_vision_states_from_snapshot(
    const WorldSnapshot& snapshot) {
    if (config_.mode != KernelMode_Client) {
        return;
    }
    vision_states_.clear();
    for (const EntitySnapshot& entity : snapshot.entities) {
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& replicated_entity) {
                return replicated_entity.net_id == entity.net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        const KernelActorTemplateDefinition* actor_template =
            find_actor_template(actor_templates_, replicated->actor_template_id);
        if (actor_template == nullptr ||
            actor_template->vision.vision_collider_template_id == 0u) {
            continue;
        }
        const glm::quat rotation = entity.rotation;
        const glm::vec3 origin =
            entity.position +
            rotation * from_kernel_vec3(actor_template->vision.local_origin);
        const glm::vec3 configured_forward =
            from_kernel_vec3(actor_template->vision.local_forward);
        const glm::vec3 forward =
            glm::length(configured_forward) > 0.0001f
                ? glm::normalize(rotation * configured_forward)
                : glm::vec3{1.0f, 0.0f, 0.0f};
        KernelVisionStateView view{};
        view.struct_size = sizeof(KernelVisionStateView);
        view.agent_net_id = entity.net_id;
        view.entity_type = static_cast<std::uint16_t>(entity.type);
        view.actor_type = static_cast<std::uint8_t>(entity.actor_type);
        view.camp = actor_template->vision.camp;
        view.vision_origin = to_kernel_vec3(origin);
        view.vision_forward = to_kernel_vec3(forward);
        view.vision_collider_template_id =
            actor_template->vision.vision_collider_template_id;
        view.resolved_collider_template_id = actor_template->collider_template_id;
        view.valid = 1u;
        vision_states_[entity.net_id].view = view;
    }
}

void KernelEngine::resolve_client_beam_geometry(WorldSnapshot* snapshot) const {
    if (snapshot == nullptr) {
        return;
    }
    for (EntitySnapshot& entity : snapshot->entities) {
        if ((entity.state_flags & kSnapshotStateFlagProjectileBeam) == 0u) {
            continue;
        }
        const auto beam = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == entity.net_id;
            });
        // The shooter comes from the projectile spawn batch, which is reliable
        // and sent right behind the beam's EntitySpawn on the same channel, so
        // it cannot be missing once the beam is known at all. A beam a snapshot
        // mentions before either has landed is left at zero reach rather than
        // drawn full length through the world origin.
        glm::vec3 shooter_position{0.0f, 0.0f, 0.0f};
        glm::vec3 shooter_aim{0.0f, 0.0f, 0.0f};
        bool resolved = false;
        if (beam != client_replicated_entities_.end() &&
            beam->owner_net_id != 0u) {
            const auto shooter_frame = std::find_if(
                snapshot->entities.begin(),
                snapshot->entities.end(),
                [&beam](const EntitySnapshot& candidate) {
                    return candidate.net_id == beam->owner_net_id;
                });
            if (shooter_frame != snapshot->entities.end()) {
                shooter_position = shooter_frame->position;
                shooter_aim = shooter_frame->aim_direction;
                resolved = true;
            } else {
                // Relevance can cull the shooter while its beam is still in
                // range -- the beam is a separate entity with its own distance
                // test. Its last replicated aim beats no beam at all.
                const auto shooter = std::find_if(
                    client_replicated_entities_.begin(),
                    client_replicated_entities_.end(),
                    [&beam](const ClientReplicatedEntity& candidate) {
                        return candidate.net_id == beam->owner_net_id;
                    });
                if (shooter != client_replicated_entities_.end()) {
                    shooter_position = shooter->position;
                    shooter_aim = shooter->aim_direction;
                    resolved = true;
                }
            }
        }
        if (!resolved) {
            entity.beam_effective_length = 0.0f;
            continue;
        }
        // The same muzzle the server fired from, and the same axis mapping
        // simulate_beams writes onto the beam's transform, so the reconstructed
        // pose matches what used to be replicated outright. The one difference
        // is lag compensation: the server rewinds this origin to the shooter's
        // historical hitbox, which the client cannot see.
        entity.position = projectile_launch_position(Transform{shooter_position});
        entity.rotation = beam_rotation(shooter_aim);
    }
}

void KernelEngine::store_client_snapshot(WorldSnapshot snapshot) {
    auto existing = std::find_if(
        client_snapshot_buffer_.begin(),
        client_snapshot_buffer_.end(),
        [&snapshot](const WorldSnapshot& buffered) {
            return buffered.header.server_tick == snapshot.header.server_tick;
        });
    if (existing != client_snapshot_buffer_.end()) {
        *existing = std::move(snapshot);
    } else {
        client_snapshot_buffer_.push_back(std::move(snapshot));
    }

    std::sort(
        client_snapshot_buffer_.begin(),
        client_snapshot_buffer_.end(),
        [](const WorldSnapshot& lhs, const WorldSnapshot& rhs) {
            return lhs.header.server_tick < rhs.header.server_tick;
        });
    constexpr std::size_t kMaxClientSnapshots = 32;
    if (client_snapshot_buffer_.size() > kMaxClientSnapshots) {
        client_snapshot_buffer_.erase(
            client_snapshot_buffer_.begin(),
            client_snapshot_buffer_.begin() +
                static_cast<std::ptrdiff_t>(
                    client_snapshot_buffer_.size() - kMaxClientSnapshots));
    }
}

bool KernelEngine::client_snapshot_entity_is_tombstoned(
    NetId net_id,
    std::uint32_t snapshot_tick) const {
    const auto tombstone = client_despawned_entities_.find(net_id);
    if (tombstone == client_despawned_entities_.end()) {
        return false;
    }
    return tombstone->second.reason != KernelDespawnReason_OutOfRange ||
        snapshot_tick <= tombstone->second.tick;
}

bool KernelEngine::snapshot_entity_has_required_metadata(
    const EntitySnapshot& entity) const {
    if (entity.net_id == 0) {
        return true;
    }
    if (has_predicted_local_entity_ && entity.net_id == local_player_net_id_ &&
        session_rules_.actor_blocking_mode !=
            KernelActorBlockingMode_Predicted) {
        return true;
    }
    if (has_predicted_projectile_net_id(entity.net_id)) {
        return true;
    }
    const auto replicated = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&entity](const ClientReplicatedEntity& replicated_entity) {
            return replicated_entity.net_id == entity.net_id;
        });
    if (entity.type == EntityType::kActor) {
        if (replicated == client_replicated_entities_.end() ||
            replicated->actor_template_id == 0u) {
            return false;
        }
        if (session_rules_.actor_blocking_mode !=
            KernelActorBlockingMode_Predicted) {
            return true;
        }
        const auto entity_template = std::find_if(
            entity_templates_.begin(),
            entity_templates_.end(),
            [replicated](const KernelEntityTemplateDefinition& definition) {
                return definition.actor_template_id ==
                    replicated->actor_template_id;
            });
        if (entity_template == entity_templates_.end()) {
            return false;
        }
        if (entity_template->movement.controller_type !=
            KernelMovementControllerType_Character) {
            return true;
        }
        const KernelColliderTemplateDefinition* collider =
            find_collider_template(
                collider_templates_,
                entity_template->movement.movement_collider_template_id);
        return collider != nullptr &&
               collider->shape_type == KernelColliderShapeType_Capsule &&
               (collider->purpose_flags & KernelColliderPurpose_Movement) != 0u;
    }
    if (entity.type == EntityType::kProjectile) {
        return replicated != client_replicated_entities_.end() &&
               replicated->projectile_template_id != 0u &&
               replicated->collider_template_id != 0u;
    }
    return true;
}

void KernelEngine::diagnose_client_snapshot_metadata_waits() {
    if (config_.mode != KernelMode_Client || !has_client_snapshot_) {
        return;
    }
    const std::uint32_t latest_tick = latest_client_snapshot_.header.server_tick;
    client_snapshot_buffer_.erase(
        std::remove_if(
            client_snapshot_buffer_.begin(),
            client_snapshot_buffer_.end(),
            [this, latest_tick](const WorldSnapshot& snapshot) {
                if (latest_tick <= snapshot.header.server_tick ||
                    latest_tick - snapshot.header.server_tick <=
                        kClientSnapshotMetadataGraceTicks) {
                    return false;
                }

                bool missing_metadata = false;
                for (const EntitySnapshot& entity : snapshot.entities) {
                    if (client_snapshot_entity_is_tombstoned(
                            entity.net_id,
                            snapshot.header.server_tick) ||
                        snapshot_entity_has_required_metadata(entity)) {
                        continue;
                    }
                    missing_metadata = true;
                    if (client_metadata_timeout_reported_entities_
                            .insert(entity.net_id)
                            .second) {
                        if (network_stats_enabled()) {
                            ++network_stats_.replication_metadata_timeout_count;
                        }
                        spdlog::warn(
                            "[NetworkExample] client snapshot metadata timeout "
                            "net_id={} snapshot_tick={} latest_tick={} grace_ticks={}",
                            entity.net_id,
                            snapshot.header.server_tick,
                            latest_tick,
                            kClientSnapshotMetadataGraceTicks);
                    }
                }
                if (missing_metadata &&
                    kDropStaleClientSnapshotsMissingMetadata) {
                    if (network_stats_enabled()) {
                        ++network_stats_.replication_stale_snapshot_drop_count;
                    }
                    return true;
                }
                return false;
            }),
        client_snapshot_buffer_.end());
    if (session_rules_.actor_blocking_mode ==
            KernelActorBlockingMode_Predicted &&
        !client_metadata_timeout_reported_entities_.empty()) {
        fail_client_prediction(
            "remote actor movement metadata timed out before prediction acceptance");
    }
}

bool KernelEngine::build_local_character_movement_config(
    movement_solver::CharacterMovementConfig* out_config) {
    if (out_config == nullptr || local_player_net_id_ == 0u) {
        return false;
    }
    const auto replicated = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [this](const ClientReplicatedEntity& entity) {
            return entity.net_id == local_player_net_id_;
        });
    if (replicated == client_replicated_entities_.end() ||
        replicated->actor_template_id == 0u) {
        return false;
    }
    const auto entity_template = std::find_if(
        entity_templates_.begin(),
        entity_templates_.end(),
        [replicated](const KernelEntityTemplateDefinition& definition) {
            return definition.actor_template_id == replicated->actor_template_id;
        });
    if (entity_template == entity_templates_.end() ||
        entity_template->movement.controller_type !=
            KernelMovementControllerType_Character) {
        return false;
    }
    const KernelColliderTemplateDefinition* collider = find_collider_template(
        collider_templates_,
        entity_template->movement.movement_collider_template_id);
    if (collider == nullptr ||
        collider->shape_type != KernelColliderShapeType_Capsule ||
        (collider->purpose_flags & KernelColliderPurpose_Movement) == 0u) {
        return false;
    }

    movement_solver::CharacterMovementConfig config{};
    config.character_id = local_player_net_id_;
    config.shape.type = physics::CollisionShapeType::kCapsule;
    config.shape.local_center = from_kernel_vec3(collider->center);
    config.shape.capsule_half_height = collider->shape_params.x;
    config.shape.radius = collider->shape_params.y;
    config.gravity = from_kernel_vec3(entity_template->movement.gravity);
    config.max_slope_degrees = entity_template->movement.max_slope_degrees;
    config.step_height = entity_template->movement.step_height;
    config.ground_snap_distance =
        entity_template->movement.ground_snap_distance;
    // Built the same way the server builds it in player_movement.cc: the
    // authored mask, with zero meaning the engine default, then the actor layer
    // struck out when the session does not block actors. This used to hardcode
    // terrain|static_obstacle and ignore movement.collision_mask outright, which
    // silently gave the predicted local player a different set of blockers from
    // the authoritative one the moment a player template authored a mask.
    const std::uint32_t authored_mask =
        entity_template->movement.movement_collision_mask;
    config.filter.collision_mask = authored_mask == 0u
        ? physics::kMovementCollisionMask
        : authored_mask;
    if (session_rules_.actor_blocking_mode !=
        KernelActorBlockingMode_Predicted) {
        // Limbs go with the capsule rather than surviving on their own: they
        // are the same claim -- that another actor's body blocks this one --
        // made about a different shape, and leaving them on in a session that
        // does not block actors would stop the player on a leg while walking
        // through the body it hangs from.
        config.filter.collision_mask &= ~(
            physics::collision_layer_bit(
                physics::CollisionLayer::kActorMovement) |
            physics::collision_layer_bit(physics::CollisionLayer::kActorLimb));
    }
    config.filter.ignored_entity_net_id = local_player_net_id_;
    local_player_move_speed_meters_per_second_ =
        entity_template->combat.move_speed_meters_per_second;
    *out_config = config;
    return true;
}

bool KernelEngine::sync_prediction_actor_proxies(
    const WorldSnapshot& snapshot,
    std::uint32_t prediction_tick) {
    if (prediction_physics_world_ == nullptr ||
        session_rules_.actor_blocking_mode !=
            KernelActorBlockingMode_Predicted) {
        return true;
    }

    std::unordered_set<NetId> current_proxies;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.net_id == local_player_net_id_ ||
            entity.type != EntityType::kActor ||
            client_snapshot_entity_is_tombstoned(
                entity.net_id,
                snapshot.header.server_tick)) {
            continue;
        }
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == entity.net_id;
            });
        if (replicated == client_replicated_entities_.end() ||
            replicated->actor_template_id == 0u) {
            return false;
        }
        const auto entity_template = std::find_if(
            entity_templates_.begin(),
            entity_templates_.end(),
            [replicated](const KernelEntityTemplateDefinition& definition) {
                return definition.actor_template_id ==
                    replicated->actor_template_id;
            });
        if (entity_template == entity_templates_.end()) {
            return false;
        }
        if (entity_template->movement.controller_type !=
            KernelMovementControllerType_Character ||
            // Same symmetry the server applies in sync_physics_colliders. Left
            // out of current_proxies, so an existing proxy is retired below.
            !movement_capsule_blocks_other_actors(
                entity_template->movement.movement_collision_mask)) {
            continue;
        }
        const KernelColliderTemplateDefinition* collider = find_collider_template(
            collider_templates_,
            entity_template->movement.movement_collider_template_id);
        if (collider == nullptr ||
            collider->shape_type != KernelColliderShapeType_Capsule ||
            (collider->purpose_flags & KernelColliderPurpose_Movement) == 0u) {
            return false;
        }

        auto proxy = prediction_proxy_collider_ids_.find(entity.net_id);
        if (proxy == prediction_proxy_collider_ids_.end()) {
            proxy = prediction_proxy_collider_ids_
                        .emplace(
                            entity.net_id,
                            next_prediction_proxy_collider_id_++)
                        .first;
        }
        const std::uint32_t extrapolated_ticks =
            prediction_tick > snapshot.header.server_tick
                ? std::min<std::uint32_t>(
                      3u, prediction_tick - snapshot.header.server_tick)
                : 0u;
        const glm::vec3 position =
            entity.position +
            entity.velocity *
                (static_cast<float>(extrapolated_ticks) *
                 tick_loop_.fixed_delta_seconds());
        physics::CollisionObjectDescriptor object{};
        object.identity.entity_net_id = entity.net_id;
        object.identity.collider_id = proxy->second;
        object.identity.kind = physics::CollisionObjectKind::kActorMovement;
        object.identity.layer = physics::CollisionLayer::kActorMovement;
        object.identity.gameplay_category = collider->layer_mask;
        object.shape.type = physics::CollisionShapeType::kCapsule;
        object.shape.capsule_half_height = collider->shape_params.x;
        object.shape.radius = collider->shape_params.y;
        object.position =
            position + entity.rotation * from_kernel_vec3(collider->center);
        object.rotation = entity.rotation;
        std::string error;
        if (!prediction_physics_world_->upsert_object(object, &error)) {
            spdlog::error(
                "failed to update prediction actor proxy net_id={}: {}",
                entity.net_id,
                error);
            return false;
        }
        current_proxies.insert(entity.net_id);
    }

    for (auto proxy = prediction_proxy_collider_ids_.begin();
         proxy != prediction_proxy_collider_ids_.end();) {
        if (current_proxies.contains(proxy->first)) {
            ++proxy;
            continue;
        }
        prediction_physics_world_->remove_object(proxy->second);
        proxy = prediction_proxy_collider_ids_.erase(proxy);
    }

    // Same reasoning as in sync_client_render_colliders(): actor proxies come
    // and go as snapshots arrive, and nothing else rebuilds this world's tree.
    prediction_physics_world_->optimize_broad_phase();
    return true;
}

bool KernelEngine::step_local_character_prediction(
    const KernelPlayerInput& input,
    std::uint32_t prediction_tick) {
    if (prediction_physics_world_ == nullptr) {
        return session_rules_.actor_blocking_mode ==
            KernelActorBlockingMode_Disabled;
    }
    if (!has_local_presentation_position_ && has_predicted_local_entity_) {
        local_presentation_position_ =
            predicted_local_simulation_position(client_local_time_us_);
        local_presentation_velocity_ = predicted_local_entity_.velocity;
        has_local_presentation_position_ = true;
    }
    // Before the capsules, and on this clock rather than the render frame's:
    // the character step below is the only thing that reads them, so they have
    // to be current as of this prediction tick. A replay of several pending
    // inputs reuses one pose for all of them -- there is only ever one solved
    // pose to hand, and unlike an actor capsule a limb has no velocity to
    // extrapolate along.
    sync_prediction_limb_proxies();
    movement_solver::CharacterMovementConfig movement_config{};
    if (!build_local_character_movement_config(&movement_config) ||
        !sync_prediction_actor_proxies(
            latest_client_snapshot_, prediction_tick)) {
        return false;
    }
    // The twin of player_movement.cc's desired_horizontal: while the impulse
    // lockout stands the authority keeps the actor's current horizontal
    // velocity instead of rebuilding it from input, so the prediction has to
    // do exactly the same or it walks away from the authority for N ticks and
    // is snapped back at reconciliation.
    const bool impulse_locked =
        prediction_tick < predicted_impulse_lockout_until_tick_;
    const glm::vec3 desired_horizontal = impulse_locked
        ? glm::vec3{
              predicted_character_state_.velocity.x,
              0.0f,
              predicted_character_state_.velocity.z}
        : movement_solver::input_move_to_world(input) *
              local_player_move_speed_meters_per_second_;
    std::string error;
    if (!movement_solver::step_character(
            *prediction_physics_world_,
            movement_config,
            desired_horizontal,
            tick_loop_.fixed_delta_seconds(),
            &predicted_character_state_,
            &error)) {
        spdlog::error("client CharacterVirtual prediction step failed: {}", error);
        return false;
    }
    // Same rule as the authority: the landing release cannot fire on the tick
    // the impulse armed, or a flat knockback on a grounded actor releases
    // before it has held anything off.
    if (impulse_locked &&
        prediction_tick > predicted_impulse_lockout_armed_tick_ &&
        predicted_character_state_.ground_state ==
            physics::CharacterGroundState::kGrounded) {
        predicted_impulse_lockout_until_tick_ = 0u;
    }
    predicted_character_tick_ = prediction_tick;
    predicted_local_entity_.position = predicted_character_state_.position;
    predicted_local_entity_.rotation = predicted_character_state_.rotation;
    predicted_local_entity_.velocity = predicted_character_state_.velocity;
    predicted_local_state_time_us_ = client_local_time_us_;
    return true;
}

void KernelEngine::fail_client_prediction(std::string_view diagnostic) {
    if (prediction_failed_) {
        return;
    }
    spdlog::error("client prediction session failure: {}", diagnostic);
    push_event(KernelEventType_Error, local_player_net_id_, kServerPeerId, 31);
    clear_client_session();
    prediction_failed_ = true;
}

void KernelEngine::reconcile_local_prediction(const WorldSnapshot& snapshot) {
    if (local_player_net_id_ == 0) {
        return;
    }

    const EntitySnapshot* authoritative =
        find_snapshot_entity(snapshot, local_player_net_id_);
    if (authoritative == nullptr) {
        return;
    }
    if (session_rules_.actor_blocking_mode ==
        KernelActorBlockingMode_Predicted) {
        for (const EntitySnapshot& entity : snapshot.entities) {
            if (entity.type == EntityType::kActor &&
                !client_snapshot_entity_is_tombstoned(
                    entity.net_id,
                    snapshot.header.server_tick) &&
                !snapshot_entity_has_required_metadata(entity)) {
                return;
            }
        }
    }

    pending_prediction_inputs_.erase(
        std::remove_if(
            pending_prediction_inputs_.begin(),
            pending_prediction_inputs_.end(),
            [&snapshot](const PendingPredictionInput& pending) {
                return pending.input.input_seq <=
                    snapshot.header.last_processed_input_seq;
            }),
        pending_prediction_inputs_.end());
    std::sort(
        pending_prediction_inputs_.begin(),
        pending_prediction_inputs_.end(),
        [](const PendingPredictionInput& lhs,
           const PendingPredictionInput& rhs) {
            return lhs.prediction_tick != rhs.prediction_tick
                ? lhs.prediction_tick < rhs.prediction_tick
                : lhs.input.input_seq < rhs.input.input_seq;
        });

    if (!has_local_presentation_position_ && has_predicted_local_entity_) {
        local_presentation_position_ =
            predicted_local_simulation_position(client_local_time_us_);
        local_presentation_velocity_ = predicted_local_entity_.velocity;
        has_local_presentation_position_ = true;
    }
    predicted_local_entity_ = *authoritative;
    predicted_local_state_time_us_ = client_local_time_us_;
    has_predicted_local_entity_ = true;
    has_authoritative_local_entity_ = true;
    if (prediction_physics_world_ != nullptr) {
        if (!authoritative->has_authoritative_movement_state) {
            fail_client_prediction(
                "owner snapshot is missing authoritative movement state");
            return;
        }
        predicted_character_state_.position = authoritative->position;
        predicted_character_state_.rotation = authoritative->rotation;
        predicted_character_state_.velocity = authoritative->velocity;
        predicted_character_state_.ground_state =
            static_cast<physics::CharacterGroundState>(
                authoritative->ground_state);
        predicted_character_state_.ground_normal =
            authoritative->ground_normal;
        predicted_character_state_.supporting_identity.entity_net_id =
            authoritative->supporting_entity_net_id;
        predicted_character_state_.supporting_identity.collider_id =
            authoritative->supporting_collider_id;
        predicted_character_tick_ = snapshot.header.server_tick;
        movement_solver::CharacterMovementConfig movement_config{};
        std::string error;
        if (!build_local_character_movement_config(&movement_config)) {
            return;
        }
        if (!movement_solver::reset_character(
                *prediction_physics_world_, movement_config, &error)) {
            fail_client_prediction(
                error.empty() ? "CharacterVirtual reset failed" : error);
            return;
        }
    } else if (session_rules_.actor_blocking_mode ==
               KernelActorBlockingMode_Predicted) {
        fail_client_prediction("prediction PhysicsWorld is unavailable");
        return;
    }
    predicted_action_buttons_ = 0u;
    predicted_action_next_commit_tick_ = 0u;
    if (const KernelActionTemplateDefinition* action_template =
            find_action_template(
                action_templates_, authoritative->action_template_id)) {
        predicted_action_next_commit_tick_ =
            authoritative->action_start_tick +
            action_template->commit_offset_ticks +
            authoritative->action_commit_count *
                action_template->commit_interval_ticks;
    }
    for (const PendingPredictionInput& pending : pending_prediction_inputs_) {
        if (!step_local_character_prediction(
                pending.input, pending.prediction_tick)) {
            fail_client_prediction(
                "CharacterVirtual reset/replay step failed");
            return;
        }
        predict_local_action(pending.input);
    }

    for (auto outstanding = outstanding_predicted_actions_.begin();
         outstanding != outstanding_predicted_actions_.end();) {
        const auto applied = applied_local_action_results_.find(outstanding->first);
        if (applied == applied_local_action_results_.end() ||
            snapshot.header.server_tick < applied->second.authoritative_tick ||
            authoritative->action_instance_id == outstanding->first) {
            ++outstanding;
            continue;
        }
        if (outstanding->second.terminal_correction ||
            outstanding->second.confirmed_commit_count >=
                applied->second.confirmed_commit_count) {
            outstanding = outstanding_predicted_actions_.erase(outstanding);
        } else {
            ++outstanding;
        }
    }

    predicted_local_state_time_us_ = client_local_time_us_;
}

void KernelEngine::reconcile_predicted_projectiles(const WorldSnapshot& snapshot) {
    const auto cost_start = std::chrono::steady_clock::now();
    bool corrected_projectile = false;
    const float fixed_delta_seconds = tick_loop_.fixed_delta_seconds();
    const std::uint32_t local_tick =
        local_prediction_server_tick(snapshot.header.server_tick);
    const std::uint32_t fast_forward_ticks =
        local_tick > snapshot.header.server_tick
            ? local_tick - snapshot.header.server_tick
            : 0;
    const std::uint32_t max_homing_visual_ticks =
        fixed_delta_seconds > 0.0f
            ? static_cast<std::uint32_t>(
                  kMaxHomingVisualExtrapolationSeconds / fixed_delta_seconds)
            : 0u;
    const std::uint32_t snapshot_age_ticks = fast_forward_ticks;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.type != EntityType::kProjectile) {
            continue;
        }
        if (entity.action_instance_id == 0) {
            continue;
        }
        PredictedProjectile* predicted =
            find_predicted_projectile(entity.owner_peer, entity.action_instance_id);
        if (predicted == nullptr) {
            continue;
        }
        const std::uint32_t authoritative_age_ticks =
            predicted->motion_model == ProjectileMotionModel::kHoming
                ? std::min(snapshot_age_ticks, max_homing_visual_ticks)
                : snapshot_age_ticks;
        const float authoritative_age_duration =
            static_cast<float>(authoritative_age_ticks) * fixed_delta_seconds;
        const glm::vec3 previous_render_position =
            predicted->position + predicted->correction_offset;
        const glm::vec3 authoritative_now = projectile_position_at(
            entity.position,
            entity.velocity,
            predicted->motion_model,
            predicted->gravity,
            authoritative_age_duration);
        const glm::vec3 authoritative_velocity_now = projectile_velocity_at(
            entity.velocity,
            predicted->motion_model,
            predicted->gravity,
            authoritative_age_duration);
        predicted->net_id = entity.net_id;
        predicted->position = authoritative_now;
        predicted->rotation = entity.rotation;
        predicted->velocity = authoritative_velocity_now;
        predicted->spawn_position = entity.position;
        predicted->initial_velocity = entity.velocity;
        predicted->age_ticks = authoritative_age_ticks;
        predicted->spawn_tick = entity.spawn_tick;
        if (predicted->projectile_template_id == 0u ||
            predicted->collider_template_id == 0u) {
            const auto replicated = std::find_if(
                client_replicated_entities_.begin(),
                client_replicated_entities_.end(),
                [&entity](const ClientReplicatedEntity& replicated_entity) {
                    return replicated_entity.net_id == entity.net_id;
                });
            if (replicated != client_replicated_entities_.end()) {
                predicted->projectile_template_id =
                    replicated->projectile_template_id;
                predicted->collider_template_id = replicated->collider_template_id;
            }
        }
        predicted->bound = true;
        const glm::vec3 correction = previous_render_position - authoritative_now;
        predicted->correction_offset =
            glm::length(correction) > kPredictionCorrectionSnapDistanceMeters
                ? glm::vec3{0.0f, 0.0f, 0.0f}
                : correction;
        entity_ids_by_net_id_[entity.net_id] = predicted->entity_id;
        corrected_projectile = true;
    }

    if (corrected_projectile) {
        benchmark_stats_.hybrid_correction_cost_us +=
            std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
    }
}

void KernelEngine::predict_local_input(const KernelPlayerInput& input) {
    if (local_player_net_id_ == 0 || prediction_failed_) {
        return;
    }
    if (session_rules_.actor_blocking_mode ==
            KernelActorBlockingMode_Predicted &&
        has_client_snapshot_) {
        for (const EntitySnapshot& entity : latest_client_snapshot_.entities) {
            if (entity.type == EntityType::kActor &&
                !client_snapshot_entity_is_tombstoned(
                    entity.net_id,
                    latest_client_snapshot_.header.server_tick) &&
                !snapshot_entity_has_required_metadata(entity)) {
                return;
            }
        }
    }

    if (!has_predicted_local_entity_) {
        const EntitySnapshot* latest =
            find_snapshot_entity(latest_client_snapshot_, local_player_net_id_);
        if (latest == nullptr) {
            predicted_local_entity_.net_id = local_player_net_id_;
            predicted_local_entity_.type = EntityType::kActor;
            predicted_local_entity_.actor_type = ActorType::kPlayer;
        } else {
            predicted_local_entity_ = *latest;
        }
        has_predicted_local_entity_ = true;
        predicted_local_state_time_us_ = client_local_time_us_;
        predicted_character_state_.position = predicted_local_entity_.position;
        predicted_character_state_.rotation = predicted_local_entity_.rotation;
        predicted_character_state_.velocity = predicted_local_entity_.velocity;
        if (predicted_local_entity_.has_authoritative_movement_state) {
            predicted_character_state_.ground_state =
                static_cast<physics::CharacterGroundState>(
                    predicted_local_entity_.ground_state);
            predicted_character_state_.ground_normal =
                predicted_local_entity_.ground_normal;
        }
    }

    std::uint32_t prediction_tick = local_prediction_server_tick(
        has_client_snapshot_ ? latest_client_snapshot_.header.server_tick : 0u);
    if (prediction_tick <= predicted_character_tick_) {
        prediction_tick = predicted_character_tick_ + 1u;
    }
    if (prediction_physics_world_ != nullptr) {
        movement_solver::CharacterMovementConfig movement_config{};
        if (!build_local_character_movement_config(&movement_config)) {
            return;
        }
    }
    if (!step_local_character_prediction(input, prediction_tick)) {
        if (session_rules_.actor_blocking_mode ==
                KernelActorBlockingMode_Predicted ||
            prediction_physics_world_ != nullptr) {
            fail_client_prediction("immediate CharacterVirtual step failed");
        }
        return;
    }
    pending_prediction_inputs_.push_back(
        PendingPredictionInput{input, prediction_tick});
}

bool KernelEngine::predict_local_action(const KernelPlayerInput& input) {
    if (!has_predicted_local_entity_) {
        return false;
    }
    const auto actor = world_.find_entity(local_player_net_id_);
    if (!actor.has_value() ||
        !world_.registry().all_of<WeaponState>(*actor)) {
        return false;
    }
    const WeaponState& weapon_state =
        world_.registry().get<WeaponState>(*actor);
    predicted_local_entity_.aim_direction = input_aim_to_world(input);
    predicted_local_entity_.flags &=
        ~(kVisualFlagAiming | kVisualFlagFiring | kVisualFlagReloading);
    if ((input.buttons & InputButton_Aim) != 0u) {
        predicted_local_entity_.flags |= kVisualFlagAiming;
    }

    const std::uint32_t current_tick = tick_loop_.current_tick();
    if (predicted_local_entity_.action_phase == KernelActionPhase_Recovery &&
        current_tick >= predicted_action_recovery_end_tick_) {
        predicted_local_entity_.action_template_id = 0u;
        predicted_local_entity_.action_instance_id = 0u;
        predicted_local_entity_.action_phase = KernelActionPhase_None;
        predicted_local_entity_.action_start_tick = 0u;
        predicted_local_entity_.action_commit_count = 0u;
        predicted_action_next_commit_tick_ = 0u;
        predicted_action_binding_id_ = 0u;
    }

    if (predicted_local_entity_.action_phase == KernelActionPhase_None) {
        if (input.action_intent.action_instance_id == 0u) {
            return false;
        }
        const WeaponMechanicsDefinition* weapon =
            entity_weapon_mechanics(world_, local_player_net_id_, input.selected_weapon);
        if (weapon == nullptr) {
            return false;
        }
        const std::uint32_t action_template_id =
            input.action_intent.binding_id == KernelActionBinding_PrimaryFire
                ? weapon->fire_action_template_id
                : input.action_intent.binding_id == KernelActionBinding_Reload
                    ? weapon->reload_action_template_id
                    : 0u;
        const KernelActionTemplateDefinition* action_template =
            find_action_template(action_templates_, action_template_id);
        if (action_template == nullptr) {
            return false;
        }
        const std::size_t selected_slot =
            find_weapon_slot(weapon_state, input.selected_weapon);
        if (input.action_intent.binding_id ==
                KernelActionBinding_PrimaryFire &&
            (selected_slot >=
                 predicted_next_primary_commit_tick_.size() ||
             projected_primary_commit_is_blocked(
                 current_tick,
                 action_template->commit_offset_ticks,
                 predicted_next_primary_commit_tick_[
                     selected_slot]))) {
            return false;
        }
        predicted_local_entity_.action_template_id =
            action_template->action_template_id;
        predicted_local_entity_.action_instance_id =
            input.action_intent.action_instance_id;
        predicted_action_binding_id_ = input.action_intent.binding_id;
        predicted_action_weapon_id_ = input.selected_weapon;
        predicted_local_entity_.action_phase =
            action_template->commit_offset_ticks == 0u
                ? KernelActionPhase_Active
                : KernelActionPhase_Windup;
        predicted_local_entity_.action_start_tick = current_tick;
        predicted_action_next_commit_tick_ =
            current_tick + action_template->commit_offset_ticks;
        predicted_local_entity_.action_commit_count = 0u;
    }

    const KernelActionTemplateDefinition* action_template =
        find_action_template(
            action_templates_, predicted_local_entity_.action_template_id);
    if (action_template == nullptr) {
        return false;
    }

    bool committed = false;
    const bool cancel_on_release =
        input.action_input.action_instance_id ==
            predicted_local_entity_.action_instance_id &&
        input.action_input.held == 0u &&
        (action_template->flags &
         KernelActionTemplateFlag_CancelOnRelease) != 0u;
    if (cancel_on_release &&
        predicted_local_entity_.action_commit_count == 0u &&
        (action_template->flags &
         KernelActionTemplateFlag_CancelBeforeFirstCommit) != 0u) {
        predicted_local_entity_.action_phase = KernelActionPhase_Recovery;
        predicted_action_recovery_end_tick_ =
            current_tick + action_template->recovery_ticks;
    } else if (
        predicted_local_entity_.action_phase != KernelActionPhase_None &&
        predicted_local_entity_.action_phase != KernelActionPhase_Recovery &&
        current_tick >= predicted_action_next_commit_tick_) {
        predicted_local_entity_.action_phase = KernelActionPhase_Active;
        ++predicted_local_entity_.action_commit_count;
        predicted_action_next_commit_tick_ =
            current_tick + action_template->commit_interval_ticks;
        committed =
            predicted_action_binding_id_ == KernelActionBinding_PrimaryFire;
        const std::size_t committed_slot =
            find_weapon_slot(weapon_state, predicted_action_weapon_id_);
        if (committed &&
            committed_slot < predicted_next_primary_commit_tick_.size()) {
            predicted_next_primary_commit_tick_[committed_slot] =
                current_tick + action_template->commit_interval_ticks;
        }
        if ((action_template->max_commit_count != 0u &&
             predicted_local_entity_.action_commit_count >=
                 action_template->max_commit_count) ||
            cancel_on_release) {
            predicted_local_entity_.action_phase = KernelActionPhase_Recovery;
            predicted_action_recovery_end_tick_ =
                current_tick + action_template->recovery_ticks;
        }
    }
    if (predicted_action_binding_id_ == KernelActionBinding_PrimaryFire &&
        predicted_local_entity_.action_phase == KernelActionPhase_Active) {
        predicted_local_entity_.flags |= kVisualFlagFiring;
    }
    if (predicted_action_binding_id_ == KernelActionBinding_Reload &&
        predicted_local_entity_.action_phase != KernelActionPhase_None) {
        predicted_local_entity_.flags |= kVisualFlagReloading;
    }
    return committed;
}

void KernelEngine::predict_local_projectile(const KernelPlayerInput& input) {
    const std::uint32_t action_instance_id =
        predicted_local_entity_.action_instance_id;
    if (local_client_peer_id_ == 0 || action_instance_id == 0 ||
        find_predicted_projectile(local_client_peer_id_, action_instance_id) != nullptr) {
        return;
    }
    const WeaponMechanicsDefinition* weapon =
        entity_weapon_mechanics(world_, local_player_net_id_, input.selected_weapon);
    if (weapon == nullptr || weapon->mode != WeaponFireMode::kProjectile) {
        return;
    }
    const KernelProjectileTemplateDefinition* projectile_template =
        find_projectile_template(projectile_templates_, weapon->projectile_template_id);
    if (projectile_template == nullptr) {
        return;
    }
    const KernelProjectileMechanicsDefinition& mechanics =
        projectile_template->mechanics;
    const std::uint8_t sync_mode = mechanics.sync_mode;
    const std::uint32_t collider_template_id = mechanics.collider_template_id;
    if (sync_mode == KernelProjectileSyncMode_ServerSnapshotOnly) {
        return;
    }

    glm::vec3 player_position{0.0f, 0.0f, 0.0f};
    if (has_predicted_local_entity_) {
        player_position = predicted_local_entity_.position;
    } else if (
        const EntitySnapshot* latest =
            find_snapshot_entity(latest_client_snapshot_, local_player_net_id_)) {
        player_position = latest->position;
    }

    const glm::vec3 origin = player_position + glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 direction = input_aim_to_world(input);
    const glm::vec3 velocity = direction * mechanics.speed;
    const ProjectileMotionModel motion_model =
        to_projectile_motion_model(mechanics.motion_model);
    const glm::vec3 gravity = from_kernel_vec3(mechanics.gravity);
    predicted_projectiles_.push_back(PredictedProjectile{
        allocate_predicted_entity_id(),
        0,
        local_client_peer_id_,
        input.input_seq,
        action_instance_id,
        tick_loop_.current_tick(),
        0,
        origin,
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        velocity,
        origin,
        velocity,
        gravity,
        motion_model,
        mechanics.lifetime_ticks,
        weapon->projectile_template_id,
        collider_template_id,
        weapon->id,
        sync_mode,
        glm::vec3{0.0f, 0.0f, 0.0f},
        false,
        false,
    });
}

KernelPlayerInput KernelEngine::prepare_client_input(const KernelPlayerInput& input) {
    KernelPlayerInput input_to_send = input;
    if (input_to_send.client_action_time_us == 0) {
        input_to_send.client_action_time_us = client_local_action_time_us();
    }
    if ((input_to_send.action_intent.action_instance_id != 0u &&
         (input_to_send.action_intent.flags != 0u ||
          input_to_send.action_intent.reserved != 0u ||
          (input_to_send.action_intent.binding_id !=
               KernelActionBinding_PrimaryFire &&
           input_to_send.action_intent.binding_id !=
               KernelActionBinding_Reload))) ||
        (input_to_send.action_input.action_instance_id != 0u &&
         (input_to_send.action_input.flags != 0u ||
          input_to_send.action_input.reserved != 0u ||
          input_to_send.action_input.held > 1u))) {
        input_to_send.action_intent = KernelActionIntent{};
        input_to_send.action_input = KernelActionInput{};
        push_event(KernelEventType_Error, local_player_net_id_, local_client_peer_id_, 27);
    }
    return input_to_send;
}

std::uint64_t KernelEngine::client_local_action_time_us() const {
    return std::max<std::uint64_t>(1, client_local_time_us_);
}

std::uint64_t KernelEngine::current_server_time_us() const {
    return tick_time_us(tick_loop_.current_tick(), tick_loop_.fixed_delta_seconds());
}

std::uint64_t KernelEngine::convert_client_action_time_to_server_time(
    PeerId peer,
    std::uint64_t client_action_time_us,
    std::uint64_t received_server_time_us) const {
    if (client_action_time_us == 0) {
        return received_server_time_us;
    }

    const PeerSession* session = find_session(peer);
    if (session == nullptr && config_.mode == KernelMode_ListenServer &&
        local_listen_session_.peer == peer && local_listen_session_.welcomed) {
        session = &local_listen_session_;
    }
    if (session == nullptr || !session->has_clock_sync) {
        return received_server_time_us;
    }

    const auto converted =
        static_cast<std::int64_t>(client_action_time_us) + session->clock_offset_us;
    return converted <= 0 ? 0 : static_cast<std::uint64_t>(converted);
}

std::uint64_t KernelEngine::uncompensated_action_time_us(
    const QueuedInput& queued_input) const {
    if (queued_input.has_action_server_time) {
        return queued_input.action_server_time_us;
    }
    return queued_input.input.client_action_time_us == 0
               ? tick_time_us(
                     queued_input.received_server_tick,
                     tick_loop_.fixed_delta_seconds())
               : queued_input.input.client_action_time_us;
}

std::uint64_t KernelEngine::clamp_compensated_action_time_us(
    std::uint64_t action_server_time_us,
    std::uint64_t received_server_time_us) const {
    // Policy: intentionally clamp, rather than reject, action timestamps outside
    // the accepted compensation window.
    if (action_server_time_us >= received_server_time_us) {
        return received_server_time_us;
    }
    const std::uint64_t earliest_compensated_time =
        received_server_time_us > kMaxCompensationWindowUs
            ? received_server_time_us - kMaxCompensationWindowUs
            : 0;
    return std::max(action_server_time_us, earliest_compensated_time);
}

std::uint64_t KernelEngine::compensated_action_time_us(
    const QueuedInput& queued_input) const {
    const std::uint64_t received_server_time_us = tick_time_us(
        queued_input.received_server_tick,
        tick_loop_.fixed_delta_seconds());
    return clamp_compensated_action_time_us(
        uncompensated_action_time_us(queued_input),
        received_server_time_us);
}

bool KernelEngine::client_render_server_time_us(
    std::uint64_t client_render_time_us,
    std::uint64_t* out_server_time_us) const {
    if (out_server_time_us == nullptr || client_snapshot_buffer_.empty()) {
        return false;
    }
    const float fixed_delta_seconds = tick_loop_.fixed_delta_seconds();
    if (client_snapshot_buffer_.size() == 1) {
        *out_server_time_us = tick_time_us(
            client_snapshot_buffer_.back().header.server_tick,
            fixed_delta_seconds);
        return true;
    }

    const std::uint64_t interpolation_delay_us =
        tick_time_us(
            tick_loop_.snapshot_interval_ticks() * 2u,
            fixed_delta_seconds);
    // A listen server's client half shares the server's clock outright -- both
    // advance off the same update() -- so the offset is exactly zero rather
    // than an estimate. It never learns this the normal way: clock-sync pings
    // only go to peer_sessions_, which excludes local_listen_session_, and the
    // loopback poll drops everything that is not a snapshot, reliable event, or
    // presentation packet -- ChannelId::kSession, which carries ping/pong,
    // included. Without this the branch below quantises the render target to
    // whole snapshot ticks, so the root (and now the pose that follows it) step
    // instead of moving continuously.
    const bool shares_server_clock =
        config_.mode == KernelMode_ListenServer;
    std::uint64_t target_server_time_us = 0;
    if (has_client_clock_sync_ || shares_server_clock) {
        const std::uint64_t server_now_us = shares_server_clock
            ? client_render_time_us
            : offset_time_us(client_render_time_us, client_clock_offset_us_);
        target_server_time_us =
            server_now_us > interpolation_delay_us
                ? server_now_us - interpolation_delay_us
                : 0;
    } else {
        const std::uint32_t newest_tick =
            client_snapshot_buffer_.back().header.server_tick;
        const std::uint32_t interpolation_delay_ticks =
            tick_loop_.snapshot_interval_ticks() * 2u;
        const std::uint32_t target_tick =
            newest_tick > interpolation_delay_ticks
                ? newest_tick - interpolation_delay_ticks
                : client_snapshot_buffer_.front().header.server_tick;
        target_server_time_us =
            tick_time_us(target_tick, fixed_delta_seconds);
    }
    // Report the instant the snapshot interpolation will actually land on, ends
    // included: build_interpolated_snapshot_for_server_time clamps to the
    // buffer rather than extrapolating, and the skeleton pose has to be sampled
    // at the same instant the root ends up at, not the one we asked for.
    *out_server_time_us = std::clamp(
        target_server_time_us,
        tick_time_us(
            client_snapshot_buffer_.front().header.server_tick,
            fixed_delta_seconds),
        tick_time_us(
            client_snapshot_buffer_.back().header.server_tick,
            fixed_delta_seconds));
    return true;
}

bool KernelEngine::build_interpolated_snapshot(
    std::uint64_t client_render_time_us,
    WorldSnapshot* out_snapshot) const {
    if (out_snapshot == nullptr || client_snapshot_buffer_.empty()) {
        return false;
    }
    if (client_snapshot_buffer_.size() == 1) {
        *out_snapshot = client_snapshot_buffer_.back();
        return true;
    }

    std::uint64_t target_server_time_us = 0;
    if (!client_render_server_time_us(
            client_render_time_us,
            &target_server_time_us)) {
        return false;
    }

    return build_interpolated_snapshot_for_server_time(
        target_server_time_us,
        out_snapshot);
}

bool KernelEngine::build_interpolated_snapshot_for_server_time(
    std::uint64_t target_server_time_us,
    WorldSnapshot* out_snapshot) const {
    if (out_snapshot == nullptr || client_snapshot_buffer_.empty()) {
        return false;
    }
    if (client_snapshot_buffer_.size() == 1) {
        *out_snapshot = client_snapshot_buffer_.back();
        return true;
    }

    const float fixed_delta_seconds = tick_loop_.fixed_delta_seconds();
    const std::uint64_t oldest_time_us =
        tick_time_us(
            client_snapshot_buffer_.front().header.server_tick,
            fixed_delta_seconds);
    if (target_server_time_us <= oldest_time_us) {
        *out_snapshot = client_snapshot_buffer_.front();
        return true;
    }
    const std::uint64_t newest_time_us =
        tick_time_us(
            client_snapshot_buffer_.back().header.server_tick,
            fixed_delta_seconds);
    if (target_server_time_us >= newest_time_us) {
        *out_snapshot = client_snapshot_buffer_.back();
        return true;
    }

    const WorldSnapshot* from = &client_snapshot_buffer_.front();
    const WorldSnapshot* to = &client_snapshot_buffer_.back();
    for (std::size_t index = 0; index < client_snapshot_buffer_.size(); ++index) {
        const WorldSnapshot& snapshot = client_snapshot_buffer_[index];
        const std::uint64_t snapshot_time_us =
            tick_time_us(snapshot.header.server_tick, fixed_delta_seconds);
        if (snapshot_time_us <= target_server_time_us) {
            from = &snapshot;
        }
        if (snapshot_time_us >= target_server_time_us) {
            to = &snapshot;
            break;
        }
    }

    if (from->header.server_tick == to->header.server_tick) {
        *out_snapshot = *from;
        return true;
    }

    const std::uint64_t from_time_us =
        tick_time_us(from->header.server_tick, fixed_delta_seconds);
    const std::uint64_t to_time_us =
        tick_time_us(to->header.server_tick, fixed_delta_seconds);
    const float alpha =
        static_cast<float>(target_server_time_us - from_time_us) /
        static_cast<float>(to_time_us - from_time_us);
    WorldSnapshot interpolated;
    interpolated.header = to->header;
    interpolated.header.server_tick =
        tick_for_time_us(target_server_time_us, fixed_delta_seconds);
    interpolated.header.server_time_ms =
        static_cast<std::uint32_t>(target_server_time_us / 1000u);
    interpolated.entities.reserve(to->entities.size());
    for (const EntitySnapshot& from_entity : from->entities) {
        if (const EntitySnapshot* to_entity = find_snapshot_entity(*to, from_entity.net_id)) {
            interpolated.entities.push_back(
                interpolate_snapshot_entity(from_entity, *to_entity, alpha));
        } else {
            interpolated.entities.push_back(from_entity);
        }
    }
    for (const EntitySnapshot& to_entity : to->entities) {
        if (find_snapshot_entity(*from, to_entity.net_id) == nullptr) {
            interpolated.entities.push_back(to_entity);
        }
    }

    *out_snapshot = std::move(interpolated);
    return true;
}

void KernelEngine::advance_predicted_projectile_corrections(
    float delta_seconds) {
    if (delta_seconds <= 0.0f) {
        return;
    }

    const float decay = std::pow(
        0.5f,
        delta_seconds / kPredictionCorrectionHalfLifeSeconds);
    const auto decay_offset = [decay](glm::vec3* offset) {
        *offset *= decay;
        if (glm::length(*offset) < kPredictionCorrectionEpsilonMeters) {
            *offset = glm::vec3{0.0f, 0.0f, 0.0f};
        }
    };
    for (PredictedProjectile& projectile : predicted_projectiles_) {
        decay_offset(&projectile.correction_offset);
    }
}

glm::vec3 KernelEngine::predicted_local_simulation_position(
    std::uint64_t client_time_us) const {
    float extrapolation_seconds = 0.0f;
    if (client_time_us > predicted_local_state_time_us_) {
        extrapolation_seconds = std::min(
            static_cast<float>(
                client_time_us - predicted_local_state_time_us_) /
                1000000.0f,
            tick_loop_.fixed_delta_seconds());
    }
    return predicted_local_entity_.position +
        predicted_local_entity_.velocity * extrapolation_seconds;
}

void KernelEngine::advance_local_presentation(float delta_seconds) {
    if (!has_predicted_local_entity_ ||
        (has_welcome_ && !has_authoritative_local_entity_)) {
        local_presentation_position_ = glm::vec3{0.0f, 0.0f, 0.0f};
        local_presentation_velocity_ = glm::vec3{0.0f, 0.0f, 0.0f};
        has_local_presentation_position_ = false;
        return;
    }

    const glm::vec3 target =
        predicted_local_simulation_position(client_local_time_us_);
    if (!has_local_presentation_position_) {
        local_presentation_position_ = target;
        local_presentation_velocity_ = predicted_local_entity_.velocity;
        has_local_presentation_position_ = true;
        return;
    }
    if (delta_seconds <= 0.0f) {
        return;
    }
    if (glm::length(target - local_presentation_position_) >
        kPredictionCorrectionSnapDistanceMeters) {
        local_presentation_position_ = target;
        local_presentation_velocity_ = predicted_local_entity_.velocity;
        return;
    }

    const glm::vec3 velocity_displacement =
        local_presentation_velocity_ * delta_seconds;
    const glm::vec3 extrapolated_position =
        local_presentation_position_ + velocity_displacement;
    const float correction_fraction = 1.0f - std::pow(
        0.5f,
        delta_seconds / kPredictionCorrectionHalfLifeSeconds);
    glm::vec3 correction =
        (target - extrapolated_position) * correction_fraction;

    const float presentation_speed = glm::length(local_presentation_velocity_);
    if (presentation_speed >
        kPredictionPresentationMinSpeedMetersPerSecond) {
        const glm::vec3 movement_direction =
            local_presentation_velocity_ / presentation_speed;
        const float forward_displacement =
            glm::dot(velocity_displacement + correction, movement_direction);
        if (forward_displacement < 0.0f) {
            correction -= movement_direction * forward_displacement;
        }
    }

    local_presentation_position_ += velocity_displacement + correction;
    local_presentation_velocity_ = predicted_local_entity_.velocity;
}

glm::vec3 KernelEngine::predicted_local_render_position() const {
    return has_local_presentation_position_
        ? local_presentation_position_
        : predicted_local_simulation_position(client_local_time_us_);
}

void KernelEngine::append_predicted_local_render_state() {
    if (!has_predicted_local_entity_ ||
        (has_welcome_ && !has_authoritative_local_entity_)) {
        return;
    }

    EntitySnapshot local = predicted_local_entity_;
    local.position = predicted_local_render_position();
    RenderEntityState state = render_state_from_snapshot_entity(
        local,
        entity_id_for_net_id(local.net_id));
    const auto replicated = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [this](const ClientReplicatedEntity& replicated_entity) {
            return replicated_entity.net_id == local_player_net_id_;
        });
    if (state.entity_type == static_cast<std::uint16_t>(EntityType::kActor) &&
        replicated != client_replicated_entities_.end() &&
        replicated->actor_template_id != 0u) {
        state.template_id = replicated->actor_template_id;
        state.collider_template_id =
            collider_template_id_for_actor_template(state.template_id);
    }
    state.status = RenderEntityStatus_Predicted;
    render_states_.push_back(state);
}

void KernelEngine::append_predicted_projectile_render_states() {
    const auto cost_start = std::chrono::steady_clock::now();
    const bool had_projectiles = std::any_of(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [](const PredictedProjectile& projectile) {
            return !projectile.locally_terminated;
        });
    for (const PredictedProjectile& projectile : predicted_projectiles_) {
        if (projectile.locally_terminated) {
            continue;
        }
        const glm::vec3 render_position =
            projectile.position + projectile.correction_offset;
        render_states_.push_back(RenderEntityState{
            projectile.entity_id,
            projectile.net_id,
            static_cast<std::uint16_t>(EntityType::kProjectile),
            static_cast<std::uint16_t>(ActorType::kUnknown),
            projectile.owner_peer,
            to_kernel_vec3(render_position),
            to_kernel_quat(projectile.rotation),
            to_kernel_vec3(projectile.velocity),
            0,
            0,
            0,
            kVisualFlagMoving,
            projectile.spawn_tick,
            projectile.action_instance_id,
            RenderEntityStatus_Predicted,
            projectile.projectile_template_id,
            projectile.collider_template_id,
            KernelActionRuntimeView{sizeof(KernelActionRuntimeView)},
            KernelVec3{1.0f, 0.0f, 0.0f},
        });
    }
    if (had_projectiles) {
        benchmark_stats_.render_solver_cost_us +=
            std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
    }
}

void KernelEngine::advance_predicted_projectiles(float fixed_delta_seconds) {
    const auto cost_start = std::chrono::steady_clock::now();
    const bool had_projectiles = std::any_of(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [](const PredictedProjectile& projectile) {
            return !projectile.locally_terminated;
        });
    for (PredictedProjectile& projectile : predicted_projectiles_) {
        if (projectile.locally_terminated) {
            continue;
        }
        projectile.age_ticks += 1;
        const float projectile_age_duration =
            static_cast<float>(projectile.age_ticks) * fixed_delta_seconds;
        const glm::vec3 next_position = projectile_position_at(
            projectile.spawn_position,
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            projectile_age_duration);
        const glm::vec3 next_velocity = projectile_velocity_at(
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            projectile_age_duration);

        if (projectile.sync_mode ==
            KernelProjectileSyncMode_LocalPredictedDeterministic) {
            if (prediction_physics_world_ == nullptr) {
                if (!predicted_projectile_collision_warning_emitted_) {
                    spdlog::warn(
                        "local deterministic projectile collision prediction "
                        "is unavailable without a prediction physics world");
                    predicted_projectile_collision_warning_emitted_ = true;
                }
            } else {
                const KernelProjectileTemplateDefinition* projectile_template =
                    find_projectile_template(
                        projectile_templates_, projectile.projectile_template_id);
                const KernelColliderTemplateDefinition* collider_template =
                    projectile_template == nullptr
                        ? nullptr
                        : find_collider_template(
                              collider_templates_,
                              projectile_template->mechanics.collider_template_id);
                if (projectile_template != nullptr &&
                    collider_template != nullptr) {
                    ProjectileState collision_spec{};
                    collision_spec.collision_query_mode =
                        to_projectile_collision_query_mode(
                            projectile_template->mechanics.collision_query_mode);
                    collision_spec.collision_geometry =
                        projectile_collision_geometry_from_template(
                            *collider_template);
                    collision_spec.has_collision_geometry = true;

                    physics::CollisionQueryFilter filter{};
                    filter.collision_mask =
                        physics::collision_layer_bit(
                            physics::CollisionLayer::kTerrain) |
                        physics::collision_layer_bit(
                            physics::CollisionLayer::kStaticObstacle);
                    filter.object_kind_mask =
                        (1u << static_cast<std::uint32_t>(
                             physics::CollisionObjectKind::kTerrain)) |
                        (1u << static_cast<std::uint32_t>(
                             physics::CollisionObjectKind::kStaticObstacle));
                    const std::vector<physics::CollisionHit> hits =
                        query_projectile_collision_hits(
                            *prediction_physics_world_,
                            collision_spec,
                            projectile.position,
                            next_position,
                            filter);
                    if (!hits.empty()) {
                        // Unless the template opts in, the authority does not
                        // let an area effect touch whoever fired it:
                        // simulate_area_effects hands its overlap query
                        // ignored_entity_net_id = shooter_net_id, and a spawn
                        // chain carries that shooter forward, so a rocket and
                        // the explosion it spawns are filtered against the same
                        // actor. Predicting a self-impulse the authority will
                        // not apply predicts a push no snapshot can confirm,
                        // and the next owner snapshot -- which overwrites the
                        // predicted velocity outright rather than replaying
                        // this -- would yank it back. owner_peer is the
                        // shooter's peer on both spawn paths, which is all it
                        // takes to recognise one of ours.
                        const bool self_impulse_filtered_by_authority =
                            local_client_peer_id_ != 0u &&
                            projectile.owner_peer == local_client_peer_id_ &&
                            projectile_template->mechanics.area_effect
                                    .hit_instigator == 0u;
                        if (!self_impulse_filtered_by_authority &&
                            projectile_template->mechanics.projectile_type ==
                                KernelProjectileType_AreaEffect &&
                            projectile_template->mechanics.projectile_impact_trigger
                                    .struct_size >=
                                sizeof(KernelActionTriggerDefinition)) {
                            const glm::vec3 local_actor_position =
                                predicted_local_entity_.position;
                            const glm::vec3 radial = local_actor_position -
                                hits.front().position;
                            const float radial_length = glm::length(radial);
                            const float area_radius =
                                projectile_template->mechanics.area_effect.radius;
                            const std::uint32_t action_count =
                                projectile_template->mechanics.projectile_impact_trigger
                                    .action_count;
                            if (radial_length > 0.0001f &&
                                radial_length <= area_radius) {
                                const auto replicated = std::find_if(
                                    client_replicated_entities_.begin(),
                                    client_replicated_entities_.end(),
                                    [this](const ClientReplicatedEntity& candidate) {
                                        return candidate.net_id == local_player_net_id_;
                                    });
                                const KernelEntityTemplateDefinition* local_template =
                                    nullptr;
                                if (replicated != client_replicated_entities_.end()) {
                                    const auto found_local_template = std::find_if(
                                        entity_templates_.begin(),
                                        entity_templates_.end(),
                                        [replicated](const KernelEntityTemplateDefinition& candidate) {
                                            return candidate.actor_template_id ==
                                                replicated->actor_template_id;
                                        });
                                    if (found_local_template != entity_templates_.end()) {
                                        local_template = &*found_local_template;
                                    }
                                }
                                const float resistance = local_template == nullptr
                                    ? 0.0f
                                    : local_template->impulse_resistance;
                                for (std::uint32_t action_index = 0u;
                                     action_index < (action_count == 0u ? 1u : action_count);
                                     ++action_index) {
                                    KernelActionDefinition action{};
                                    const KernelActionTriggerDefinition& trigger =
                                        projectile_template->mechanics.projectile_impact_trigger;
                                    if (action_count == 0u) {
                                        action.action_type = trigger.action_type;
                                        action.target_source = trigger.target_source;
                                        action.direction_source = trigger.direction_source;
                                        action.impulse_strength = trigger.impulse_strength;
                                        action.impulse_collision_mask = trigger.impulse_collision_mask;
                                        action.impulse_direction = trigger.impulse_direction;
                                        action.impulse_lockout_ticks = trigger.impulse_lockout_ticks;
                                        action.impulse_strength_mode = trigger.impulse_strength_mode;
                                        action.impulse_strength_vertical = trigger.impulse_strength_vertical;
                                    } else {
                                        action = trigger.actions[action_index];
                                    }
                                    if (action.action_type !=
                                            KernelEntityTriggerActionType_ApplyImpulse ||
                                        action.target_source !=
                                            KernelEntityRefSource_EventTarget ||
                                        (action.impulse_collision_mask &
                                         KERNEL_COLLISION_MASK_ACTOR) == 0u ||
                                        !impulse_strength_is_authorable(
                                            action.impulse_strength_mode,
                                            action.impulse_strength,
                                            action.impulse_strength_vertical) ||
                                        impulse_effective_strength(
                                            action.impulse_strength_mode,
                                            action.impulse_strength,
                                            action.impulse_strength_vertical) <=
                                            resistance) {
                                        continue;
                                    }
                                    // Whatever the authority will resolve the
                                    // source to, resolved the same way here.
                                    // A subject direction that came out zero
                                    // means the field is not travelling, which
                                    // the catalog loader refuses to author, so
                                    // treating it as "nothing to predict" is
                                    // belt and braces rather than a fallback
                                    // with its own behaviour.
                                    glm::vec3 direction = glm::normalize(radial);
                                    if (action.direction_source ==
                                        KernelEventVec3Source_Literal) {
                                        direction = glm::vec3{
                                            action.impulse_direction.x,
                                            action.impulse_direction.y,
                                            action.impulse_direction.z};
                                    } else if (
                                        action.direction_source ==
                                        KernelEventVec3Source_SubjectDirection) {
                                        if (glm::length(projectile.initial_velocity) <=
                                            0.0001f) {
                                            continue;
                                        }
                                        direction = glm::normalize(
                                            projectile.initial_velocity);
                                    }
                                    const glm::vec3 impulse =
                                        impulse_velocity_delta(
                                            action.impulse_strength_mode,
                                            glm::normalize(direction),
                                            action.impulse_strength,
                                            action.impulse_strength_vertical);
                                    predicted_local_entity_.velocity += impulse;
                                    predicted_character_state_.velocity += impulse;
                                    predicted_character_state_.ground_state =
                                        physics::CharacterGroundState::kAirborne;
                                    predicted_character_state_.supporting_identity = {};
                                    if (action.impulse_lockout_ticks > 0u) {
                                        predicted_impulse_lockout_until_tick_ =
                                            predicted_character_tick_ +
                                            action.impulse_lockout_ticks;
                                        predicted_impulse_lockout_armed_tick_ =
                                            predicted_character_tick_;
                                    }
                                    break;
                                }
                            }
                        }
                        projectile.position = hits.front().position;
                        projectile.velocity = glm::vec3{0.0f, 0.0f, 0.0f};
                        // An area effect that meets the world is parked by the
                        // authority, not destroyed by it -- simulate_area_effects
                        // still owns when it ends -- so terminating it here
                        // would blink out a field the next snapshot still has.
                        // Zeroing initial_velocity parks it the same way the
                        // authority does.
                        if (projectile_template->mechanics.projectile_type ==
                            KernelProjectileType_AreaEffect) {
                            // And only if the template said the world stops
                            // it. One that did not is advanced straight
                            // through terrain by the authority, so parking it
                            // here would invent a stop no snapshot agrees
                            // with.
                            if (projectile_template->mechanics.area_effect
                                    .motion_collision_mask == 0u) {
                                projectile.position = next_position;
                                projectile.velocity = next_velocity;
                                continue;
                            }
                            projectile.spawn_position = projectile.position;
                            projectile.initial_velocity =
                                glm::vec3{0.0f, 0.0f, 0.0f};
                            projectile.age_ticks = 0;
                            continue;
                        }
                        projectile.locally_terminated = true;
                        continue;
                    }
                }
            }
        }

        projectile.position = next_position;
        projectile.velocity = next_velocity;
    }
    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [](const PredictedProjectile& projectile) {
                return !projectile.locally_terminated &&
                       projectile.max_lifetime_ticks > 0u &&
                       projectile.age_ticks >= projectile.max_lifetime_ticks;
            }),
        predicted_projectiles_.end());
    if (had_projectiles) {
        benchmark_stats_.projectile_solver_cost_us +=
            std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
    }
}

std::uint32_t KernelEngine::local_prediction_server_tick(
    std::uint32_t snapshot_tick) const {
    std::uint32_t estimate = tick_loop_.current_tick();
    if (has_client_clock_sync_) {
        const std::uint64_t server_time_us =
            offset_time_us(client_local_time_us_, client_clock_offset_us_);
        estimate = tick_for_time_us(server_time_us, tick_loop_.fixed_delta_seconds());
    }
    return std::max(estimate, snapshot_tick);
}

std::uint32_t KernelEngine::rewind_tick_for_input(
    const QueuedInput& queued_input) const {
    return tick_for_time_us(
        compensated_action_time_us(queued_input),
        tick_loop_.fixed_delta_seconds());
}

std::uint64_t KernelEngine::entity_id_for_net_id(NetId net_id) {
    if (net_id == 0) {
        return 0;
    }
    auto found = entity_ids_by_net_id_.find(net_id);
    if (found != entity_ids_by_net_id_.end()) {
        return found->second;
    }
    const std::uint64_t entity_id = next_entity_id_++;
    entity_ids_by_net_id_.emplace(net_id, entity_id);
    return entity_id;
}

std::uint64_t KernelEngine::allocate_predicted_entity_id() {
    return next_predicted_entity_id_++;
}

bool KernelEngine::has_predicted_projectile_net_id(NetId net_id) const {
    if (net_id == 0) {
        return false;
    }
    return std::any_of(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [net_id](const PredictedProjectile& projectile) {
            return projectile.bound && projectile.net_id == net_id;
        });
}

KernelEngine::PredictedProjectile* KernelEngine::find_predicted_projectile(
    PeerId owner_peer,
    std::uint32_t action_instance_id) {
    if (action_instance_id == 0) {
        return nullptr;
    }
    auto found = std::find_if(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [owner_peer, action_instance_id](const PredictedProjectile& projectile) {
            return projectile.owner_peer == owner_peer &&
                   projectile.action_instance_id == action_instance_id;
        });
    if (found == predicted_projectiles_.end()) {
        return nullptr;
    }
    return &(*found);
}

bool KernelEngine::enqueue_simulation_command(const simulation::Command& command) {
    if (!command_queue_.enqueue(command)) {
        ++rejected_simulation_command_count_;
        return false;
    }

    const std::size_t queue_depth = command_queue_.size();
    if (queue_depth >= simulation::CommandQueue::kWarningThreshold &&
        (command_queue_capacity_warning_count_ == 0 ||
         tick_loop_.current_tick() - last_command_queue_capacity_warning_tick_ >=
             120)) {
        ++command_queue_capacity_warning_count_;
        last_command_queue_capacity_warning_tick_ = tick_loop_.current_tick();
        spdlog::warn(
            "[NetworkExample] simulation command queue depth={} warning_threshold={} "
            "capacity={} rejected_count={}",
            queue_depth,
            simulation::CommandQueue::kWarningThreshold,
            simulation::CommandQueue::kDefaultCapacity,
            rejected_simulation_command_count_);
    }
    return true;
}

std::size_t KernelEngine::drain_simulation_commands() {
    const auto commands = command_queue_.commands();
    last_simulation_command_queue_depth_ = commands.size();

    std::size_t processed_count = 0;
    simulation::Dispatcher dispatcher;
    for (const simulation::Command& command : commands) {
        const simulation::CommandResult result = dispatcher.dispatch(*this, command);
        rpc_dispatcher_.complete_simulation_command(
            command.completion_token,
            command.id,
            result);
        if (!result.ok) {
            ++failed_simulation_command_count_;
        }
        ++processed_count;
    }
    command_queue_.clear();
    last_simulation_command_processed_count_ = processed_count;
    return processed_count;
}

void KernelEngine::record_simulation_tick_cost(
    std::uint64_t cost_us,
    std::size_t queue_depth,
    std::size_t processed_command_count) {
    last_simulation_tick_cost_us_ = std::max<std::uint64_t>(cost_us, 1);
    if (simulation_tick_cost_sample_count_ <
        simulation_tick_cost_samples_us_.size()) {
        ++simulation_tick_cost_sample_count_;
    } else {
        simulation_tick_cost_sample_sum_us_ -=
            simulation_tick_cost_samples_us_[simulation_tick_cost_sample_index_];
    }
    simulation_tick_cost_samples_us_[simulation_tick_cost_sample_index_] =
        last_simulation_tick_cost_us_;
    simulation_tick_cost_sample_sum_us_ += last_simulation_tick_cost_us_;
    simulation_tick_cost_sample_index_ =
        (simulation_tick_cost_sample_index_ + 1) %
        simulation_tick_cost_samples_us_.size();
    average_simulation_tick_cost_us_ =
        simulation_tick_cost_sample_sum_us_ / simulation_tick_cost_sample_count_;

    const auto fixed_delta_us = static_cast<std::uint64_t>(
        std::max(1.0, static_cast<double>(tick_loop_.fixed_delta_seconds()) *
                          1'000'000.0));
    const std::uint64_t default_threshold_us =
        std::min<std::uint64_t>(8000, std::max<std::uint64_t>(1, fixed_delta_us / 4));
    const std::uint64_t threshold_us =
        simulation_tick_cost_warning_threshold_us_ == 0
            ? default_threshold_us
            : simulation_tick_cost_warning_threshold_us_;
    if (average_simulation_tick_cost_us_ >= threshold_us &&
        (simulation_tick_cost_warning_count_ == 0 ||
         tick_loop_.current_tick() - last_simulation_tick_cost_warning_tick_ >=
             120)) {
        ++simulation_tick_cost_warning_count_;
        last_simulation_tick_cost_warning_tick_ = tick_loop_.current_tick();
        spdlog::warn(
            "[NetworkExample] simulation tick avg_cost_us={} last_cost_us={} "
            "threshold_us={} queue_depth={} processed_commands={}",
            average_simulation_tick_cost_us_,
            last_simulation_tick_cost_us_,
            threshold_us,
            queue_depth,
            processed_command_count);
    }
}

void KernelEngine::finalize_simulated_projectile_destructions(
    std::size_t first_event,
    std::size_t last_event,
    const std::unordered_set<NetId>& actors_before_tick) {
    const std::size_t capped_last = std::min(last_event, events_.size());
    std::unordered_set<NetId> finalized_projectiles;
    for (std::size_t index = first_event; index < capped_last; ++index) {
        const KernelEvent& event = events_[index];
        if (event.type != KernelEventType_EntityDestroyed ||
            event.code != KernelDespawnReason_Destroyed ||
            actors_before_tick.find(event.net_id) != actors_before_tick.end() ||
            !finalized_projectiles.insert(event.net_id).second) {
            continue;
        }

        if (config_.mode == KernelMode_ListenServer) {
            const bool was_relevant =
                local_listen_session_.relevant_entities.erase(event.net_id) > 0;
            const bool was_out_of_range =
                local_listen_session_.out_of_range_projectiles.erase(event.net_id) > 0;
            if ((was_relevant || was_out_of_range) &&
                listen_server_transport_ != nullptr) {
                send_entity_despawn(
                    kLocalListenPeerId,
                    event.net_id,
                    KernelDespawnReason_Destroyed);
            }
        }
        for (PeerSession& session : peer_sessions_) {
            const bool was_relevant =
                session.relevant_entities.erase(event.net_id) > 0;
            const bool was_out_of_range =
                session.out_of_range_projectiles.erase(event.net_id) > 0;
            if (was_relevant || was_out_of_range) {
                send_entity_despawn(
                    session.peer,
                    event.net_id,
                    KernelDespawnReason_Destroyed);
            }
        }
        lifecycle_events_.push_back(KernelEntityLifecycleEvent{
            KernelEntityLifecycleEventType_Destroyed,
            tick_loop_.current_tick(),
            event.net_id,
            KernelDespawnReason_Destroyed,
            static_cast<std::uint16_t>(EntityType::kProjectile),
            static_cast<std::uint16_t>(ActorType::kUnknown),
            0,
        });
    }
}

void KernelEngine::update_legged_locomotion(
    const std::vector<QueuedInput>& movement_inputs,
    float fixed_delta_seconds) {
    std::unordered_set<NetId> active_locomotion_entities;
    const auto view = world_.registry().view<NetworkIdentity, Transform>();
    for (const entt::entity entity : view) {
        std::uint32_t template_id = 0u;
        if (world_.registry().all_of<EntityTemplateRef>(entity)) {
            template_id = world_.registry()
                .get<EntityTemplateRef>(entity)
                .entity_template_id;
        } else if (world_.registry().all_of<ActorTemplateRef>(entity)) {
            template_id = world_.registry()
                .get<ActorTemplateRef>(entity)
                .actor_template_id;
        }
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(entity_templates_, template_id);
        if (entity_template == nullptr ||
            entity_template->skeleton.struct_size <
                sizeof(KernelSkeletonBindingDefinition)) {
            continue;
        }

        const NetId net_id = view.get<NetworkIdentity>(entity).net_id;
        Transform& transform = view.get<Transform>(entity);
        active_locomotion_entities.insert(net_id);
        auto [state, inserted] = locomotion_states_.try_emplace(net_id);
        if (inserted) {
            const glm::vec3 forward =
                transform.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
            const float initial_yaw = std::atan2(forward.x, forward.z);
            if (!initialize_locomotion_state(
                    entity_template->skeleton,
                    initial_yaw,
                    &state->second)) {
                locomotion_states_.erase(state);
                continue;
            }
        }

        const QueuedInput* entity_input = nullptr;
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        for (const QueuedInput& candidate : movement_inputs) {
            if (candidate.controlled_net_id != net_id ||
                (entity_input != nullptr &&
                 candidate.input.input_seq <= entity_input->input.input_seq)) {
                continue;
            }
            entity_input = &candidate;
        }
        if (entity_input == nullptr) {
            for (const QueuedInput& candidate : movement_inputs) {
                if (candidate.controlled_net_id != 0u ||
                    candidate.owner_peer != identity.owner_peer ||
                    (entity_input != nullptr &&
                     candidate.input.input_seq <= entity_input->input.input_seq)) {
                    continue;
                }
                entity_input = &candidate;
            }
        }
        const KernelVec2 move_input = entity_input == nullptr
            ? KernelVec2{}
            : entity_input->input.move;
        if (!advance_locomotion_state(
                entity_template->skeleton,
                move_input,
                entity_template->movement.max_yaw_degrees_per_second,
                fixed_delta_seconds,
                &state->second)) {
            // No pose this tick, so nothing to hang limbs on. See
            // materialize_entity_limb_colliders: a stale limb is worse than no
            // limb, because it keeps blocking where the actor no longer is.
            world_.collider_registry().remove_bone_colliders(net_id);
            continue;
        }
        transform.rotation = glm::angleAxis(
            state->second.root_yaw_radians,
            glm::vec3{0.0f, 1.0f, 0.0f});

        const RuntimeSkeletonAsset* skeleton_asset = find_skeleton_asset(
            skeleton_assets_,
            entity_template->skeleton.skeleton_asset_id);
        if (skeleton_asset == nullptr) {
            state->second.pose_valid = false;
            world_.collider_registry().remove_bone_colliders(net_id);
            continue;
        }
        const LocomotionGroundingQuery grounding_query =
            [this, net_id](
                const glm::vec3& origin,
                float max_distance,
                LocomotionGroundingHit* out_hit) {
                if (physics_world_ == nullptr || out_hit == nullptr) {
                    return false;
                }
                physics::RayCastRequest request{};
                request.origin = origin;
                request.direction = glm::vec3{0.0f, -1.0f, 0.0f};
                request.max_distance = max_distance;
                request.filter.collision_mask =
                    physics::collision_layer_bit(
                        physics::CollisionLayer::kTerrain) |
                    physics::collision_layer_bit(
                        physics::CollisionLayer::kStaticObstacle);
                request.filter.ignored_entity_net_id = net_id;
                request.filter.object_kind_mask =
                    (1u << static_cast<std::uint32_t>(
                         physics::CollisionObjectKind::kTerrain)) |
                    (1u << static_cast<std::uint32_t>(
                         physics::CollisionObjectKind::kStaticObstacle));
                physics::CollisionHit hit{};
                if (!physics_world_->ray_cast_closest(request, &hit)) {
                    return false;
                }
                out_hit->position = hit.position;
                out_hit->normal = hit.normal;
                out_hit->supporting_entity_net_id =
                    hit.identity.entity_net_id;
                out_hit->supporting_collider_id = hit.identity.collider_id;
                return true;
            };
        // Body grounding follow, part 1 of 2: settle the body onto the height
        // fit the previous tick computed, BEFORE solving. Applying it after the
        // solve would move the body out from under a pose that was built for the
        // old height, shifting every foot off its foothold by the blend step.
        // (The matching tilt is carried inside the locomotion state and applied
        // by the solve itself, so both corrections lag exactly one tick.)
        //
        // While this is on, locomotion owns y outright. simulate_actor_movement
        // ran earlier this tick and already wrote the controller's own answer
        // into transform.position.y, so the smoothing deliberately starts from
        // the height locomotion last applied instead: blending from the
        // transform would let the controller's answer back in every single tick,
        // and that answer -- terrain sampled under a capsule a few metres wide,
        // for a body standing on a footprint tens of metres across -- is the
        // mismatch this exists to remove. x and z stay the controller's.
        //
        // The controller gets y back the moment no foot is planted, which is
        // what carries a spawn drop-in. Note what that does NOT cover: the
        // foothold rays are long enough to find ground far below, so an actor
        // walking off a ledge rides its feet down the drop rather than falling
        // ballistically. That is the prototype's behaviour and is fine for a
        // walker; anything that needs real airtime needs a grounded test here
        // rather than a foot-planted one.
        const bool locomotion_owns_height =
            entity_template->skeleton.body_follow_speed > 0.0f &&
            state->second.body_follow_valid;
        if (locomotion_owns_height) {
            const float blend = 1.0f - std::exp(
                -entity_template->skeleton.body_follow_speed *
                fixed_delta_seconds);
            const float previous = state->second.has_body_follow_applied_height
                ? state->second.body_follow_applied_height
                : transform.position.y;
            const float settled = std::lerp(
                previous,
                state->second.body_follow_target_height,
                blend);
            if (std::isfinite(settled)) {
                // Velocity is deliberately left alone. It is the controller's
                // own vertical state, and the controller now runs on its own
                // anchor, lands on terrain and integrates gravity there without
                // help. Writing the body's rate into it instead couples two
                // heights that are metres apart by design: the difference
                // between them divided by a tick reads as a ~120 m/s dive, which
                // Jolt duly applies and which slides the actor backwards down
                // the nearest slope.
                transform.position.y = settled;
                state->second.body_follow_applied_height = settled;
                state->second.has_body_follow_applied_height = true;
            }
        } else {
            state->second.has_body_follow_applied_height = false;
        }

        // The leg solve is intentionally decoupled from the character/movement
        // controller: it reads transform.position only as a world anchor for
        // foot placement and never inspects the controller's grounded/landed
        // state (the controller owns whether the body advances).
        if (!solve_legged_locomotion_pose(
                skeleton_asset->skeleton,
                skeleton_asset->bind_pose,
                entity_template->skeleton,
                transform.position,
                entity_template->movement.max_slope_degrees,
                fixed_delta_seconds,
                grounding_query,
                &state->second)) {
            state->second.pose_valid = false;
        }

        // Body grounding follow, part 2 of 2: the solve tilted the root and
        // placed the feet with that exact rotation, so the transform must carry
        // it. Re-deriving a pure-yaw rotation here would both slide the feet and
        // restart the tilt smoothing every tick. Disabled by default, in which
        // case the tilt is identity and physics keeps the transform to itself.
        if (entity_template->skeleton.body_follow_speed > 0.0f &&
            state->second.pose_valid) {
            transform.rotation = state->second.applied_root_rotation;
        }

        // The rig's own colliders, refreshed from the pose that was just solved
        // and composed onto the transform that solve was given -- including the
        // body-follow height applied above and the rotation written back. The
        // next sync_entity_colliders_from_world() is what pushes them into the
        // physics world; this only says where they are.
        materialize_entity_limb_colliders(
            net_id,
            entity_template->skeleton,
            *skeleton_asset,
            state->second);

        // Deliberately records no pose history. The history is sampled at the
        // instant presentation renders, which is a server tick behind the live
        // one this loop solves at; since the follower now reconstructs the same
        // entities a listen server simulates, recording here too would put two
        // time bases in one per-entity buffer and presentation would blend
        // across them. step_follower_locomotion_tick owns it instead. A
        // dedicated server records nothing and needs nothing: with no snapshot
        // buffer there is no instant to interpolate to, and presentation there
        // reads the live pose directly.

        // Publish the steps this tick committed. Both swing endpoints are frozen
        // at lift-off, so one of these fully describes a step and a follower can
        // reproduce it without the terrain, the gait, or the 41 bones.
        std::array<LocomotionStepEvent, KERNEL_MAX_SKELETON_LEGS> committed{};
        const std::uint32_t committed_count = collect_locomotion_step_events(
            state->second,
            tick_loop_.current_tick(),
            committed);
        for (std::uint32_t index = 0u;
             index < std::min<std::uint32_t>(
                 committed_count,
                 static_cast<std::uint32_t>(committed.size()));
             ++index) {
            outgoing_locomotion_steps_.push_back(
                PendingLocomotionStep{net_id, committed[index]});
        }
    }
    // Followed entities keep their history too. This function only knows the
    // entities this kernel simulates, and on a client that set never contains a
    // replicated actor -- so pruning on it alone wiped the follower's history
    // every tick. update_follower_locomotion refills it only on ticks it
    // actually steps, and it steps only when a snapshot advances the target
    // tick, which at a client tick rate above the snapshot rate is roughly half
    // of them. The other half found an empty history, fell back to the newest
    // solved pose, and composed it onto a root interpolated at a different
    // instant -- the exact root/pose time-base split f728035 exists to prevent,
    // reappearing every other frame as feet that jump between two placements.
    // Entries for entities that stop being replicated are dropped here on the
    // tick after update_follower_locomotion releases their follower state.
    std::erase_if(
        skeleton_pose_history_,
        [this, &active_locomotion_entities](const auto& entry) {
            return !active_locomotion_entities.contains(entry.first) &&
                !follower_locomotion_states_.contains(entry.first);
        });
    std::erase_if(
        locomotion_states_,
        [&active_locomotion_entities](const auto& entry) {
            return !active_locomotion_entities.contains(entry.first);
        });
}

void KernelEngine::enqueue_replicated_locomotion_step(
    NetId net_id,
    const LocomotionStepEvent& event) {
    if (net_id == 0u) {
        return;
    }
    pending_follower_steps_.push_back(PendingLocomotionStep{net_id, event});
}

void KernelEngine::update_follower_locomotion() {
    if (!has_client_snapshot_ || client_snapshot_buffer_.empty()) {
        follower_locomotion_states_.clear();
        has_follower_locomotion_tick_ = false;
        // Held steps are deliberately NOT dropped here. A baseline is sent
        // beside the spawn that makes an entity relevant, so it can arrive
        // before that entity's first snapshot does -- discarding it would put
        // back exactly the "legs appear one at a time" symptom the baseline
        // exists to remove. Capped so a client that never receives a snapshot
        // cannot grow the queue without bound.
        constexpr std::size_t kMaxHeldSteps = 256u;
        if (pending_follower_steps_.size() > kMaxHeldSteps) {
            pending_follower_steps_.erase(
                pending_follower_steps_.begin(),
                pending_follower_steps_.begin() +
                    static_cast<std::ptrdiff_t>(
                        pending_follower_steps_.size() - kMaxHeldSteps));
        }
        return;
    }
    // Followed legs live in the snapshot buffer's tick space, not this kernel's:
    // a client's own tick counter is free-running, and the poses have to be
    // stamped with the same ticks the roots are, or presentation would sample
    // the two at different instants and slide every foot.
    const std::uint32_t target_tick =
        client_snapshot_buffer_.back().header.server_tick;
    // A gap in delivery (or a first snapshot) must not turn into a long replay:
    // there is nothing to reconstruct in the missing span anyway, since the
    // steps that happened there were never received.
    constexpr std::uint32_t kMaxCatchUpTicks = 8u;
    if (!has_follower_locomotion_tick_ ||
        static_cast<std::int32_t>(target_tick - follower_locomotion_tick_) >
            static_cast<std::int32_t>(kMaxCatchUpTicks) ||
        static_cast<std::int32_t>(target_tick - follower_locomotion_tick_) < 0) {
        follower_locomotion_tick_ = target_tick > kMaxCatchUpTicks
            ? target_tick - kMaxCatchUpTicks
            : 0u;
        has_follower_locomotion_tick_ = true;
    }
    while (static_cast<std::int32_t>(
               target_tick - follower_locomotion_tick_) > 0) {
        ++follower_locomotion_tick_;
        step_follower_locomotion_tick(follower_locomotion_tick_);
    }

    // Steps that never became applicable are eventually dropped so the queue
    // cannot grow without bound. The measure is how long a step has been held,
    // NOT how old its tick is: a baseline is an already-finished step, so it
    // arrives deliberately ancient, and a tick-age window discards it before
    // the follower reaches a tick on which to apply it. Updates run at the
    // client's tick rate while ticks are stepped only when snapshots arrive, so
    // roughly half of all updates step nothing at all -- which is precisely
    // when a tick-age sweep used to eat the baseline.
    constexpr std::uint32_t kMaxHeldUpdates = 64u;
    for (PendingLocomotionStep& pending : pending_follower_steps_) {
        ++pending.held_updates;
    }
    std::erase_if(
        pending_follower_steps_,
        [](const PendingLocomotionStep& pending) {
            return pending.held_updates > kMaxHeldUpdates;
        });
    std::erase_if(
        follower_locomotion_states_,
        [this](const auto& entry) {
            return std::none_of(
                client_replicated_entities_.begin(),
                client_replicated_entities_.end(),
                [&entry](const ClientReplicatedEntity& replicated) {
                    return replicated.net_id == entry.first;
                });
        });
}

void KernelEngine::step_follower_locomotion_tick(std::uint32_t server_tick) {
    const float fixed_delta_seconds = tick_loop_.fixed_delta_seconds();
    const std::uint64_t server_time_us =
        tick_time_us(server_tick, fixed_delta_seconds);
    WorldSnapshot stepped_snapshot;
    if (!build_interpolated_snapshot_for_server_time(
            server_time_us,
            &stepped_snapshot)) {
        return;
    }
    for (const EntitySnapshot& entity : stepped_snapshot.entities) {
        // Every entity the client half was told about is reconstructed here,
        // including the ones this kernel also simulates. A listen server used
        // to skip those and present its own authoritative legs instead, which
        // made it the one topology whose rigs were not what a client sees --
        // useless as a stand-in for the dedicated server it is meant to model.
        // The authoritative solve still runs: it owns the transform, the limb
        // colliders and the steps published from here. It just no longer
        // doubles as presentation.
        const auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == entity.net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        const std::uint32_t template_id =
            replicated->type == EntityType::kActor
                ? replicated->actor_template_id
                : replicated->entity_template_id;
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(entity_templates_, template_id);
        if (entity_template == nullptr ||
            entity_template->skeleton.struct_size <
                sizeof(KernelSkeletonBindingDefinition)) {
            continue;
        }
        const RuntimeSkeletonAsset* skeleton_asset = find_skeleton_asset(
            skeleton_assets_,
            entity_template->skeleton.skeleton_asset_id);
        if (skeleton_asset == nullptr) {
            continue;
        }

        auto [state, inserted] =
            follower_locomotion_states_.try_emplace(entity.net_id);
        if (inserted) {
            const glm::vec3 forward =
                entity.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
            if (!initialize_locomotion_state(
                    entity_template->skeleton,
                    std::atan2(forward.x, forward.z),
                    &state->second)) {
                follower_locomotion_states_.erase(state);
                continue;
            }
        }

        // A follower reads its heading off the replicated transform: it has no
        // movement input to derive one from, and advance_locomotion_state never
        // runs on this path.
        const glm::vec3 forward = entity.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
        state->second.root_yaw_radians = std::atan2(forward.x, forward.z);

        for (std::size_t index = 0u; index < pending_follower_steps_.size();) {
            const PendingLocomotionStep& pending = pending_follower_steps_[index];
            if (pending.net_id != entity.net_id ||
                static_cast<std::int32_t>(
                    server_tick - pending.event.start_tick) < 0) {
                ++index;
                continue;
            }
            apply_locomotion_step_event(
                entity_template->skeleton,
                pending.event,
                server_tick,
                &state->second);
            pending_follower_steps_.erase(
                pending_follower_steps_.begin() +
                static_cast<std::ptrdiff_t>(index));
        }

        if (!solve_legged_locomotion_follower_pose(
                skeleton_asset->skeleton,
                skeleton_asset->bind_pose,
                entity_template->skeleton,
                entity.position,
                fixed_delta_seconds,
                &state->second)) {
            state->second.pose_valid = false;
            continue;
        }
        record_skeleton_pose_sample(
            server_tick,
            server_time_us,
            state->second.local_pose,
            skeleton_pose_history_capacity(),
            &skeleton_pose_history_[entity.net_id]);
    }
}

void KernelEngine::simulate_tick() {
    const auto tick_cost_start = std::chrono::steady_clock::now();
    const float fixed_delta = tick_loop_.fixed_delta_seconds();
    const std::uint64_t server_time_us =
        tick_time_us(tick_loop_.current_tick(), fixed_delta);
    const std::size_t first_tick_event = events_.size();
    world_.prune_action_graph_batches(tick_loop_.current_tick());
    std::unordered_set<NetId> actors_before_tick;
    std::vector<ActionOutcome> action_outcomes;
    const auto actor_view = world_.registry().view<NetworkIdentity, EntityKind>();
    for (const entt::entity entity : actor_view) {
        const EntityKind& kind = actor_view.get<EntityKind>(entity);
        if (kind.type != EntityType::kActor) {
            continue;
        }
        const NetId net_id = actor_view.get<NetworkIdentity>(entity).net_id;
        actors_before_tick.insert(net_id);
    }
    world_.collider_registry().expire_tick_lifetimes();
    EntityLifecycleSystem{}.update_prop_lifetimes(*this);
    const std::size_t queue_depth = command_queue_.size();
    const std::size_t processed_command_count = drain_simulation_commands();
    advance_predicted_projectiles(fixed_delta);
    for (const QueuedInput& pending_input : pending_inputs_) {
        if (pending_input.controlled_net_id != 0) {
            continue;
        }
        damage_pipeline_.ingest_defensive_input(
            pending_input.owner_peer,
            pending_input.input,
            server_time_us,
            compensated_action_time_us(pending_input),
            true);
    }
    sync_entity_colliders_from_world();
    simulate_status_effects(*this, server_time_us);
    MovementSimulationStats movement_stats{};
    std::vector<QueuedInput> movement_inputs =
        build_effective_movement_inputs(server_time_us);
    std::vector<NetId> physics_finalized_actor_net_ids;
    simulate_actor_movement(
        world_,
        movement_inputs,
        fixed_delta,
        tick_loop_.current_tick(),
        &events_,
        &movement_stats,
        session_rules_.actor_blocking_mode,
        &physics_finalized_actor_net_ids);
    update_legged_locomotion(movement_inputs, fixed_delta);
    // Entities that only arrive as snapshots get their legs from replayed steps
    // rather than from the solve above, which never sees them: they are not in
    // this kernel's registry.
    update_follower_locomotion();
    acknowledge_simulated_movement_inputs(movement_inputs);
    benchmark_stats_.grounded_query_count +=
        movement_stats.grounded_query_count;
    benchmark_stats_.grounded_query_cost_us +=
        movement_stats.grounded_query_cost_us;
    benchmark_stats_.kinematic_move_count +=
        movement_stats.kinematic_move_count;
    benchmark_stats_.kinematic_move_cost_us +=
        movement_stats.kinematic_move_cost_us;
    benchmark_stats_.character_move_count +=
        movement_stats.character_move_count;
    benchmark_stats_.character_move_cost_us +=
        movement_stats.character_move_cost_us;
    ItemGameplaySystem{}.update_carried_props(*this);
    simulate_velocity_movement(world_, fixed_delta);
    sync_entity_colliders_from_world();
    CollisionTriggerSystem{}.update(*this, server_time_us);
    bool released_first_physics_actor = false;
    for (const NetId net_id : physics_finalized_actor_net_ids) {
        released_first_physics_actor =
            pending_first_physics_actors_.erase(net_id) > 0 ||
            released_first_physics_actor;
    }
    for (auto pending = pending_first_physics_actors_.begin();
         pending != pending_first_physics_actors_.end();) {
        if (!world_.find_entity(pending->first).has_value()) {
            pending = pending_first_physics_actors_.erase(pending);
            continue;
        }
        if (!pending->second.warning_reported &&
            tick_loop_.current_tick() > pending->second.spawn_tick) {
            spdlog::warn(
                "actor remains hidden before first physics finalization "
                "net_id={} spawn_tick={} current_tick={}",
                pending->first,
                pending->second.spawn_tick,
                tick_loop_.current_tick());
            pending->second.warning_reported = true;
        }
        ++pending;
    }
    for (const QueuedInput& pending_input : pending_inputs_) {
        const HistoryFrame* rewind_frame = nullptr;
        std::uint32_t rewind_tick = tick_loop_.current_tick();
        if (pending_input.controlled_net_id == 0 &&
            ((pending_input.input.action_intent.action_instance_id != 0u &&
              pending_input.input.action_intent.binding_id ==
                  KernelActionBinding_PrimaryFire) ||
             (pending_input.input.action_input.action_instance_id != 0u &&
              pending_input.input.action_input.held != 0u))) {
            rewind_tick = rewind_tick_for_input(pending_input);
            rewind_frame = history_buffer_.find_frame_clamped(rewind_tick);
            if (rewind_frame != nullptr) {
                rewind_tick = rewind_frame->server_tick;
            }
        }
        const std::vector<QueuedInput> single_input{pending_input};
        simulate_weapons(
            world_,
            single_input,
            WeaponSimulationContext{
                &history_buffer_,
                rewind_frame,
                &damage_pipeline_,
                rewind_tick,
                tick_loop_.current_tick(),
                fixed_delta,
                compensated_action_time_us(pending_input),
                &action_outcomes},
            &events_);
    }
    simulate_weapons(
        world_,
        {},
        WeaponSimulationContext{
            nullptr,
            nullptr,
            &damage_pipeline_,
            tick_loop_.current_tick(),
            tick_loop_.current_tick(),
            fixed_delta,
            server_time_us,
            &action_outcomes},
        &events_);
    const std::size_t first_projectile_simulation_event = events_.size();
    std::vector<ActionGraphCommandBatch> area_action_graph_batches;
    simulate_projectiles(
        world_,
        fixed_delta,
        tick_loop_.current_tick(),
        &events_,
        &damage_pipeline_);
    simulate_area_effects(
        world_,
        tick_loop_.current_tick(),
        server_time_us,
        &events_,
        &damage_pipeline_,
        &area_action_graph_batches);
    for (const ActionGraphCommandBatch& batch : area_action_graph_batches) {
        (void)execute_action_graph_command_batch(*this, batch, server_time_us);
    }
    simulate_beams(
        world_,
        tick_loop_.current_tick(),
        fixed_delta,
        server_time_us,
        &events_,
        &damage_pipeline_);
    finalize_simulated_projectile_destructions(
        first_projectile_simulation_event,
        events_.size(),
        actors_before_tick);
    sync_entity_colliders_from_world();
    const std::vector<ConfirmedDamage> ready_damage =
        damage_pipeline_.drain_ready_damage(world_, server_time_us);
    queue_hit_debug_records(ready_damage);
    const std::vector<ConfirmedDamage> health_depleted = apply_damage_applications(
        world_,
        ready_damage,
        tick_loop_.current_tick(),
        &events_);
    EntityLifecycleSystem lifecycle_system;
    lifecycle_system.process_health_depleted(
        *this, health_depleted, server_time_us);
    lifecycle_system.destroy_dead_entities(*this, health_depleted);
    update_vision_states(fixed_delta);
    DirectorAISystem{}.update(*this);
    DirectorIntentExecutor{}.update(*this);
    const std::size_t last_tick_event = events_.size();
    for (std::size_t index = first_tick_event; index < last_tick_event; ++index) {
        if (events_[index].type != KernelEventType_HealthChanged) {
            continue;
        }
        const std::optional<entt::entity> entity =
            world_.find_entity(events_[index].net_id);
        if (entity.has_value() &&
            world_.registry().all_of<EntityKind>(*entity) &&
            world_.registry().get<EntityKind>(*entity).type == EntityType::kProp) {
            queue_prop_state_change(events_[index].net_id);
        }
    }
    finalize_server_action_outcomes(action_outcomes);
    queue_remote_presentation_from_events(
        first_tick_event,
        last_tick_event,
        actors_before_tick);
    if (config_.mode == KernelMode_ListenServer &&
        local_listen_session_.welcomed) {
        flush_local_action_results(&local_listen_session_);
        flush_remote_action_presentation(
            &local_listen_session_,
            pending_server_remote_presentations_);
    }
    for (PeerSession& session : peer_sessions_) {
        if (!session.welcomed) {
            continue;
        }
        flush_local_action_results(&session);
        flush_remote_action_presentation(
            &session,
            pending_server_remote_presentations_);
    }
    pending_server_remote_presentations_.clear();
    broadcast_combat_events(first_tick_event, last_tick_event);
    history_buffer_.write_frame(world_, tick_loop_.current_tick());
    const bool wrote_snapshot =
        released_first_physics_actor ||
        tick_loop_.should_write_snapshot() ||
        !pending_network_gameplay_outcomes_.empty();
    if (wrote_snapshot) {
        publish_snapshot();
    }
    flush_inventory_replication();
    flush_prop_state_changes();
    // With the snapshot rather than every tick, which is what
    // LocomotionStepRecord has always said it does: start_tick_delta exists so
    // that a batch can span a snapshot interval. Flushing per tick sent a packet
    // header -- 34 B of it -- for each tick that produced a single step, and
    // left this channel on a cadence the netcode preset does not govern; a
    // server switched to a snapshot every tick now moves both channels, and one
    // at half rate moves neither.
    //
    // The cost is up to one tick of latency on a footfall, against the 133 ms
    // the client already holds every snapshot for. The follower was built for
    // this: update_follower_locomotion is written to be called on ticks that
    // carry no new step.
    if (wrote_snapshot) {
        flush_locomotion_steps();
    }
    flush_network_gameplay_request_outcomes();
    send_due_clock_sync_pings(server_time_us);
    pending_inputs_.clear();
    record_simulation_tick_cost(
        elapsed_cost_us(tick_cost_start),
        queue_depth,
        processed_command_count);
    tick_loop_.advance_tick();
}

WorldSnapshot KernelEngine::build_relevant_snapshot(
    const PeerSession& session,
    std::uint32_t server_time_ms) const {
    WorldSnapshot full_snapshot = build_world_snapshot(
        world_,
        tick_loop_.current_tick(),
        server_time_ms,
        session.last_processed_input_seq);
    const EntitySnapshot* player_entity =
        find_snapshot_entity(full_snapshot, session.player);

    WorldSnapshot filtered;
    filtered.header = full_snapshot.header;
    filtered.entities.reserve(full_snapshot.entities.size());
    for (const EntitySnapshot& entity : full_snapshot.entities) {
        if (is_actor_pending_first_physics(entity.net_id)) {
            continue;
        }
        if (is_entity_relevant_to_session(session, entity, player_entity)) {
            EntitySnapshot filtered_entity = entity;
            filtered_entity.has_authoritative_movement_state =
                entity.net_id == session.player;
            filtered.entities.push_back(filtered_entity);
        }
    }
    return filtered;
}

WorldSnapshot KernelEngine::build_snapshot_send_set(
    PeerSession& session,
    const WorldSnapshot& relevant_snapshot,
    std::size_t byte_budget) const {
    WorldSnapshot send_snapshot;
    send_snapshot.header = relevant_snapshot.header;
    std::size_t estimated_size = estimate_snapshot_base_packet_size();
    if (byte_budget < estimated_size) {
        return send_snapshot;
    }
    // Counts send sets, not ticks: the sections rotate once per snapshot, and
    // keying the stamps on the server tick would tie their order to a caller
    // that is free to build several send sets for the same tick.
    ++session.snapshot_send_sequence;

    const auto prepare_send_entity =
        [&](const EntitySnapshot& entity) -> std::optional<EntitySnapshot> {
            EntitySnapshot send_entity = entity;
            if (entity.type == EntityType::kProp &&
                is_dormant_placed_prop(entity.net_id)) {
                return std::nullopt;
            }
            if (entity.type != EntityType::kProjectile) {
                return send_entity;
            }

            std::uint8_t sync_mode =
                KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
            const std::optional<entt::entity> world_entity =
                world_.find_entity(entity.net_id);
            if (!world_entity.has_value()) {
                return send_entity;
            }
            if (world_.registry().all_of<HomingState>(*world_entity)) {
                sync_mode = to_kernel_projectile_sync_mode(
                    world_.registry().get<HomingState>(*world_entity).sync_mode);
            } else if (world_.registry().all_of<ProjectileState>(*world_entity)) {
                const ProjectileState& projectile =
                    world_.registry().get<ProjectileState>(*world_entity);
                if (const KernelProjectileTemplateDefinition* projectile_template =
                        find_projectile_template(
                            projectile_templates_,
                            projectile.projectile_template_id)) {
                    sync_mode = projectile_template->mechanics.sync_mode;
                }
            }

            if (sync_mode == KernelProjectileSyncMode_LocalPredictedDeterministic) {
                return std::nullopt;
            }
            if (sync_mode == KernelProjectileSyncMode_HybridDeterministicThenSnapshot) {
                send_entity.state_flags |=
                    kSnapshotStateFlagProjectileHybridCorrection;
            } else {
                send_entity.state_flags &=
                    ~kSnapshotStateFlagProjectileHybridCorrection;
            }
            return send_entity;
        };

    const auto try_add_entity = [&](const EntitySnapshot& entity) -> bool {
        const std::optional<EntitySnapshot> send_entity = prepare_send_entity(entity);
        if (!send_entity.has_value()) {
            return false;
        }
        const std::size_t entity_size = estimate_snapshot_entity_size(*send_entity);
        if (estimated_size + entity_size > byte_budget) {
            return false;
        }
        send_snapshot.entities.push_back(*send_entity);
        estimated_size += entity_size;
        return true;
    };

    // What the receiving player's own position is, for the distance weighting
    // below. Absent only if the player's own record did not survive relevance,
    // in which case every entity falls into the far band and the weighting
    // reduces to whether the entity is mid-action.
    const EntitySnapshot* priority_origin =
        find_snapshot_entity(relevant_snapshot, session.player);

    const auto entity_send_weight = [&](const EntitySnapshot& entity) {
        std::uint64_t weight = 1;
        if (entity.type == EntityType::kActor) {
            weight *= kSnapshotPriorityActorWeight;
        }
        if (entity.type == EntityType::kActor &&
            entity.actor_type == ActorType::kPlayer) {
            weight *= kSnapshotPriorityPlayerWeight;
        }
        if (entity.type == EntityType::kProjectile) {
            weight *= kSnapshotPriorityProjectileWeight;
        }
        if (entity.action_template_id != 0u ||
            entity.action_phase != KernelActionPhase_None) {
            weight *= kSnapshotPriorityActingWeight;
        }
        if (priority_origin != nullptr) {
            const float distance = std::max(
                0.0f,
                glm::length(entity.position - priority_origin->position) -
                    entity_bounding_radius(entity.net_id));
            if (distance <= kSnapshotPriorityNearMeters) {
                weight *= kSnapshotPriorityNearWeight;
            } else if (distance <= kSnapshotPriorityMidMeters) {
                weight *= kSnapshotPriorityMidWeight;
            }
        }
        return weight;
    };

    // One queue over everything the session can see, serving whoever is most
    // overdue for its own share -- how long it has waited multiplied by how much
    // that wait matters. Equal weights reduce this to plain
    // longest-waiting-first.
    //
    // It used to be three passes in a fixed order: actors, then projectiles,
    // then everything else written straight out with no rotation at all. The
    // first pass to fill the budget took all of it, so a crowd of relevant
    // agents meant projectiles and props reached the client at a rate of zero,
    // and the tail had no memory of who it had dropped.
    //
    // Selection order does not have to match the wire: encode_snapshot_packet
    // regroups by section at encode time, so merging the queue leaves the packet
    // format untouched.
    struct SendCandidate {
        bool overdue = false;
        std::uint64_t priority = 0;
        std::uint64_t stamp = 0;
        NetId net_id = 0;
        std::size_t size = 0;
        EntitySnapshot entity;
    };

    // The receiving session's own player is the one record that is never
    // scheduled: local prediction is reconciled against the authoritative state
    // in here, so a snapshot that omits it is a snapshot the client cannot
    // correct itself with.
    for (const EntitySnapshot& entity : relevant_snapshot.entities) {
        if (entity.net_id == session.player) {
            try_add_entity(entity);
        }
    }

    std::vector<SendCandidate> candidates;
    candidates.reserve(relevant_snapshot.entities.size());
    for (const EntitySnapshot& entity : relevant_snapshot.entities) {
        if (entity.net_id == session.player) {
            continue;
        }
        // Resolved once, here, rather than again inside the packing loop. An
        // entity this rejects -- a dormant placed prop, a projectile the client
        // predicts for itself -- is not deliverable at all this snapshot, so it
        // is dropped before the sort instead of sitting permanently overdue at
        // the head of a queue it can never be served from.
        std::optional<EntitySnapshot> prepared = prepare_send_entity(entity);
        if (!prepared.has_value()) {
            continue;
        }
        const auto found = session.last_sent_sequence.find(entity.net_id);
        const std::uint64_t stamp =
            found == session.last_sent_sequence.end() ? 0u : found->second;
        // A never-sent entity carries stamp 0 and therefore the largest wait
        // available, which is what puts newcomers at the front without a
        // special case for them.
        const std::uint64_t wait = session.snapshot_send_sequence - stamp;
        const std::size_t size = estimate_snapshot_entity_size(*prepared);
        candidates.push_back(SendCandidate{
            wait >= kMaxSnapshotsWithoutSend,
            wait * entity_send_weight(entity),
            stamp,
            entity.net_id,
            size,
            std::move(*prepared)});
    }

    // Net id breaks the tie so that a run of equal priorities -- every entity on
    // the first snapshot, all of them unsent and equally weighted -- is ordered
    // by something stable rather than by however the world iterated.
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const SendCandidate& lhs, const SendCandidate& rhs) {
            if (lhs.overdue != rhs.overdue) {
                return lhs.overdue;
            }
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            return lhs.net_id < rhs.net_id;
        });

    // Rebuilt rather than updated in place, so that an entity which has left the
    // relevant set drops its stamp with it. Kept without a stamp of its own, a
    // returning or recycled net id would look freshly served and lose a turn.
    std::unordered_map<NetId, std::uint64_t> next_last_sent;
    next_last_sent.reserve(candidates.size());
    for (SendCandidate& candidate : candidates) {
        // Deliberately not stopping at the first entity that does not fit: a
        // later one may be smaller, and an entity too large for the whole budget
        // must not shut the queue down. The cost is that a large record can be
        // passed over for smaller ones while the budget is nearly full, which is
        // what kMaxSnapshotsWithoutSend is underneath to catch.
        const bool fits = estimated_size + candidate.size <= byte_budget;
        if (fits) {
            send_snapshot.entities.push_back(std::move(candidate.entity));
            estimated_size += candidate.size;
        }
        next_last_sent[candidate.net_id] =
            fits ? session.snapshot_send_sequence : candidate.stamp;
    }
    session.last_sent_sequence = std::move(next_last_sent);
    return send_snapshot;
}

void KernelEngine::update_vision_states(float delta_seconds) {
    std::vector<NetId> stale_configs;
    std::vector<NetId> active_agents;

    for (const auto& [agent_net_id, config] : vision_configs_) {
        const std::optional<entt::entity> entity = world_.find_entity(agent_net_id);
        if (!entity.has_value() ||
            !world_.registry().all_of<NetworkIdentity, EntityKind, Transform>(
                *entity)) {
            stale_configs.push_back(agent_net_id);
            continue;
        }
        const KernelColliderTemplateDefinition* vision_collider =
            config.vision_collider_template_id == 0u
                ? nullptr
                : find_collider_template(
                      collider_templates_,
                      config.vision_collider_template_id);
        if (vision_collider != nullptr &&
            vision_collider->shape_type == KernelColliderShapeType_Cone &&
            (vision_collider->purpose_flags & KernelColliderPurpose_Vision) != 0u) {
            active_agents.push_back(agent_net_id);
        } else {
            vision_states_.erase(agent_net_id);
        }
    }
    for (const NetId stale_net_id : stale_configs) {
        vision_configs_.erase(stale_net_id);
        vision_states_.erase(stale_net_id);
    }

    for (const NetId agent_net_id : active_agents) {
        const auto config_iter = vision_configs_.find(agent_net_id);
        const std::optional<entt::entity> agent_entity =
            world_.find_entity(agent_net_id);
        if (config_iter == vision_configs_.end() || !agent_entity.has_value()) {
            continue;
        }
        const KernelAgentVisionConfig& config = config_iter->second;
        const KernelColliderTemplateDefinition* vision_collider =
            find_collider_template(
                collider_templates_,
                config.vision_collider_template_id);
        if (vision_collider == nullptr) {
            continue;
        }
        const float cone_range = collider_template_cone_range(*vision_collider);
        const float fov_degrees =
            collider_template_cone_fov_degrees(*vision_collider);
        const EntityKind& agent_kind =
            world_.registry().get<EntityKind>(*agent_entity);
        const Transform& agent_transform =
            world_.registry().get<Transform>(*agent_entity);

        const glm::vec3 configured_forward = from_kernel_vec3(config.local_forward);
        glm::vec3 forward = is_zero_vec3(configured_forward)
            ? agent_transform.rotation * glm::vec3{1.0f, 0.0f, 0.0f}
            : agent_transform.rotation * configured_forward;
        forward.y = 0.0f;
        forward = normalized_or_forward(forward);
        const glm::vec3 origin =
            agent_transform.position +
            agent_transform.rotation * from_kernel_vec3(config.local_origin);

        VisionRuntimeState& runtime_state = vision_states_[agent_net_id];
        const KernelVisionStateView previous = runtime_state.view;
        KernelVisionStateView view{};
        view.struct_size = sizeof(KernelVisionStateView);
        view.agent_net_id = agent_net_id;
        view.entity_type = static_cast<std::uint16_t>(agent_kind.type);
        view.actor_type = static_cast<std::uint8_t>(agent_kind.actor_type);
        view.camp = config.camp;
        view.vision_origin = to_kernel_vec3(origin);
        view.vision_forward = to_kernel_vec3(forward);
        view.vision_collider_template_id = config.vision_collider_template_id;
        if (world_.registry().all_of<Hitbox>(*agent_entity)) {
            view.resolved_collider_template_id =
                world_.registry().get<Hitbox>(*agent_entity).collider_template_id;
        }
        view.last_seen_target = previous.last_seen_target;
        view.last_known_target_position = previous.last_known_target_position;
        view.time_since_last_seen_target =
            previous.last_seen_target == 0
                ? 0.0f
                : previous.time_since_last_seen_target + delta_seconds;
        view.valid = 1u;

        float nearest_hostile_distance_squared =
            std::numeric_limits<float>::max();
        auto candidates =
            world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
        for (const entt::entity candidate_entity : candidates) {
            const NetworkIdentity& candidate_identity =
                candidates.get<const NetworkIdentity>(candidate_entity);
            const auto candidate_config_iter =
                vision_configs_.find(candidate_identity.net_id);
            if (candidate_config_iter == vision_configs_.end()) {
                continue;
            }
            const KernelAgentVisionConfig& candidate_config =
                candidate_config_iter->second;
            const std::uint8_t relation = classify_agent_relation(
                agent_net_id,
                config.camp,
                candidate_identity.net_id,
                candidate_config.camp);
            if (relation != KernelAgentRelation_Ally &&
                relation != KernelAgentRelation_Hostile &&
                relation != KernelAgentRelation_Neutral) {
                continue;
            }

            const Transform& candidate_transform =
                candidates.get<const Transform>(candidate_entity);
            glm::vec3 delta = candidate_transform.position - origin;
            delta.y = 0.0f;
            const float distance_squared = glm::dot(delta, delta);
            if (distance_squared > cone_range * cone_range) {
                continue;
            }

            bool inside_angle = true;
            if (distance_squared > 0.0001f) {
                const glm::vec3 direction = glm::normalize(delta);
                const float half_fov_radians =
                    fov_degrees * 0.5f *
                    (std::acos(-1.0f) / 180.0f);
                inside_angle = glm::dot(forward, direction) >=
                    std::cos(half_fov_radians);
            }
            if (!inside_angle) {
                continue;
            }

            if (relation == KernelAgentRelation_Hostile) {
                if (view.visible_hostile_count < config.max_visible_hostiles &&
                    view.visible_hostile_count < KERNEL_MAX_VISIBLE_HOSTILES) {
                    view.visible_hostiles[view.visible_hostile_count++] =
                        candidate_identity.net_id;
                }
                if (distance_squared < nearest_hostile_distance_squared) {
                    nearest_hostile_distance_squared = distance_squared;
                    view.current_target_candidate = candidate_identity.net_id;
                    view.relation_to_current_target = KernelAgentRelation_Hostile;
                    view.last_seen_target = candidate_identity.net_id;
                    view.last_known_target_position =
                        to_kernel_vec3(candidate_transform.position);
                    view.time_since_last_seen_target = 0.0f;
                    runtime_state.has_last_seen_target = true;
                }
            } else if (
                relation == KernelAgentRelation_Ally &&
                view.visible_ally_count < config.max_visible_allies &&
                view.visible_ally_count < KERNEL_MAX_VISIBLE_ALLIES) {
                view.visible_allies[view.visible_ally_count++] =
                    candidate_identity.net_id;
            } else if (
                relation == KernelAgentRelation_Neutral &&
                view.visible_neutral_count < config.max_visible_neutrals &&
                view.visible_neutral_count < KERNEL_MAX_VISIBLE_NEUTRALS) {
                view.visible_neutrals[view.visible_neutral_count++] =
                    candidate_identity.net_id;
            }
        }

        runtime_state.view = view;
    }
}

bool KernelEngine::is_entity_relevant_to_session(
    const PeerSession& session,
    const EntitySnapshot& entity,
    const EntitySnapshot* player_entity) const {
    if (entity.net_id == session.player) {
        return true;
    }
    if (entity.type == EntityType::kProjectile) {
        const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
        if (world_entity.has_value() &&
            world_.registry().all_of<NetworkIdentity>(*world_entity) &&
            world_.registry().get<NetworkIdentity>(*world_entity).owner_peer == session.peer) {
            return true;
        }
    }
    if (player_entity == nullptr) {
        return false;
    }

    const float distance = glm::length(entity.position - player_entity->position);
    // To the entity's near edge, not to its origin: see entity_bounding_radius.
    const float effective_distance =
        std::max(0.0f, distance - entity_bounding_radius(entity.net_id));
    // relevant_entities is still last snapshot's set here: sync_session_relevance
    // runs after this, on the snapshot this call helps build.
    const float relevance_distance =
        session.relevant_entities.contains(entity.net_id)
            ? kDefaultEntityRelevanceExitDistanceMeters
            : kDefaultEntityRelevanceDistanceMeters;
    if (effective_distance <= relevance_distance) {
        return true;
    }
    if (entity.type == EntityType::kProjectile &&
        distance <= kDefaultProjectileRelevanceDistanceMeters) {
        const glm::vec3 to_player = player_entity->position - entity.position;
        if (glm::length(entity.velocity) > 0.001f &&
            glm::length(to_player) > 0.001f &&
            glm::dot(glm::normalize(entity.velocity), glm::normalize(to_player)) > 0.5f) {
            return true;
        }
    }
    return false;
}

void KernelEngine::sync_session_relevance(
    PeerSession* session,
    const WorldSnapshot& snapshot) {
    if (session == nullptr || !session->welcomed) {
        return;
    }

    std::unordered_set<NetId> next_relevant;
    std::vector<const EntitySnapshot*> newly_relevant;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.type == EntityType::kProjectile) {
            session->out_of_range_projectiles.erase(entity.net_id);
        }
        if (session->relevant_entities.find(entity.net_id) !=
            session->relevant_entities.end()) {
            next_relevant.insert(entity.net_id);
            continue;
        }
        newly_relevant.push_back(&entity);
    }

    // Nearest first. The quota below defers the rest to later snapshots, and
    // what the player is walking towards must not queue behind whatever the
    // world happened to iterate first -- an arbitrary order would trade the
    // burst for entities popping in back to front.
    const EntitySnapshot* player_entity =
        find_snapshot_entity(snapshot, session->player);
    if (player_entity != nullptr &&
        newly_relevant.size() > kMaxEntitySpawnsPerSnapshot) {
        const glm::vec3 player_position = player_entity->position;
        std::sort(
            newly_relevant.begin(),
            newly_relevant.end(),
            [&player_position](
                const EntitySnapshot* lhs, const EntitySnapshot* rhs) {
                const glm::vec3 lhs_delta = lhs->position - player_position;
                const glm::vec3 rhs_delta = rhs->position - player_position;
                const float lhs_distance = glm::dot(lhs_delta, lhs_delta);
                const float rhs_distance = glm::dot(rhs_delta, rhs_delta);
                if (lhs_distance != rhs_distance) {
                    return lhs_distance < rhs_distance;
                }
                return lhs->net_id < rhs->net_id;
            });
    }

    std::size_t spawns_sent = 0;
    for (const EntitySnapshot* entity : newly_relevant) {
        // The session's own player is never deferred: nothing it predicts can
        // begin before it exists.
        const bool is_own_player = entity->net_id == session->player;
        if (!is_own_player && spawns_sent >= kMaxEntitySpawnsPerSnapshot) {
            // Left out of next_relevant on purpose, so the next snapshot sees
            // it as new again and offers it another turn. Nothing has to
            // remember it: the relevant set is rebuilt from scratch each time.
            continue;
        }
        ++spawns_sent;
        send_entity_spawn(session->peer, *entity);
        // Steps alone cannot tell a session where feet already are, so a
        // session that has just started seeing an entity is handed them.
        // Without this its legs would appear one at a time, each only once
        // it happened to take its first step.
        send_locomotion_baseline(session, entity->net_id);
        if (is_own_player) {
            send_status_effect_state(session, entity->net_id);
        }
        if (entity->type == EntityType::kProp &&
            is_dormant_placed_prop(entity->net_id)) {
            PropStateChangeBatchPacket prop_state{};
            prop_state.server_tick = tick_loop_.current_tick();
            PropStateChangeRecord record{};
            if (make_prop_state_change_record(entity->net_id, &record)) {
                prop_state.records.push_back(record);
                send_prop_state_changes(session, prop_state);
            }
        }
        next_relevant.insert(entity->net_id);
    }

    for (NetId net_id : session->relevant_entities) {
        if (next_relevant.find(net_id) == next_relevant.end()) {
            const std::optional<entt::entity> world_entity =
                world_.find_entity(net_id);
            if (world_entity.has_value() &&
                world_.registry().all_of<ProjectileState>(*world_entity)) {
                session->out_of_range_projectiles.insert(net_id);
            }
            send_entity_despawn(
                session->peer,
                net_id,
                KernelDespawnReason_OutOfRange);
        }
    }
    session->relevant_entities = std::move(next_relevant);
}

// A client silently ignores a snapshot record for a net id it has never been
// given a spawn for -- handle_client_snapshot looks the entity up and skips it.
// Those bytes are therefore not merely early, they are discarded on arrival,
// and while they sat in the send set they displaced entities the client could
// actually have used. Run this after sync_session_relevance, whose quota is
// what leaves entities deferred in the first place.
void KernelEngine::drop_unannounced_entities(
    const PeerSession& session,
    WorldSnapshot* snapshot) const {
    if (snapshot == nullptr) {
        return;
    }
    snapshot->entities.erase(
        std::remove_if(
            snapshot->entities.begin(),
            snapshot->entities.end(),
            [&session](const EntitySnapshot& entity) {
                return !session.relevant_entities.contains(entity.net_id);
            }),
        snapshot->entities.end());
}

// How far an entity reaches from its own origin, horizontally.
//
// Relevance and the priority bands both measure to an origin, which treats every
// entity as a point. That is fine for a 0.8 m grunt and wrong for the legged
// rigs: a quadruped is 24 m across and 28 m tall, a biped is 42 m tall -- taller
// than the radius at which it used to be culled. Subtracting this from the
// centre distance is what lets those two rules ask how far away the *object* is
// rather than how far away its origin is, and it changes nothing for anything
// small, because a grunt's radius is 0.57 m.
//
// Horizontal only. Relevance and the bands are about how much of the view an
// entity occupies from a player standing on roughly the same ground, and a rig's
// height is already most of its half extents.
float KernelEngine::entity_bounding_radius(NetId net_id) const {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Hitbox>(*entity)) {
        return 0.0f;
    }
    // The authored hitbox, which the spawn path copies straight off the entity
    // template -- so this is the size the catalog says the thing is, with no
    // second source to drift from.
    const glm::vec3& extents =
        world_.registry().get<Hitbox>(*entity).half_extents;
    return std::sqrt(extents.x * extents.x + extents.z * extents.z);
}

bool KernelEngine::is_dormant_placed_prop(NetId net_id) const {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<EntityKind, PropWorldMode>(*entity) ||
        world_.registry().get<EntityKind>(*entity).type != EntityType::kProp ||
        world_.registry().get<PropWorldMode>(*entity).mode != PropMode::kPlaced) {
        return false;
    }
    return !world_.registry().all_of<Velocity>(*entity) ||
        glm::length(world_.registry().get<Velocity>(*entity).linear) <= 0.001f;
}

void KernelEngine::send_entity_spawn(PeerId peer, const EntitySnapshot& entity) {
    PeerId owner_peer = 0;
    std::uint32_t actor_template_id = 0;
    std::uint32_t entity_template_id = 0;
    std::uint32_t collider_template_id = 0;
    std::uint32_t item_template_id = 0;
    KernelItemInstanceId item_instance_id = 0;
    std::uint8_t world_item_mode = KernelWorldItemMode_Placed;
    NetId carrier_entity_id = 0;
    glm::vec3 spawn_position = entity.position;
    const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
    if (world_entity.has_value() &&
        world_.registry().all_of<NetworkIdentity>(*world_entity)) {
        owner_peer = world_.registry().get<NetworkIdentity>(*world_entity).owner_peer;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<ActorTemplateRef>(*world_entity)) {
        actor_template_id =
            world_.registry().get<ActorTemplateRef>(*world_entity).actor_template_id;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<EntityTemplateRef>(*world_entity)) {
        entity_template_id =
            world_.registry().get<EntityTemplateRef>(*world_entity).entity_template_id;
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(entity_templates_, entity_template_id);
        collider_template_id =
            entity_template == nullptr ? 0u : entity_template->collider_template_id;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<ItemTemplateRef>(*world_entity)) {
        item_template_id =
            world_.registry().get<ItemTemplateRef>(*world_entity).item_template_id;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<ItemInstanceRef>(*world_entity)) {
        item_instance_id =
            world_.registry().get<ItemInstanceRef>(*world_entity).item_instance_id;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<PropWorldMode>(*world_entity)) {
        world_item_mode = static_cast<std::uint8_t>(
            world_.registry().get<PropWorldMode>(*world_entity).mode);
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<CarriedBy>(*world_entity)) {
        carrier_entity_id =
            world_.registry().get<CarriedBy>(*world_entity).carrier_entity_id;
    }
    if (world_entity.has_value() &&
        world_.registry().all_of<ProjectileState>(*world_entity)) {
        spawn_position =
            world_.registry().get<ProjectileState>(*world_entity).spawn_position;
    }
    const std::vector<std::uint8_t> packet = encode_entity_spawn_packet(
        EntitySpawnPacket{
            entity.net_id,
            entity.type,
            entity.actor_type,
            owner_peer,
            tick_loop_.current_tick(),
            actor_template_id,
            spawn_position,
            entity.rotation,
            entity_template_id,
            collider_template_id,
            item_template_id,
            item_instance_id,
            world_item_mode,
            carrier_entity_id,
        },
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, entity.net_id, peer, 16);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
    if (entity.type == EntityType::kProjectile) {
        send_projectile_spawn_batch(peer, entity);
    }
}

void KernelEngine::queue_prop_state_change(NetId net_id) {
    if (net_id != 0u) pending_prop_state_changes_.push_back(net_id);
}

bool KernelEngine::make_prop_state_change_record(
    NetId net_id,
    PropStateChangeRecord* out_record) const {
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (out_record == nullptr || !entity.has_value() ||
        !world_.registry().all_of<EntityKind, Transform, PropWorldMode>(*entity) ||
        world_.registry().get<EntityKind>(*entity).type != EntityType::kProp) {
        return false;
    }
    const Transform& transform = world_.registry().get<Transform>(*entity);
    PropStateChangeRecord record{};
    record.net_id = net_id;
    record.changed_fields = kPropStateChangeMode | kPropStateChangeTransform |
        kPropStateChangeVelocity;
    record.world_mode = static_cast<KernelWorldItemMode>(
        world_.registry().get<PropWorldMode>(*entity).mode);
    record.carrier_entity_id = world_.registry().all_of<CarriedBy>(*entity)
        ? world_.registry().get<CarriedBy>(*entity).carrier_entity_id
        : 0u;
    record.position = transform.position;
    record.rotation = transform.rotation;
    record.velocity = world_.registry().all_of<Velocity>(*entity)
        ? world_.registry().get<Velocity>(*entity).linear
        : glm::vec3{0.0f};
    if (world_.registry().all_of<Health>(*entity)) {
        const Health& health = world_.registry().get<Health>(*entity);
        record.changed_fields |= kPropStateChangeHealth;
        record.hp = health.hp;
        record.max_hp = health.max_hp;
    }
    *out_record = record;
    return true;
}

void KernelEngine::send_locomotion_steps(
    PeerSession* session,
    const LocomotionStepBatchPacket& packet) {
    if (session == nullptr || packet.records.empty()) {
        return;
    }
    const std::vector<std::uint8_t> encoded =
        encode_locomotion_step_batch_packet(packet, next_packet_sequence_++);
    // Sent unreliably, on the snapshot channel, on purpose: a lost step leaves
    // one leg wrong until its next step because the landing position is
    // absolute, whereas a reliable ordered channel could stall every leg behind
    // one retransmit.
    if (encoded.empty() || !transport_->Send(
            session->peer,
            encoded.data(),
            static_cast<std::uint32_t>(encoded.size()),
            SendMode::kUnreliable,
            ChannelId::kSnapshot)) {
        // Reported rather than swallowed. A step that never leaves the server
        // leaves that leg at its bind pose on every client -- not moving and
        // not on the ground -- and the skeleton render state still reports the
        // pose as PROCEDURAL, so nothing downstream reveals it.
        push_event(KernelEventType_Error, 0u, session->peer, 30u);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(encoded.size()),
        SendMode::kUnreliable,
        ChannelId::kSnapshot);
}

void KernelEngine::send_locomotion_baseline(PeerSession* session, NetId net_id) {
    if (session == nullptr) {
        return;
    }
    const auto state = locomotion_states_.find(net_id);
    if (state == locomotion_states_.end()) {
        return;
    }
    // A baseline is expressed as steps that already finished: apply sees a
    // swing whose whole duration is in the past and plants the foot outright.
    // That reuses the receiving path exactly rather than adding a second one.
    const std::uint32_t current_tick = tick_loop_.current_tick();
    LocomotionStepBatchPacket batch{};
    batch.server_tick = current_tick;
    for (std::uint32_t leg_index = 0u;
         leg_index < state->second.legs.size();
         ++leg_index) {
        const LegLocomotionState& leg = state->second.legs[leg_index];
        if (!leg.foot_initialized) {
            continue;
        }
        LocomotionStepRecord record{};
        record.net_id = net_id;
        record.leg_index = static_cast<std::uint8_t>(leg_index);
        record.start_tick_delta = UINT8_MAX;
        record.landing_target_world = leg.foot_target_world;
        batch.records.push_back(record);
    }
    send_locomotion_steps(session, batch);
}

void KernelEngine::flush_locomotion_steps() {
    const std::uint32_t current_tick = tick_loop_.current_tick();
    if (outgoing_locomotion_steps_.empty()) {
        return;
    }
    // Dropped rather than deferred, unlike a snapshot record. A step's landing
    // target is stale the moment it is not sent, start_tick_delta expires it
    // after 255 ticks anyway, and the channel is already unreliable by design --
    // a lost step leaves one leg wrong until its next step, because the landing
    // position is absolute. So a budget here is not introducing loss, it is
    // choosing which loss instead of leaving it to the network.
    //
    // The leg a step never arrives for is still initialised:
    // send_locomotion_baseline writes every planted leg when an entity becomes
    // relevant, so nothing here can leave a leg at its bind pose.
    struct StepCandidate {
        std::uint32_t band = 0;
        std::uint32_t rotation = 0;
        LocomotionStepRecord record;
    };

    const auto send = [&](PeerSession* session) {
        glm::vec3 origin{0.0f, 0.0f, 0.0f};
        bool has_origin = false;
        const std::optional<entt::entity> player =
            world_.find_entity(session->player);
        if (player.has_value() && world_.registry().all_of<Transform>(*player)) {
            origin = world_.registry().get<Transform>(*player).position;
            has_origin = true;
        }

        std::vector<StepCandidate> candidates;
        candidates.reserve(outgoing_locomotion_steps_.size());
        for (const PendingLocomotionStep& step : outgoing_locomotion_steps_) {
            if (!session->relevant_entities.contains(step.net_id)) {
                continue;
            }
            const std::int32_t age = static_cast<std::int32_t>(
                current_tick - step.event.start_tick);
            if (age < 0 || age >= static_cast<std::int32_t>(UINT8_MAX)) {
                continue;
            }
            LocomotionStepRecord record{};
            record.net_id = step.net_id;
            record.leg_index = static_cast<std::uint8_t>(step.event.leg_index);
            record.start_tick_delta = static_cast<std::uint8_t>(age);
            record.landing_target_world = step.event.landing_target_world;

            // Banded on the rig, not on where its foot happens to land. A rig
            // with 23 m legs can put a foot a whole band away from its body, and
            // banding the two differently would deprioritise a body whose
            // footfalls are being prioritised. One object, one distance -- the
            // same one relevance uses.
            std::uint32_t band = 0;
            if (has_origin) {
                const std::optional<entt::entity> rig =
                    world_.find_entity(step.net_id);
                const glm::vec3 rig_position =
                    rig.has_value() &&
                        world_.registry().all_of<Transform>(*rig)
                    ? world_.registry().get<Transform>(*rig).position
                    : record.landing_target_world;
                const float distance = std::max(
                    0.0f,
                    glm::length(rig_position - origin) -
                        entity_bounding_radius(step.net_id));
                band = distance <= kSnapshotPriorityNearMeters
                    ? 0u
                    : (distance <= kSnapshotPriorityMidMeters ? 1u : 2u);
            }
            candidates.push_back(StepCandidate{
                band,
                locomotion_step_rotation(record.net_id, current_tick),
                record});
        }
        if (candidates.empty()) {
            return;
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const StepCandidate& lhs, const StepCandidate& rhs) {
                if (lhs.band != rhs.band) {
                    return lhs.band < rhs.band;
                }
                if (lhs.rotation != rhs.rotation) {
                    return lhs.rotation < rhs.rotation;
                }
                if (lhs.record.net_id != rhs.record.net_id) {
                    return lhs.record.net_id < rhs.record.net_id;
                }
                return lhs.record.leg_index < rhs.record.leg_index;
            });

        LocomotionStepBatchPacket batch{};
        batch.server_tick = current_tick;
        for (const StepCandidate& candidate : candidates) {
            // Every record is the same size, so the first one that does not fit
            // means none of the rest do either.
            if (estimate_locomotion_step_batch_size(batch.records.size() + 1u) >
                kLocomotionStepBudgetBytes) {
                break;
            }
            batch.records.push_back(candidate.record);
        }
        send_locomotion_steps(session, batch);
    };

    if (config_.mode == KernelMode_ListenServer &&
        local_listen_session_.welcomed) {
        send(&local_listen_session_);
    }
    for (PeerSession& session : peer_sessions_) {
        if (session.welcomed) {
            send(&session);
        }
    }
    outgoing_locomotion_steps_.clear();
}

void KernelEngine::handle_client_locomotion_step_batch(
    const LocomotionStepBatchPacket& packet) {
    for (const LocomotionStepRecord& record : packet.records) {
        LocomotionStepEvent event{};
        event.leg_index = record.leg_index;
        // A baseline arrives as a step older than any swing, which the follower
        // resolves by planting the foot instead of animating a step that is
        // already over.
        event.start_tick =
            packet.server_tick - static_cast<std::uint32_t>(
                record.start_tick_delta);
        event.landing_target_world = record.landing_target_world;
        enqueue_replicated_locomotion_step(record.net_id, event);
    }
}

void KernelEngine::send_prop_state_changes(
    PeerSession* session,
    const PropStateChangeBatchPacket& packet) {
    if (session == nullptr || packet.records.empty()) return;
    const std::vector<std::uint8_t> encoded =
        encode_prop_state_change_batch_packet(packet, next_packet_sequence_++);
    if (encoded.empty() || !transport_->Send(
            session->peer,
            encoded.data(),
            static_cast<std::uint32_t>(encoded.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, 0u, session->peer, 29u);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(encoded.size()),
        SendMode::kReliable,
        ChannelId::kReliableEvent);
    network_stats_.prop_state_bytes_sent += encoded.size();
}

bool KernelEngine::claim_scope_transfer(
    KernelItemInstanceId item_instance_id,
    NetId prop_entity_id) {
    if ((item_instance_id != 0u &&
         claimed_item_instances_.contains(item_instance_id)) ||
        (prop_entity_id != 0u && claimed_prop_entities_.contains(prop_entity_id))) {
        return false;
    }
    if (item_instance_id != 0u) claimed_item_instances_.insert(item_instance_id);
    if (prop_entity_id != 0u) claimed_prop_entities_.insert(prop_entity_id);
    return true;
}

std::pair<std::size_t, std::size_t>
KernelEngine::scope_transfer_publication_checkpoint() const {
    return {events_.size(), pending_prop_state_changes_.size()};
}

void KernelEngine::finish_scope_transfer(
    KernelItemInstanceId item_instance_id,
    NetId prop_entity_id,
    bool committed,
    std::pair<std::size_t, std::size_t> publication_checkpoint) {
    claimed_item_instances_.erase(item_instance_id);
    claimed_prop_entities_.erase(prop_entity_id);
    if (committed) return;
    if (publication_checkpoint.first <= events_.size()) {
        events_.resize(publication_checkpoint.first);
    }
    if (publication_checkpoint.second <= pending_prop_state_changes_.size()) {
        pending_prop_state_changes_.resize(publication_checkpoint.second);
    }
}

void KernelEngine::flush_prop_state_changes() {
    if (pending_prop_state_changes_.empty()) return;
    std::sort(
        pending_prop_state_changes_.begin(), pending_prop_state_changes_.end());
    pending_prop_state_changes_.erase(
        std::unique(
            pending_prop_state_changes_.begin(),
            pending_prop_state_changes_.end()),
        pending_prop_state_changes_.end());

    PropStateChangeBatchPacket batch{};
    batch.server_tick = tick_loop_.current_tick();
    for (const NetId net_id : pending_prop_state_changes_) {
        PropStateChangeRecord record{};
        if (make_prop_state_change_record(net_id, &record)) {
            batch.records.push_back(record);
        }
    }
    pending_prop_state_changes_.clear();
    if (batch.records.empty()) return;

    const auto send = [&](PeerSession* session) {
        PropStateChangeBatchPacket relevant{};
        relevant.server_tick = batch.server_tick;
        for (const PropStateChangeRecord& record : batch.records) {
            if (session->relevant_entities.contains(record.net_id)) {
                relevant.records.push_back(record);
            }
        }
        if (relevant.records.empty()) return;
        send_prop_state_changes(session, relevant);
    };
    if (config_.mode == KernelMode_ListenServer && local_listen_session_.welcomed) {
        send(&local_listen_session_);
    }
    for (PeerSession& session : peer_sessions_) {
        if (session.welcomed) send(&session);
    }
}

void KernelEngine::handle_client_prop_state_change_batch(
    const PropStateChangeBatchPacket& packet) {
    for (const PropStateChangeRecord& record : packet.records) {
        auto entity = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&record](const ClientReplicatedEntity& candidate) {
                return candidate.net_id == record.net_id;
            });
        if (entity == client_replicated_entities_.end()) continue;
        if (entity->has_prop_state &&
            static_cast<std::int32_t>(
                packet.server_tick - entity->prop_state_tick) < 0) {
            continue;
        }
        if (!entity->has_prop_state || packet.server_tick != entity->prop_state_tick) {
            entity->prop_state_fields = 0u;
        }
        if ((record.changed_fields & kPropStateChangeMode) != 0u) {
            entity->world_item_mode = static_cast<std::uint8_t>(record.world_mode);
            entity->carrier_entity_id = record.carrier_entity_id;
        }
        if ((record.changed_fields & kPropStateChangeTransform) != 0u) {
            entity->position = record.position;
            entity->rotation = record.rotation;
        }
        if ((record.changed_fields & kPropStateChangeVelocity) != 0u) {
            entity->velocity = record.velocity;
        }
        if ((record.changed_fields & kPropStateChangeHealth) != 0u) {
            entity->hp = record.hp;
            entity->max_hp = record.max_hp;
            entity->hp_known = true;
        }
        entity->prop_state_tick = packet.server_tick;
        entity->prop_state_fields |= record.changed_fields;
        entity->has_prop_state = true;
        // A throw arrives here as one record carrying mode, transform and
        // velocity together (make_prop_state_change_record always sends the
        // three), which is the whole initial condition of the flight. Anchoring
        // on it lets the render pass evaluate the trajectory instead of waiting
        // for the snapshot rotation to sample it again. Re-anchoring on any
        // later in-flight record is deliberate: the server is authoritative, so
        // a correction mid-flight moves the curve rather than being argued with.
        const bool in_flight =
            entity->world_item_mode == KernelWorldItemMode_InFlight;
        const bool carries_initial_condition =
            (record.changed_fields & kPropStateChangeTransform) != 0u &&
            (record.changed_fields & kPropStateChangeVelocity) != 0u;
        if (in_flight && carries_initial_condition) {
            entity->thrown_anchor_position = entity->position;
            entity->thrown_anchor_velocity = entity->velocity;
            entity->thrown_anchor_tick = packet.server_tick;
            entity->has_thrown_anchor = true;
        } else if (!in_flight) {
            entity->has_thrown_anchor = false;
        }
    }
    if (has_client_snapshot_) rebuild_render_states();
}

bool KernelEngine::thrown_prop_render_transform(
    const ClientReplicatedEntity& replicated,
    std::uint32_t render_tick,
    glm::vec3* out_position,
    glm::vec3* out_velocity) const {
    if (out_position == nullptr || out_velocity == nullptr ||
        replicated.type != EntityType::kProp ||
        !replicated.has_thrown_anchor ||
        replicated.world_item_mode != KernelWorldItemMode_InFlight) {
        return false;
    }
    const KernelEntityTemplateDefinition* entity_template =
        find_entity_template(entity_templates_, replicated.entity_template_id);
    if (entity_template == nullptr ||
        entity_template->prop.throw_trajectory_projectile_template_id == 0u) {
        return false;
    }
    const KernelProjectileTemplateDefinition* trajectory =
        find_projectile_template(
            projectile_templates_,
            entity_template->prop.throw_trajectory_projectile_template_id);
    if (trajectory == nullptr) {
        return false;
    }
    // Signed: the render instant sits an interpolation delay behind the server,
    // so the first frames after a throw are still earlier than the anchor. The
    // curve is only run forwards -- backwards would draw the prop somewhere it
    // was never thrown from.
    const std::int32_t elapsed_ticks =
        static_cast<std::int32_t>(render_tick - replicated.thrown_anchor_tick);
    const float elapsed_seconds = elapsed_ticks <= 0
        ? 0.0f
        : static_cast<float>(elapsed_ticks) * tick_loop_.fixed_delta_seconds();
    // The same evaluator the server steps the prop with, over the same motion
    // model and gravity -- both read from the trajectory projectile the item's
    // throw policy names, which the client already holds in the synced catalog.
    // Nothing about this needs a wire field that is not already sent.
    const ProjectileMotionModel motion_model =
        to_projectile_motion_model(trajectory->mechanics.motion_model);
    const glm::vec3 gravity = from_kernel_vec3(trajectory->mechanics.gravity);
    *out_position = projectile_position_at(
        replicated.thrown_anchor_position,
        replicated.thrown_anchor_velocity,
        motion_model,
        gravity,
        elapsed_seconds);
    *out_velocity = projectile_velocity_at(
        replicated.thrown_anchor_velocity,
        motion_model,
        gravity,
        elapsed_seconds);
    return true;
}

void KernelEngine::send_projectile_spawn_batch(
    PeerId peer,
    const EntitySnapshot& entity) {
    const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
    if (!world_entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, ProjectileState, Velocity>(
            *world_entity)) {
        return;
    }
    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*world_entity);
    const ProjectileState& projectile =
        world_.registry().get<ProjectileState>(*world_entity);
    const Velocity& velocity = world_.registry().get<Velocity>(*world_entity);
    ProjectileSpawnRecord record{};
    record.projectile_net_id = entity.net_id;
    record.owner_net_id = projectile.shooter_net_id;
    record.owner_peer = identity.owner_peer;
    record.action_instance_id = projectile.action_instance_id;
    record.spawn_position = projectile.spawn_position;
    record.initial_velocity =
        glm::length(projectile.initial_velocity) > 0.0001f
            ? projectile.initial_velocity
            : velocity.linear;

    ProjectileSpawnGroup group{};
    group.projectile_template_id = projectile.projectile_template_id;
    group.records.push_back(record);

    ProjectileSpawnBatchPacket batch{};
    batch.server_tick = projectile.spawn_tick;
    batch.server_time_us =
        tick_time_us(projectile.spawn_tick, tick_loop_.fixed_delta_seconds());
    batch.catalog_hash = catalog_hash_;
    batch.groups.push_back(std::move(group));

    const std::vector<std::uint8_t> packet =
        encode_projectile_spawn_batch_packet(batch, next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, entity.net_id, peer, 24);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
}

void KernelEngine::send_entity_despawn(
    PeerId peer,
    NetId net_id,
    std::uint32_t reason) {
    const std::vector<std::uint8_t> packet = encode_entity_despawn_packet(
        EntityDespawnPacket{net_id, tick_loop_.current_tick(), reason},
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, net_id, peer, 17);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
}

void KernelEngine::send_entity_template_update(
    PeerId peer,
    NetId net_id,
    std::uint32_t actor_template_id) {
    const std::vector<std::uint8_t> packet = encode_entity_template_update_packet(
        EntityTemplateUpdatePacket{
            net_id,
            tick_loop_.current_tick(),
            actor_template_id,
        },
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, net_id, peer, 27);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
}

void KernelEngine::rebuild_render_states() {
    rebuild_render_states_at_time(client_local_time_us_);
}

// Whether this kernel has a client half at all. Both modes that do render
// through it: the snapshot buffer, its interpolation delay and its relevance
// set are what presentation is defined against, and a listen server is not
// exempt -- its client half simply receives over loopback instead of a socket.
// A server with no client half (dedicated) renders its own world directly.
bool KernelEngine::render_states_from_snapshot() const {
    return config_.mode == KernelMode_Client ||
        config_.mode == KernelMode_ListenServer;
}

void KernelEngine::rebuild_render_states_at_time(
    std::uint64_t client_render_time_us) {
    const auto cost_start = std::chrono::steady_clock::now();
    if (render_states_from_snapshot()) {
        rebuild_render_states_from_snapshot(client_render_time_us);
        sync_client_render_colliders();
        report_render_state_overflow_if_needed();
        benchmark_stats_.render_solver_cost_us +=
            std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
        return;
    }
    rebuild_render_states_from_world();
    report_render_state_overflow_if_needed();
    benchmark_stats_.render_solver_cost_us +=
        std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
}

std::size_t KernelEngine::skeleton_pose_history_capacity() const {
    // Deep enough to cover the interpolation delay the root is rendered at
    // (snapshot_interval_ticks * 2), with room for the snapshot buffer to run a
    // little behind, and a floor so a degenerate tick config still keeps a
    // usable window.
    const std::size_t delay_ticks =
        static_cast<std::size_t>(tick_loop_.snapshot_interval_ticks()) * 2u;
    return std::max<std::size_t>(8u, delay_ticks * 3u);
}

void KernelEngine::rebuild_skeleton_presentation_at_time(
    std::uint64_t client_render_time_us) {
    rebuild_render_states_at_time(client_render_time_us);
    skeleton_presentation_poses_.clear();
    // The pose has to be evaluated at the instant the roots just rebuilt were,
    // or the two are sampled apart and every foot slides. That instant only
    // exists once there are snapshots to interpolate between, which is what
    // client_render_server_time_us reports: it fails on an empty buffer, so a
    // kernel with no client half (dedicated, rendering its own world) falls
    // through to the live pose below without a mode test here.
    std::uint64_t pose_evaluation_time_us = 0;
    const bool interpolate_pose = client_render_server_time_us(
        client_render_time_us,
        &pose_evaluation_time_us);
    // Mirrors client_render_server_time_us: a listen server's client half runs
    // off the same clock as the server, so no conversion applies there.
    const bool shares_server_clock = config_.mode == KernelMode_ListenServer;
    for (const RenderEntityState& render_state : render_states_) {
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(entity_templates_, render_state.template_id);
        if (entity_template == nullptr ||
            entity_template->skeleton.struct_size <
                sizeof(KernelSkeletonBindingDefinition)) {
            continue;
        }
        const RuntimeSkeletonAsset* asset = find_skeleton_asset(
            skeleton_assets_,
            entity_template->skeleton.skeleton_asset_id);
        if (asset == nullptr) {
            continue;
        }
        SkeletonPresentationPose pose;
        pose.entity_net_id = render_state.net_id;
        pose.skeleton_asset_id = asset->skeleton_asset_id;
        pose.skeleton_content_hash = asset->skeleton_content_hash;
        pose.pose_tick = tick_loop_.current_tick();
        pose.pose_time_us = client_render_time_us;
        // What the client half reconstructed from replicated steps is what
        // gets rendered, in every topology that has one. The authoritative
        // solve is the fallback, for a rig this kernel simulates that the
        // reconstruction has not reached yet -- and on a dedicated server,
        // which has no client half at all, it is the only answer there is.
        const LocomotionState* solved = nullptr;
        if (const auto follower =
                follower_locomotion_states_.find(render_state.net_id);
            follower != follower_locomotion_states_.end()) {
            solved = &follower->second;
        } else if (const auto locomotion =
                       locomotion_states_.find(render_state.net_id);
                   locomotion != locomotion_states_.end()) {
            solved = &locomotion->second;
        }
        if (solved != nullptr && solved->pose_valid &&
            solved->local_pose.size() == asset->bind_pose.size()) {
            pose.pose_flags = KERNEL_SKELETON_POSE_FLAG_PROCEDURAL;
            pose.local_transforms = solved->local_pose;
            const auto history =
                skeleton_pose_history_.find(render_state.net_id);
            std::uint32_t sampled_tick = 0u;
            if (interpolate_pose && history != skeleton_pose_history_.end() &&
                sample_skeleton_pose_history(
                    history->second,
                    pose_evaluation_time_us,
                    &pose.local_transforms,
                    &sampled_tick) &&
                pose.local_transforms.size() == asset->bind_pose.size()) {
                pose.pose_tick = sampled_tick;
                // The evaluated instant, expressed in the CALLER's clock.
                // pose_evaluation_time_us is a server time, while every
                // consumer of pose_time_us works in client render time --
                // get_skeleton_render_states_at_time filters on
                // pose_time_us <= requested_render_time_us. Handing back the
                // server instant compares two different clocks: a server that
                // has been up longer than the client, which is the ordinary
                // case, makes every pose look like it is from the future and
                // the query returns nothing at all. A listen server never sees
                // it because there the two clocks are the same value.
                //
                // Converting rather than just reporting the request keeps the
                // information the field exists for: snapshot interpolation
                // clamps to the buffer, so the pose really can be older than
                // what was asked for, and the caller is entitled to know.
                pose.pose_time_us = shares_server_clock
                    ? pose_evaluation_time_us
                    : offset_time_us(
                          pose_evaluation_time_us,
                          -client_clock_offset_us_);
            } else {
                pose.local_transforms = solved->local_pose;
            }
        } else {
            pose.pose_flags = KERNEL_SKELETON_POSE_FLAG_BIND_POSE;
            pose.local_transforms = asset->bind_pose;
        }
        skeleton_presentation_poses_.push_back(std::move(pose));
    }
}

void KernelEngine::report_render_state_overflow_if_needed() {
    if (render_states_.size() > config_.max_render_states) {
        spdlog::error(
            "[NetworkExample] render state count exceeds configured cap "
            "render_states={} max_render_states={}",
            render_states_.size(),
            config_.max_render_states);
        push_event(KernelEventType_Error, 0, 0, 25);
    }
}

void KernelEngine::rebuild_render_states_from_world() {
    render_states_.clear();
    if (config_.mode == KernelMode_Client) {
        append_predicted_local_render_state();
        append_predicted_projectile_render_states();
    }
    auto view = world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
    for (const entt::entity entity : view) {
        if (world_.registry().all_of<ServerOnly>(entity)) {
            continue;
        }
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        if (is_actor_pending_first_physics(identity.net_id)) {
            continue;
        }
        RenderEntityState state = render_state_from_world_entity(
            world_,
            entity,
            entity_id_for_net_id(identity.net_id));
        const EntityKind& kind = view.get<const EntityKind>(entity);
        if (kind.type == EntityType::kProjectile &&
            world_.registry().all_of<ProjectileState>(entity)) {
            const ProjectileState& projectile =
                world_.registry().get<ProjectileState>(entity);
            state.collider_template_id =
                collider_template_id_for_projectile_template(
                    projectile.projectile_template_id);
        } else if (world_.registry().all_of<Hitbox>(entity)) {
            const Hitbox& hitbox = world_.registry().get<Hitbox>(entity);
            state.collider_template_id = hitbox.collider_template_id;
        } else if (kind.type == EntityType::kActor && state.template_id != 0u) {
            state.collider_template_id =
                collider_template_id_for_actor_template(state.template_id);
        } else {
            state.collider_template_id = 0;
        }
        render_states_.push_back(state);
    }
}

void KernelEngine::rebuild_render_states_from_snapshot(
    std::uint64_t client_render_time_us) {
    const auto render_template_id =
        [](const ClientReplicatedEntity& entity) -> std::uint32_t {
        if (entity.type == EntityType::kActor) {
            return entity.actor_template_id;
        }
        if (entity.type == EntityType::kProjectile) {
            return entity.projectile_template_id;
        }
        if (entity.type == EntityType::kProp &&
            entity.item_instance_id != 0u &&
            entity.item_template_id != 0u) {
            return entity.item_template_id;
        }
        return entity.entity_template_id;
    };
    render_states_.clear();
    append_predicted_local_render_state();
    append_predicted_projectile_render_states();

    WorldSnapshot render_snapshot;
    const WorldSnapshot& snapshot =
        build_interpolated_snapshot(client_render_time_us, &render_snapshot)
            ? render_snapshot
            : latest_client_snapshot_;
    current_render_time_us_ =
        tick_time_us(snapshot.header.server_tick, tick_loop_.fixed_delta_seconds());
    has_client_render_time_ = true;
    std::unordered_set<NetId> rendered_entities;
    for (const PredictedProjectile& projectile : predicted_projectiles_) {
        if (projectile.net_id != 0) {
            rendered_entities.insert(projectile.net_id);
        }
    }
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (has_predicted_local_entity_ && entity.net_id == local_player_net_id_) {
            continue;
        }
        if (client_snapshot_entity_is_tombstoned(
                entity.net_id,
                snapshot.header.server_tick)) {
            continue;
        }
        if (has_predicted_projectile_net_id(entity.net_id)) {
            rendered_entities.insert(entity.net_id);
            continue;
        }
        auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& replicated_entity) {
                return replicated_entity.net_id == entity.net_id;
            });
        if (replicated == client_replicated_entities_.end()) {
            continue;
        }
        if (entity.type == EntityType::kActor &&
            replicated->actor_template_id == 0u) {
            continue;
        }
        if (entity.type == EntityType::kProjectile &&
            (replicated->projectile_template_id == 0u ||
             replicated->collider_template_id == 0u)) {
            continue;
        }

        EntitySnapshot render_entity = entity;
        render_entity.item_template_id = replicated->item_template_id;
        render_entity.item_instance_id = replicated->item_instance_id;
        render_entity.world_item_mode = replicated->world_item_mode;
        render_entity.carrier_entity_id = replicated->carrier_entity_id;
        const bool use_reliable_prop_state =
            entity.type == EntityType::kProp && replicated->has_prop_state &&
            static_cast<std::int32_t>(
                snapshot.header.server_tick - replicated->prop_state_tick) <= 0;
        if (use_reliable_prop_state) {
            if ((replicated->prop_state_fields & kPropStateChangeTransform) != 0u) {
                render_entity.position = replicated->position;
                render_entity.rotation = replicated->rotation;
            }
            if ((replicated->prop_state_fields & kPropStateChangeVelocity) != 0u) {
                render_entity.velocity = replicated->velocity;
            }
            if ((replicated->prop_state_fields & kPropStateChangeHealth) != 0u) {
                render_entity.hp = replicated->hp;
                render_entity.max_hp = replicated->max_hp;
                render_entity.state_flags &= ~kSnapshotStateFlagHpUnknown;
            }
        }
        // A prop in flight is drawn from its own trajectory instead of from the
        // snapshot sample. This is not smoothing: the server steps the throw
        // with projectile_position_at over the same anchor, model and gravity,
        // so the two agree exactly, and anything that changes the flight --
        // an impulse, a correction -- arrives as a prop-state record and
        // re-anchors it. What it buys is independence from the send set, where
        // a weight-1 prop record among a crowd of weight-8 actors can go the
        // full kMaxSnapshotsWithoutSend between samples; the interpolator can
        // only render that as hold-then-jump.
        glm::vec3 thrown_position{0.0f, 0.0f, 0.0f};
        glm::vec3 thrown_velocity{0.0f, 0.0f, 0.0f};
        if (thrown_prop_render_transform(
                *replicated,
                snapshot.header.server_tick,
                &thrown_position,
                &thrown_velocity)) {
            render_entity.position = thrown_position;
            render_entity.velocity = thrown_velocity;
        }
        if ((render_entity.state_flags & kSnapshotStateFlagHpUnknown) != 0u) {
            if (replicated->hp_known) {
                render_entity.hp = replicated->hp;
                render_entity.max_hp = replicated->max_hp;
                render_entity.state_flags &= ~kSnapshotStateFlagHpUnknown;
            }
        }
        render_states_.push_back(render_state_from_snapshot_entity(
            render_entity,
            entity_id_for_net_id(entity.net_id)));
        RenderEntityState& state = render_states_.back();
        state.template_id = render_template_id(*replicated);
        if (state.entity_type == static_cast<std::uint16_t>(EntityType::kActor) &&
            replicated->actor_template_id != 0u) {
            state.collider_template_id =
                collider_template_id_for_actor_template(replicated->actor_template_id);
        } else if (
            state.entity_type == static_cast<std::uint16_t>(EntityType::kProjectile)) {
            state.collider_template_id = replicated->collider_template_id;
        } else {
            state.collider_template_id = replicated->collider_template_id;
        }
        rendered_entities.insert(entity.net_id);
        replicated->active = true;
        if (!use_reliable_prop_state) {
            replicated->position = entity.position;
            replicated->rotation = entity.rotation;
            replicated->velocity = entity.velocity;
        }
        if (!use_reliable_prop_state &&
            (entity.state_flags & kSnapshotStateFlagHpUnknown) == 0u) {
            replicated->hp = render_entity.hp;
            replicated->max_hp = render_entity.max_hp;
            replicated->hp_known = true;
        }
    }

    for (const ClientReplicatedEntity& entity : client_replicated_entities_) {
        const auto tombstone = client_despawned_entities_.find(entity.net_id);
        if (entity.net_id == local_player_net_id_ ||
            rendered_entities.find(entity.net_id) != rendered_entities.end() ||
            tombstone != client_despawned_entities_.end()) {
            continue;
        }
        if (entity.type == EntityType::kActor && entity.actor_template_id == 0u) {
            continue;
        }
        if (entity.type == EntityType::kProjectile &&
            (entity.projectile_template_id == 0u ||
             entity.collider_template_id == 0u)) {
            continue;
        }
        const std::uint32_t collider_template_id =
            entity.type == EntityType::kActor
                ? collider_template_id_for_actor_template(entity.actor_template_id)
                : entity.collider_template_id;
        // This loop is the omitted case -- entities the render snapshot has no
        // record for -- which is exactly where a starved prop lands, so a prop
        // in flight is evaluated here rather than drawn at the last position it
        // was sampled at. It is derived from an authoritative anchor, not
        // guessed, so it is reported Predicted rather than Stale.
        glm::vec3 position = entity.position;
        glm::vec3 velocity{0.0f, 0.0f, 0.0f};
        const bool thrown = thrown_prop_render_transform(
            entity,
            snapshot.header.server_tick,
            &position,
            &velocity);
        render_states_.push_back(RenderEntityState{
            entity_id_for_net_id(entity.net_id),
            entity.net_id,
            static_cast<std::uint16_t>(entity.type),
            static_cast<std::uint16_t>(entity.actor_type),
            entity.owner_peer,
            to_kernel_vec3(position),
            to_kernel_quat(entity.rotation),
            to_kernel_vec3(velocity),
            entity.hp_known ? entity.hp : static_cast<std::uint16_t>(0),
            entity.hp_known ? entity.max_hp : static_cast<std::uint16_t>(0),
            0,
            entity.hp_known ? 0u : kVisualFlagHpUnknown,
            0,
            0,
            thrown ? RenderEntityStatus_Predicted : RenderEntityStatus_Stale,
            render_template_id(entity),
            collider_template_id,
            KernelActionRuntimeView{sizeof(KernelActionRuntimeView)},
            KernelVec3{1.0f, 0.0f, 0.0f},
            entity.item_instance_id,
            entity.world_item_mode,
            0,
            0,
            entity.carrier_entity_id,
        });
    }
}

void KernelEngine::publish_snapshot() {
    const std::uint32_t server_time_ms = static_cast<std::uint32_t>(
        tick_loop_.current_tick() * tick_loop_.fixed_delta_seconds() * 1000.0f);
    latest_snapshot_ = build_world_snapshot(
        world_,
        tick_loop_.current_tick(),
        server_time_ms,
        local_last_processed_input_seq_);
    filter_pending_first_physics_actors(&latest_snapshot_);
    spdlog::debug(
        "{}",
        fmt::format(
            "snapshot tick={} entities={}",
            latest_snapshot_.header.server_tick,
            latest_snapshot_.entities.size()));

    if (config_.mode == KernelMode_ListenServer &&
        listen_server_transport_ != nullptr) {
        local_listen_session_.peer = kLocalListenPeerId;
        local_listen_session_.player = local_player_net_id_;
        local_listen_session_.last_processed_input_seq = local_last_processed_input_seq_;
        local_listen_session_.welcomed = local_player_net_id_ != 0;
        WorldSnapshot peer_snapshot =
            build_relevant_snapshot(local_listen_session_, server_time_ms);
        sync_session_relevance(&local_listen_session_, peer_snapshot);
        drop_unannounced_entities(local_listen_session_, &peer_snapshot);
        const WorldSnapshot send_snapshot = build_snapshot_send_set(
            local_listen_session_,
            peer_snapshot,
            kSnapshotSendBudgetBytes);
        const std::vector<std::uint8_t> packet =
            encode_snapshot_packet(send_snapshot, next_packet_sequence_++);
        if (!listen_server_transport_->Send(
                kLocalListenPeerId,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kSnapshot)) {
            push_event(KernelEventType_Error, 0, kLocalListenPeerId, 7);
        } else {
            record_sent_packet(
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kSnapshot);
        }
    }

    if (is_server_mode(config_.mode)) {
        for (PeerSession& session : peer_sessions_) {
            if (!session.welcomed) {
                continue;
            }
            WorldSnapshot peer_snapshot =
                build_relevant_snapshot(session, server_time_ms);
            sync_session_relevance(&session, peer_snapshot);
            drop_unannounced_entities(session, &peer_snapshot);
            const WorldSnapshot send_snapshot = build_snapshot_send_set(
                session,
                peer_snapshot,
                kSnapshotSendBudgetBytes);
            const std::vector<std::uint8_t> packet =
                encode_snapshot_packet(send_snapshot, next_packet_sequence_++);
            if (!transport_->Send(
                    session.peer,
                    packet.data(),
                    static_cast<std::uint32_t>(packet.size()),
                    SendMode::kUnreliable,
                    ChannelId::kSnapshot)) {
                push_event(KernelEventType_Error, 0, session.peer, 7);
            } else {
                record_sent_packet(
                    static_cast<std::uint32_t>(packet.size()),
                    SendMode::kUnreliable,
                    ChannelId::kSnapshot);
            }
        }
    }
}

void KernelEngine::send_gameplay_catalog_manifest_request() {
    GameplayCatalogManifestRequestPacket request;
    const std::vector<std::uint8_t> packet =
        encode_gameplay_catalog_manifest_request_packet(
            request,
            next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError_Transport);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kSession);
    gameplay_catalog_sync_state_ =
        KernelGameplayCatalogSyncState_FetchingManifest;
    gameplay_catalog_sync_elapsed_us_ = 0;
}

void KernelEngine::send_client_handshake() {
    if (client_handshake_sent_) {
        return;
    }

    const std::vector<std::uint8_t> packet =
        encode_handshake_packet(
            make_client_handshake(catalog_version_, catalog_hash_),
            next_packet_sequence_++);
    if (!transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, kServerPeerId, 9);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kSession);
    client_handshake_sent_ = true;
}

void KernelEngine::pump_gameplay_catalog_transfers() {
    if (!is_server_mode(config_.mode) || transport_ == nullptr ||
        gameplay_catalog_sync_bundle_.empty()) {
        return;
    }
    for (auto iter = gameplay_catalog_transfers_.begin();
         iter != gameplay_catalog_transfers_.end();) {
        std::size_t chunks_sent = 0;
        while (iter->second.offset < gameplay_catalog_sync_bundle_.size() &&
               chunks_sent < 4) {
            const std::size_t chunk_size = std::min(
                kGameplayCatalogBundleChunkBytes,
                gameplay_catalog_sync_bundle_.size() - iter->second.offset);
            GameplayCatalogBundleChunkPacket chunk;
            std::copy(
                std::begin(gameplay_catalog_manifest_.bundle_sha256),
                std::end(gameplay_catalog_manifest_.bundle_sha256),
                chunk.bundle_sha256.begin());
            chunk.offset = static_cast<std::uint32_t>(iter->second.offset);
            chunk.total_size =
                static_cast<std::uint32_t>(gameplay_catalog_sync_bundle_.size());
            chunk.bytes.assign(
                gameplay_catalog_sync_bundle_.begin() + iter->second.offset,
                gameplay_catalog_sync_bundle_.begin() +
                    iter->second.offset + chunk_size);
            const std::vector<std::uint8_t> packet =
                encode_gameplay_catalog_bundle_chunk_packet(
                    chunk,
                    next_packet_sequence_++);
            if (!transport_->Send(
                    iter->first,
                    packet.data(),
                    static_cast<std::uint32_t>(packet.size()),
                    SendMode::kReliable,
                    ChannelId::kSession)) {
                push_event(KernelEventType_Error, 0, iter->first, 29);
                break;
            }
            record_sent_packet(
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kReliable,
                ChannelId::kSession);
            iter->second.offset += chunk_size;
            ++chunks_sent;
        }
        if (iter->second.offset >= gameplay_catalog_sync_bundle_.size()) {
            iter = gameplay_catalog_transfers_.erase(iter);
        } else {
            ++iter;
        }
    }
}

void KernelEngine::fail_gameplay_catalog_sync(
    KernelGameplayCatalogSyncError error) {
    gameplay_catalog_sync_error_ = error;
    gameplay_catalog_sync_state_ = KernelGameplayCatalogSyncState_Failed;
    downloaded_gameplay_catalog_bundle_.clear();
}

void KernelEngine::send_clock_sync_ping(
    PeerSession* session,
    std::uint64_t server_time_us) {
    if (session == nullptr || !session->welcomed || transport_ == nullptr) {
        return;
    }

    const std::uint32_t nonce = next_clock_sync_nonce_++;
    PingPongPacket ping{nonce, server_time_us, 0, 0};
    ping.server_rtt_us = session->last_clock_sync_rtt_us;
    ping.server_jitter_us = session->last_clock_sync_jitter_us;
    const std::vector<std::uint8_t> packet =
        encode_ping_pong_packet(ping, next_packet_sequence_++);
    if (!transport_->Send(
            session->peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, session->peer, 21);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kSession);

    session->pending_clock_sync_nonce = nonce;
    session->pending_clock_sync_server_time_us = server_time_us;
    session->last_clock_sync_sent_server_time_us = server_time_us;
}

void KernelEngine::send_due_clock_sync_pings(std::uint64_t server_time_us) {
    if (!is_server_mode(config_.mode)) {
        return;
    }
    for (PeerSession& session : peer_sessions_) {
        if (!session.welcomed || session.pending_clock_sync_nonce != 0) {
            continue;
        }
        if (!session.has_clock_sync ||
            server_time_us >= session.last_clock_sync_sent_server_time_us +
                                  kClockSyncIntervalUs) {
            send_clock_sync_ping(&session, server_time_us);
        }
    }
}

void KernelEngine::send_reliable_event(PeerId peer, const KernelEvent& event) {
    const std::vector<std::uint8_t> packet =
        encode_reliable_event_packet(event, next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, event.net_id, peer, 15);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
}

void KernelEngine::send_gameplay_request_outcome(
    PeerId peer,
    const KernelGameplayRequestOutcome& outcome) {
    const std::vector<std::uint8_t> packet =
        encode_gameplay_request_outcome_packet(
            outcome, next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, outcome.prop_entity_id, peer, 32);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kReliableEvent);
}

void KernelEngine::flush_network_gameplay_request_outcomes() {
    for (const auto& [peer, outcome] : pending_network_gameplay_outcomes_) {
        send_gameplay_request_outcome(peer, outcome);
    }
    pending_network_gameplay_outcomes_.clear();
}

bool KernelEngine::send_inventory_snapshot(
    PeerSession* session,
    KernelInventoryContainerId container_id) {
    if (session == nullptr || !session->welcomed) return false;
    const InventoryContainerRecord* container =
        item_store_.find_container(container_id);
    if (container == nullptr || container->owner_entity_id != session->player) {
        return false;
    }
    std::vector<InventorySnapshotEntry> entries;
    entries.reserve(container->slots.size());
    for (std::uint16_t slot = 0; slot < container->slots.size(); ++slot) {
        const KernelItemInstanceId id = container->slots[slot];
        if (id == 0u) continue;
        entries.push_back(InventorySnapshotEntry{
            slot,
            inventory_wire_item(item_store_.item_view(id)),
        });
    }
    const std::size_t page_count_size = std::max<std::size_t>(
        1u,
        (entries.size() + kInventorySnapshotEntriesPerPage - 1u) /
            kInventorySnapshotEntriesPerPage);
    if (page_count_size > UINT16_MAX) return false;
    ITransport* target_transport =
        session->peer == kLocalListenPeerId && listen_server_transport_ != nullptr
        ? static_cast<ITransport*>(listen_server_transport_)
        : transport_.get();
    if (target_transport == nullptr) return false;
    for (std::size_t page_index = 0; page_index < page_count_size; ++page_index) {
        const std::size_t begin = page_index * kInventorySnapshotEntriesPerPage;
        const std::size_t end = std::min(
            entries.size(), begin + kInventorySnapshotEntriesPerPage);
        InventorySnapshotPagePacket page;
        page.inventory_container_id = container_id;
        page.owner_entity_id = container->owner_entity_id;
        page.revision = container->revision;
        page.slot_capacity = container->slot_capacity;
        page.page_index = static_cast<std::uint16_t>(page_index);
        page.page_count = static_cast<std::uint16_t>(page_count_size);
        if (begin < end) {
            page.entries.assign(entries.begin() + begin, entries.begin() + end);
        }
        const std::vector<std::uint8_t> packet =
            encode_inventory_snapshot_page_packet(
                page, next_packet_sequence_++);
        if (packet.empty() || !target_transport->Send(
                session->peer,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kReliable,
                ChannelId::kReliableEvent)) {
            return false;
        }
        network_stats_.inventory_snapshot_bytes_sent += packet.size();
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    }
    session->inventory_revisions[container_id] = container->revision;
    return true;
}

bool KernelEngine::send_inventory_delta_batch(
    PeerSession* session,
    KernelInventoryContainerId container_id,
    std::span<const KernelInventoryDelta> deltas) {
    if (session == nullptr || !session->welcomed || deltas.empty()) return false;
    const InventoryContainerRecord* container =
        item_store_.find_container(container_id);
    if (container == nullptr || container->owner_entity_id != session->player) {
        return false;
    }
    InventoryDeltaBatchPacket batch;
    batch.inventory_container_id = container_id;
    batch.first_revision = deltas.front().revision;
    batch.records.reserve(deltas.size());
    for (const KernelInventoryDelta& delta : deltas) {
        InventoryDeltaRecord record;
        record.type = static_cast<KernelInventoryDeltaType>(delta.type);
        record.slot = delta.slot;
        record.previous_slot = delta.previous_slot;
        record.changed_fields = delta.changed_fields;
        record.item = inventory_wire_item(delta.item);
        batch.records.push_back(std::move(record));
    }
    const std::vector<std::uint8_t> packet =
        encode_inventory_delta_batch_packet(batch, next_packet_sequence_++);
    ITransport* target_transport =
        session->peer == kLocalListenPeerId && listen_server_transport_ != nullptr
        ? static_cast<ITransport*>(listen_server_transport_)
        : transport_.get();
    if (packet.empty() || target_transport == nullptr ||
        !target_transport->Send(
            session->peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        return false;
    }
    session->inventory_revisions[container_id] = deltas.back().revision;
    network_stats_.inventory_delta_bytes_sent += packet.size();
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kReliableEvent);
    return true;
}

void KernelEngine::flush_inventory_replication() {
    const auto flush_session = [&](PeerSession* session) {
        if (session == nullptr || !session->welcomed || session->player == 0u) {
            return;
        }
        for (const KernelInventoryContainerId container_id :
             item_store_.containers_for_owner(session->player)) {
            const InventoryContainerRecord* container =
                item_store_.find_container(container_id);
            if (container == nullptr) continue;
            const auto cursor = session->inventory_revisions.find(container_id);
            if (cursor == session->inventory_revisions.end()) {
                send_inventory_snapshot(session, container_id);
                continue;
            }
            if (cursor->second >= container->revision) continue;
            const std::vector<KernelInventoryDelta> deltas =
                item_store_.inventory_deltas_since(
                    container_id,
                    cursor->second,
                    kInventoryDeltaRecordsPerPacket);
            if (deltas.empty() || deltas.front().revision != cursor->second + 1u) {
                send_inventory_snapshot(session, container_id);
                continue;
            }
            send_inventory_delta_batch(session, container_id, deltas);
        }
    };
    if (config_.mode == KernelMode_ListenServer) {
        flush_session(&local_listen_session_);
    }
    for (PeerSession& session : peer_sessions_) flush_session(&session);
}

void KernelEngine::request_inventory_snapshot(
    KernelInventoryContainerId container_id,
    std::uint64_t client_revision) {
    if (config_.mode != KernelMode_Client || transport_ == nullptr ||
        container_id == 0u ||
        !client_inventory_resync_pending_.insert(container_id).second) {
        return;
    }
    const std::vector<std::uint8_t> packet =
        encode_inventory_snapshot_request_packet(
            InventorySnapshotRequestPacket{container_id, client_revision},
            next_packet_sequence_++);
    if (!packet.empty() && transport_->Send(
            kServerPeerId,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
    } else {
        client_inventory_resync_pending_.erase(container_id);
    }
}

void KernelEngine::broadcast_reliable_event(const KernelEvent& event) {
    for (const PeerSession& session : peer_sessions_) {
        if (!session.welcomed) {
            continue;
        }
        send_reliable_event(session.peer, event);
    }
}

void KernelEngine::record_received_packet_sequence(
    const TransportEvent& transport_event) {
    if (!network_stats_enabled()) {
        return;
    }
    PacketHeader header;
    if (!decode_packet_header(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &header)) {
        return;
    }

    ReceiveSequenceState& sequence_state =
        received_sequences_by_peer_[transport_event.peer];
    if (sequence_state.received_count == 0) {
        sequence_state.last_sequence = header.sequence;
        sequence_state.received_count = 1;
        ++received_packet_count_;
        network_stats_.loss_ratio = 0.0f;
        return;
    }

    const std::uint32_t sequence_delta =
        header.sequence - sequence_state.last_sequence;
    constexpr std::uint32_t kForwardSequenceWindow = UINT32_C(0x80000000);
    if (sequence_delta > 0 && sequence_delta < kForwardSequenceWindow) {
        lost_packet_count_ += static_cast<std::uint64_t>(sequence_delta - 1);
        sequence_state.last_sequence = header.sequence;
        ++sequence_state.received_count;
        ++received_packet_count_;
    }

    const std::uint64_t expected_packet_count =
        received_packet_count_ + lost_packet_count_;
    network_stats_.loss_ratio =
        expected_packet_count == 0
            ? 0.0f
            : static_cast<float>(
                  static_cast<double>(lost_packet_count_) /
                  static_cast<double>(expected_packet_count));
}

void KernelEngine::record_sent_packet(
    std::uint32_t packet_size,
    SendMode mode,
    ChannelId channel) {
    if (!network_stats_enabled()) {
        return;
    }
    const auto cost_start = std::chrono::steady_clock::now();
    ++network_stats_.packet_count_sent;
    network_stats_.max_packet_size =
        std::max(network_stats_.max_packet_size, packet_size);
    if (mode == SendMode::kReliable) {
        network_stats_.reliable_bytes_sent += packet_size;
    } else {
        network_stats_.unreliable_bytes_sent += packet_size;
    }
    if (channel == ChannelId::kSnapshot) {
        network_stats_.snapshot_bytes_sent += packet_size;
    } else if (channel == ChannelId::kReliableEvent) {
        network_stats_.event_bytes_sent += packet_size;
    } else if (channel == ChannelId::kInput) {
        network_stats_.input_bytes_sent += packet_size;
    } else if (channel == ChannelId::kPresentation) {
        network_stats_.presentation_bytes_sent += packet_size;
    } else if (channel == ChannelId::kSession) {
        network_stats_.session_bytes_sent += packet_size;
    }
    if ((channel == ChannelId::kSnapshot ||
         channel == ChannelId::kReliableEvent) &&
        packet_size > kSnapshotSendBudgetBytes) {
        spdlog::warn(
            "[NetworkExample] large sync packet size={} warning_threshold={} "
            "send_mode={} channel={}",
            packet_size,
            kSnapshotSendBudgetBytes,
            send_mode_name(mode),
            channel_name(channel));
    }
    if (detailed_network_stats_enabled()) {
        network_stats_.packet_serialization_cost_us +=
            std::max<std::uint64_t>(1, elapsed_cost_us(cost_start));
    }
}

void KernelEngine::record_packet_deserialization_cost(std::uint64_t cost_us) {
    if (!detailed_network_stats_enabled()) {
        return;
    }
    network_stats_.packet_deserialization_cost_us +=
        std::max<std::uint64_t>(1, cost_us);
}

void KernelEngine::queue_hit_debug_records(
    const std::vector<ConfirmedDamage>& ready_damage) {
    for (const ConfirmedDamage& damage : ready_damage) {
        KernelDebugInfo debug_info{};
        debug_info.struct_size = sizeof(KernelDebugInfo);
        debug_info.tick = damage.server_tick;
        debug_info.record_type = KernelDebugRecordType_Hit;
        debug_info.data.hit.source_net_id = damage.source_net_id;
        debug_info.data.hit.target_net_id = damage.target_net_id;
        debug_info.data.hit.weapon_id = damage.source_code;
        debug_info.data.hit.position = to_kernel_vec3(damage.hit_position);
        debug_records_.push_back(debug_info);
    }
}

KernelEngine::PeerSession* KernelEngine::result_session_for_peer(PeerId peer) {
    if (config_.mode == KernelMode_ListenServer &&
        local_listen_session_.welcomed && local_listen_session_.peer == peer) {
        return &local_listen_session_;
    }
    return find_session(peer);
}

void KernelEngine::queue_local_action_result(
    PeerSession* session,
    const KernelLocalActionResult& result) {
    if (session == nullptr || result.action_instance_id == 0u) {
        return;
    }
    const bool is_new = session->recent_action_results.find(
        result.action_instance_id) == session->recent_action_results.end();
    session->recent_action_results[result.action_instance_id] = result;
    if (is_new) {
        session->recent_action_result_order.push_back(result.action_instance_id);
        if (session->recent_action_result_order.size() >
            kRecentActionResultCapacity) {
            const std::uint32_t expired =
                session->recent_action_result_order.front();
            session->recent_action_result_order.erase(
                session->recent_action_result_order.begin());
            session->recent_action_results.erase(expired);
        }
    }
    const auto duplicate = std::find_if(
        session->pending_action_results.begin(),
        session->pending_action_results.end(),
        [&result](const KernelLocalActionResult& pending) {
            return pending.action_instance_id == result.action_instance_id &&
                   pending.confirmed_commit_count ==
                       result.confirmed_commit_count &&
                   pending.result == result.result &&
                   pending.authoritative_tick == result.authoritative_tick;
        });
    if (duplicate == session->pending_action_results.end()) {
        session->pending_action_results.push_back(result);
        if (network_stats_enabled()) {
            ++network_stats_.local_action_results_generated;
            if (result.result == KernelLocalActionResultType_Accepted) {
                ++network_stats_.local_action_results_accepted;
            } else if (result.result == KernelLocalActionResultType_Corrected) {
                ++network_stats_.local_action_results_corrected;
            } else {
                ++network_stats_.local_action_results_rejected;
            }
        }
    } else if (network_stats_enabled()) {
        ++network_stats_.local_action_result_server_duplicates_suppressed;
    }
}

void KernelEngine::prepare_server_action_intent(
    PeerSession* session,
    KernelPlayerInput* input) {
    if (session == nullptr || input == nullptr ||
        input->action_intent.action_instance_id == 0u) {
        if (session != nullptr && input != nullptr &&
            input->action_intent.action_instance_id == 0u &&
            (input->action_intent.binding_id != 0u ||
             input->action_intent.flags != 0u ||
             input->action_intent.reserved != 0u) &&
            network_stats_enabled()) {
            ++network_stats_.zero_action_instance_attempts;
        }
        return;
    }
    const KernelActionIntent intent = input->action_intent;
    auto reject = [this, session, input, intent](
                      KernelLocalActionResultReason reason) {
        queue_local_action_result(
            session,
            KernelLocalActionResult{
                intent.action_instance_id,
                0u,
                KernelLocalActionResultType_Rejected,
                static_cast<std::uint8_t>(reason),
                tick_loop_.current_tick(),
            });
        input->action_intent = KernelActionIntent{};
    };
    if (intent.flags != 0u || intent.reserved != 0u ||
        (intent.binding_id != KernelActionBinding_PrimaryFire &&
         intent.binding_id != KernelActionBinding_Reload)) {
        reject(KernelLocalActionResultReason_InvalidActionId);
        return;
    }
    const auto cached = session->recent_action_results.find(
        intent.action_instance_id);
    if (cached != session->recent_action_results.end()) {
        if (network_stats_enabled()) {
            ++network_stats_.local_action_result_server_duplicates_suppressed;
        }
        queue_local_action_result(session, cached->second);
        input->action_intent = KernelActionIntent{};
        return;
    }
    if (session->active_action_instance_id == intent.action_instance_id) {
        if (network_stats_enabled()) {
            ++network_stats_.local_action_result_server_duplicates_suppressed;
        }
        input->action_intent = KernelActionIntent{};
        return;
    }
    if (session->action_instance_high_water != 0u &&
        static_cast<std::int32_t>(
            intent.action_instance_id - session->action_instance_high_water) <= 0) {
        if (network_stats_enabled()) {
            ++network_stats_.action_instance_collisions;
        }
        reject(KernelLocalActionResultReason_InvalidActionId);
        return;
    }
    session->action_instance_high_water = intent.action_instance_id;
    if (session->active_action_instance_id != 0u) {
        reject(KernelLocalActionResultReason_Busy);
        return;
    }

    const auto actor = world_.find_entity(session->player);
    if (!actor.has_value() ||
        !world_.registry().all_of<WeaponState, WeaponTuning>(*actor)) {
        reject(KernelLocalActionResultReason_MissingActor);
        return;
    }
    if (world_.registry().all_of<Health>(*actor) &&
        world_.registry().get<Health>(*actor).hp == 0u) {
        reject(KernelLocalActionResultReason_Dead);
        return;
    }
    WeaponState& weapon = world_.registry().get<WeaponState>(*actor);
    const WeaponTuning& tuning = world_.registry().get<WeaponTuning>(*actor);
    const std::size_t weapon_id = input->selected_weapon;
    const std::size_t weapon_slot =
        find_weapon_slot(weapon, input->selected_weapon);
    if (weapon_slot >= weapon.weapon_slot_count ||
        !tuning.configured[weapon_id]) {
        reject(KernelLocalActionResultReason_MissingTemplate);
        return;
    }
    const WeaponMechanicsDefinition& definition = tuning.definitions[weapon_id];
    const std::uint32_t action_template_id =
        intent.binding_id == KernelActionBinding_PrimaryFire
            ? definition.fire_action_template_id
            : definition.reload_action_template_id;
    const RuntimeActionTemplate* action_template =
        world_.find_action_template(action_template_id);
    if (action_template == nullptr) {
        reject(KernelLocalActionResultReason_MissingTemplate);
        return;
    }
    if (intent.binding_id == KernelActionBinding_PrimaryFire) {
        if (weapon.is_reloading) {
            reject(KernelLocalActionResultReason_Reloading);
            return;
        }
        if (weapon.ammo[weapon_slot] < action_template->ammo_cost_per_commit) {
            reject(KernelLocalActionResultReason_NoAmmo);
            return;
        }
        if (projected_primary_commit_is_blocked(
                tick_loop_.current_tick(),
                action_template->commit_offset_ticks,
                weapon.next_primary_commit_tick[weapon_slot])) {
            reject(KernelLocalActionResultReason_Cooldown);
            return;
        }
    } else if (
        weapon.ammo[weapon_slot] >= definition.magazine_size ||
        weapon.reserve_magazines[weapon_slot] == 0u) {
        reject(KernelLocalActionResultReason_EffectFailed);
        return;
    }
    session->active_action_instance_id = intent.action_instance_id;
    session->active_action_template_id = action_template_id;
    session->active_action_binding_id = intent.binding_id;
    session->active_action_commit_count = 0u;
}

void KernelEngine::flush_local_action_results(PeerSession* session) {
    if (session == nullptr || session->pending_action_results.empty()) {
        return;
    }
    const std::size_t max_results_per_packet = std::max<std::size_t>(
        1u,
        (config_.network_stats.action_packet_budget_bytes -
         std::min(
             static_cast<std::size_t>(
                 config_.network_stats.action_packet_budget_bytes),
             kActionBatchFixedBytes)) /
            kLocalActionResultRecordBytes);
    std::size_t offset = 0;
    while (offset < session->pending_action_results.size()) {
        const std::size_t end = std::min(
            session->pending_action_results.size(),
            offset + max_results_per_packet);
        LocalActionResultBatchPacket batch{};
        batch.server_tick = tick_loop_.current_tick();
        batch.records.assign(
            session->pending_action_results.begin() + offset,
            session->pending_action_results.begin() + end);
        const std::vector<std::uint8_t> packet =
            encode_local_action_result_batch_packet(
                batch,
                next_packet_sequence_++);
        const bool local_listen =
            config_.mode == KernelMode_ListenServer &&
            session == &local_listen_session_ &&
            listen_server_transport_ != nullptr;
        const bool sent = local_listen
            ? listen_server_transport_->Send(
                  session->peer,
                  packet.data(),
                  static_cast<std::uint32_t>(packet.size()),
                  SendMode::kReliable,
                  ChannelId::kReliableEvent)
            : transport_->Send(
                  session->peer,
                  packet.data(),
                  static_cast<std::uint32_t>(packet.size()),
                  SendMode::kReliable,
                  ChannelId::kReliableEvent);
        if (!sent) {
            push_event(KernelEventType_Error, session->player, session->peer, 29);
            return;
        }
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent);
        if (network_stats_enabled()) {
            const std::uint32_t record_count =
                static_cast<std::uint32_t>(end - offset);
            network_stats_.local_action_results_sent += record_count;
            network_stats_.local_action_result_bytes_sent += packet.size();
            ++network_stats_.local_action_result_batch_count;
            network_stats_.local_action_result_batch_record_count += record_count;
            network_stats_.max_local_action_result_batch_size = std::max(
                network_stats_.max_local_action_result_batch_size,
                record_count);
        }
        offset = end;
    }
    session->pending_action_results.clear();
}

void KernelEngine::finalize_server_action_outcomes(
    const std::vector<ActionOutcome>& outcomes) {
    if (!is_server_mode(config_.mode)) {
        return;
    }
    for (const ActionOutcome& outcome : outcomes) {
        PeerSession* session = result_session_for_peer(outcome.owner_peer);
        if (outcome.type == ActionOutcomeType::Admitted) {
            continue;
        }
        if (outcome.type == ActionOutcomeType::Completed) {
            if (session != nullptr &&
                session->active_action_instance_id == outcome.action_instance_id) {
                session->active_action_instance_id = 0u;
                session->active_action_template_id = 0u;
                session->active_action_binding_id = 0u;
                session->active_action_commit_count = 0u;
            }
            continue;
        }
        std::uint8_t result_type = KernelLocalActionResultType_Rejected;
        if (outcome.type == ActionOutcomeType::Committed) {
            result_type = KernelLocalActionResultType_Accepted;
        } else if (outcome.type == ActionOutcomeType::Corrected) {
            result_type = KernelLocalActionResultType_Corrected;
        }
        queue_local_action_result(
            session,
            KernelLocalActionResult{
                outcome.action_instance_id,
                outcome.confirmed_commit_count,
                result_type,
                static_cast<std::uint8_t>(outcome.reason),
                outcome.authoritative_tick,
            });
        if (session != nullptr && outcome.type == ActionOutcomeType::Committed) {
            session->active_action_commit_count =
                outcome.confirmed_commit_count;
        }
        if (outcome.type == ActionOutcomeType::Corrected && session != nullptr &&
            session->active_action_instance_id == outcome.action_instance_id) {
            session->active_action_instance_id = 0u;
            session->active_action_template_id = 0u;
            session->active_action_binding_id = 0u;
            session->active_action_commit_count = 0u;
        }
        if (outcome.type != ActionOutcomeType::Committed) {
            continue;
        }
        const std::uint8_t event_type =
            outcome.binding_id == KernelActionBinding_PrimaryFire
                ? KernelRemoteActionPresentationEventType_FireCommit
                : KernelRemoteActionPresentationEventType_ReloadCommit;
        KernelRemoteActionPresentationEvent presentation{
            outcome.actor_net_id,
            outcome.action_template_id,
            outcome.action_instance_id,
            outcome.confirmed_commit_count,
            1u,
            event_type,
            0u,
            0u,
        };
        if (!pending_server_remote_presentations_.empty()) {
            KernelRemoteActionPresentationEvent& previous =
                pending_server_remote_presentations_.back();
            if (previous.actor_net_id == presentation.actor_net_id &&
                previous.action_template_id == presentation.action_template_id &&
                previous.action_instance_id == presentation.action_instance_id &&
                previous.event_type == presentation.event_type &&
                previous.first_commit_index + previous.commit_count ==
                    presentation.first_commit_index &&
                previous.commit_count < UINT16_MAX) {
                ++previous.commit_count;
                continue;
            }
        }
        queue_server_remote_presentation(presentation);
    }
}

void KernelEngine::queue_server_remote_presentation(
    const KernelRemoteActionPresentationEvent& event) {
    auto priority = [](std::uint8_t event_type) {
        switch (event_type) {
            case KernelRemoteActionPresentationEventType_DeathTrigger:
                return 3;
            case KernelRemoteActionPresentationEventType_HitReaction:
                return 2;
            case KernelRemoteActionPresentationEventType_ReloadCommit:
            case KernelRemoteActionPresentationEventType_CastingCommit:
                return 1;
            default:
                return 0;
        }
    };
    if (network_stats_enabled()) {
        network_stats_.remote_presentation_records_generated +=
            event.commit_count;
    }
    if (pending_server_remote_presentations_.size() <
        kRemotePresentationDedupCapacity) {
        pending_server_remote_presentations_.push_back(event);
        return;
    }
    const auto lowest = std::min_element(
        pending_server_remote_presentations_.begin(),
        pending_server_remote_presentations_.end(),
        [&priority](
            const KernelRemoteActionPresentationEvent& lhs,
            const KernelRemoteActionPresentationEvent& rhs) {
            return priority(lhs.event_type) < priority(rhs.event_type);
        });
    if (lowest != pending_server_remote_presentations_.end() &&
        priority(event.event_type) > priority(lowest->event_type)) {
        if (network_stats_enabled()) {
            network_stats_.remote_presentation_budget_dropped +=
                lowest->commit_count;
        }
        *lowest = event;
    } else if (network_stats_enabled()) {
        network_stats_.remote_presentation_budget_dropped += event.commit_count;
    }
}

void KernelEngine::queue_status_effect_presentation(
    NetId target,
    std::uint32_t status_effect_id,
    std::uint32_t status_instance_id,
    std::uint8_t event_type,
    std::uint16_t stack_count) {
    if (!is_server_mode(config_.mode) || target == 0u ||
        status_effect_id == 0u || status_instance_id == 0u ||
        stack_count == 0u || stack_count > 32u ||
        (event_type != KernelRemoteActionPresentationEventType_StatusApplied &&
         event_type != KernelRemoteActionPresentationEventType_StatusRemoved &&
         event_type != KernelRemoteActionPresentationEventType_StatusUpdated)) {
        return;
    }
    queue_server_remote_presentation(KernelRemoteActionPresentationEvent{
        target,
        0u,
        0u,
        1u,
        1u,
        event_type,
        0u,
        0u,
        status_effect_id,
        status_instance_id,
        0u,
        0u,
        stack_count,
        0u,
    });
}

void KernelEngine::send_status_effect_state(PeerSession* session, NetId target) {
    if (session == nullptr || !session->welcomed || session->player != target) {
        return;
    }
    const std::optional<entt::entity> entity = world_.find_entity(target);
    if (!entity.has_value()) {
        return;
    }
    const StatusEffectState* state =
        world_.registry().try_get<StatusEffectState>(*entity);
    if (state == nullptr || state->revision == 0u ||
        state->active.size() > kMaxActiveStatusEffects) {
        return;
    }
    StatusEffectStatePacket packet;
    packet.server_tick = tick_loop_.current_tick();
    packet.target_net_id = target;
    packet.revision = state->revision;
    std::vector<const ActiveStatusEffect*> sorted;
    sorted.reserve(state->active.size());
    for (const ActiveStatusEffect& active : state->active) {
        sorted.push_back(&active);
    }
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const ActiveStatusEffect* lhs, const ActiveStatusEffect* rhs) {
            return lhs->instance_id < rhs->instance_id;
        });
    for (const ActiveStatusEffect* active : sorted) {
        packet.records.push_back(StatusEffectStateRecord{
            active->status_effect_id,
            active->instance_id,
            active->source,
            active->applied_tick,
            active->expire_tick,
            active->stack_count,
        });
    }
    if (config_.mode == KernelMode_ListenServer &&
        session == &local_listen_session_) {
        handle_client_status_effect_state(packet);
        return;
    }
    const std::vector<std::uint8_t> bytes =
        encode_status_effect_state_packet(packet, next_packet_sequence_++);
    if (bytes.empty() ||
        !transport_->Send(
            session->peer,
            bytes.data(),
            static_cast<std::uint32_t>(bytes.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, target, session->peer, 35);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(bytes.size()),
        SendMode::kReliable,
        ChannelId::kReliableEvent);
}

void KernelEngine::publish_status_effect_state(NetId target) {
    if (!is_server_mode(config_.mode) || target == 0u) {
        return;
    }
    if (config_.mode == KernelMode_ListenServer &&
        local_listen_session_.welcomed &&
        local_listen_session_.player == target) {
        send_status_effect_state(&local_listen_session_, target);
    }
    for (PeerSession& session : peer_sessions_) {
        if (session.welcomed && session.player == target) {
            send_status_effect_state(&session, target);
        }
    }
}

void KernelEngine::queue_remote_presentation_from_events(
    std::size_t first_event,
    std::size_t last_event,
    const std::unordered_set<NetId>& actors_before_tick) {
    if (!is_server_mode(config_.mode)) {
        return;
    }
    auto allocate_id = [this]() {
        const std::uint32_t id = next_server_presentation_instance_id_++;
        if (next_server_presentation_instance_id_ == 0u) {
            next_server_presentation_instance_id_ = 1u;
        }
        return id == 0u ? 1u : id;
    };
    const std::size_t capped_last = std::min(last_event, events_.size());
    for (std::size_t index = first_event; index < capped_last; ++index) {
        const KernelEvent& event = events_[index];
        std::uint8_t event_type = UINT8_MAX;
        if (event.type == KernelEventType_DamageApplied &&
            actors_before_tick.find(event.net_id) != actors_before_tick.end()) {
            event_type = KernelRemoteActionPresentationEventType_HitReaction;
        } else if (
            event.type == KernelEventType_EntityDestroyed &&
            actors_before_tick.find(event.net_id) != actors_before_tick.end()) {
            event_type = KernelRemoteActionPresentationEventType_DeathTrigger;
        }
        if (event_type == UINT8_MAX) {
            continue;
        }
        queue_server_remote_presentation(KernelRemoteActionPresentationEvent{
            event.net_id,
            0u,
            allocate_id(),
            1u,
            1u,
            event_type,
            0u,
            0u,
        });
    }
}

void KernelEngine::flush_remote_action_presentation(
    PeerSession* session,
    const std::vector<KernelRemoteActionPresentationEvent>& events) {
    if (session == nullptr || events.empty()) {
        return;
    }
    std::vector<KernelRemoteActionPresentationEvent> relevant;
    relevant.reserve(events.size());
    for (const KernelRemoteActionPresentationEvent& event : events) {
        const bool is_local_target = event.actor_net_id == session->player;
        const bool target_relevant =
            session->relevant_entities.find(event.actor_net_id) !=
            session->relevant_entities.end();
        if (is_local_target || !target_relevant) {
            if (network_stats_enabled()) {
                network_stats_.remote_presentation_relevance_filtered +=
                    event.commit_count;
            }
            continue;
        }
        relevant.push_back(event);
    }
    auto priority = [](std::uint8_t event_type) {
        switch (event_type) {
            case KernelRemoteActionPresentationEventType_DeathTrigger:
                return 3;
            case KernelRemoteActionPresentationEventType_HitReaction:
                return 2;
            case KernelRemoteActionPresentationEventType_ReloadCommit:
            case KernelRemoteActionPresentationEventType_CastingCommit:
                return 1;
            default:
                return 0;
        }
    };
    std::stable_sort(
        relevant.begin(),
        relevant.end(),
        [&priority](
            const KernelRemoteActionPresentationEvent& lhs,
            const KernelRemoteActionPresentationEvent& rhs) {
            return priority(lhs.event_type) > priority(rhs.event_type);
        });
    if (relevant.empty()) {
        return;
    }
    const std::size_t max_records_per_packet = std::max<std::size_t>(
        1u,
        (config_.network_stats.action_packet_budget_bytes -
         std::min(
             static_cast<std::size_t>(
                 config_.network_stats.action_packet_budget_bytes),
             kActionBatchFixedBytes)) /
            kRemotePresentationRecordBytes);
    const std::uint64_t now_us = current_server_time_us();
    refill_byte_token_bucket(
        &session->remote_presentation_budget,
        config_.network_stats
            .remote_presentation_client_budget_bytes_per_second,
        now_us);
    refill_byte_token_bucket(
        &server_remote_presentation_budget_,
        config_.network_stats
            .remote_presentation_server_budget_bytes_per_second,
        now_us);
    const std::uint64_t available_bytes = std::min(
        session->remote_presentation_budget.tokens,
        server_remote_presentation_budget_.tokens);
    const std::size_t max_budget_records =
        available_bytes <= kActionBatchFixedBytes
            ? 0u
            : static_cast<std::size_t>(
                  (available_bytes - kActionBatchFixedBytes) /
                  kRemotePresentationRecordBytes);
    const std::size_t delivered_records = std::min(
        relevant.size(),
        std::min(max_records_per_packet, max_budget_records));
    if (network_stats_enabled() && delivered_records < relevant.size()) {
        for (std::size_t index = delivered_records; index < relevant.size(); ++index) {
            network_stats_.remote_presentation_budget_dropped +=
                relevant[index].commit_count;
        }
    }
    relevant.resize(delivered_records);
    if (relevant.empty()) {
        return;
    }
    RemoteActionPresentationBatchPacket batch{};
    batch.server_tick = tick_loop_.current_tick();
    batch.records = std::move(relevant);
    const std::vector<std::uint8_t> packet =
        encode_remote_action_presentation_batch_packet(
            batch,
            next_packet_sequence_++);
    const bool local_listen =
        config_.mode == KernelMode_ListenServer &&
        session == &local_listen_session_ &&
        listen_server_transport_ != nullptr;
    const bool sent = local_listen
        ? listen_server_transport_->Send(
              session->peer,
              packet.data(),
              static_cast<std::uint32_t>(packet.size()),
              SendMode::kUnreliable,
              ChannelId::kPresentation)
        : transport_->Send(
              session->peer,
              packet.data(),
              static_cast<std::uint32_t>(packet.size()),
              SendMode::kUnreliable,
              ChannelId::kPresentation);
    if (!sent) {
        push_event(KernelEventType_Error, session->player, session->peer, 30);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kUnreliable,
        ChannelId::kPresentation);
    session->remote_presentation_budget.tokens -= packet.size();
    server_remote_presentation_budget_.tokens -= packet.size();
    if (network_stats_enabled()) {
        std::uint32_t commits = 0u;
        for (const KernelRemoteActionPresentationEvent& event : batch.records) {
            commits += event.commit_count;
        }
        network_stats_.remote_presentation_records_sent += commits;
        network_stats_.remote_action_presentation_bytes_sent += packet.size();
        ++network_stats_.remote_presentation_batch_count;
        network_stats_.remote_presentation_batch_record_count +=
            batch.records.size();
        network_stats_.max_remote_presentation_batch_size = std::max(
            network_stats_.max_remote_presentation_batch_size,
            static_cast<std::uint32_t>(batch.records.size()));
    }
}

void KernelEngine::broadcast_combat_events(
    std::size_t first_event,
    std::size_t last_event) {
    if (!is_server_mode(config_.mode) || transport_ == nullptr) {
        return;
    }
    const std::size_t capped_last = std::min(last_event, events_.size());
    for (std::size_t index = first_event; index < capped_last; ++index) {
        const KernelEvent& event = events_[index];
        if (!is_authoritative_combat_event(event.type)) {
            continue;
        }
        broadcast_reliable_event(event);
    }
}

void KernelEngine::handle_server_handshake(const TransportEvent& transport_event) {
    HandshakePacket handshake;
    const auto decode_start = std::chrono::steady_clock::now();
    if (!decode_handshake_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &handshake)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        push_event(KernelEventType_Error, 0, transport_event.peer, 9);
        return;
    }
    record_packet_deserialization_cost(elapsed_cost_us(decode_start));

    const std::uint32_t reject_reason =
        handshake_reject_reason(handshake, catalog_version_, catalog_hash_);
    if (reject_reason != 0) {
        const KernelBuildInfo build_info = current_build_info();
        spdlog::warn(
            "[NetworkExample] Connection rejected reason={} peer={} "
            "local_protocol_version={} remote_protocol_version={} "
            "local_snapshot_schema_version={} remote_snapshot_schema_version={} "
            "local_packet_schema_version={} remote_packet_schema_version={} "
            "local_catalog_version={} remote_catalog_version={} "
            "local_catalog_hash={} remote_catalog_hash={} "
            "local_module_version={} remote_module_version={} "
            "local_git_commit={} remote_git_commit={}",
            disconnect_reason_name(reject_reason),
            transport_event.peer,
            build_info.protocol_version,
            handshake.protocol_version,
            build_info.snapshot_schema_version,
            handshake.snapshot_schema_version,
            build_info.packet_schema_version,
            handshake.packet_schema_version,
            catalog_version_,
            handshake.catalog_version,
            catalog_hash_,
            handshake.catalog_hash,
            build_info.module_version,
            handshake.module_version,
            build_info.git_commit,
            handshake.git_commit);
        const DisconnectPacket disconnect{reject_reason};
        const std::vector<std::uint8_t> packet =
            encode_disconnect_packet(disconnect, next_packet_sequence_++);
        if (!transport_->Send(
                transport_event.peer,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kReliable,
                ChannelId::kSession)) {
            push_event(KernelEventType_Error, 0, transport_event.peer, 12);
        } else {
            record_sent_packet(
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kReliable,
                ChannelId::kSession);
        }
        return;
    }

    PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr) {
        const NetId player = world_.spawn_player(
            transport_event.peer,
            glm::vec3{0.0f, 0.0f, 0.0f});
        register_actor_for_first_physics(player);
        peer_sessions_.push_back(PeerSession{transport_event.peer, player, 0, false});
        session = &peer_sessions_.back();
        push_event(KernelEventType_PlayerJoined, player, transport_event.peer);
        push_event(
            KernelEventType_EntitySpawned,
            player,
            transport_event.peer,
            static_cast<std::uint32_t>(EntityType::kActor));
    }

    const WelcomePacket welcome{
        transport_event.peer,
        session->player,
        tick_loop_.current_tick(),
        config_.tick.server_tick_rate,
        config_.tick.snapshot_rate,
        catalog_version_,
        catalog_hash_,
        session_rules_.actor_blocking_mode,
    };
    const std::vector<std::uint8_t> packet =
        encode_welcome_packet(welcome, next_packet_sequence_++);
    if (!transport_->Send(
            transport_event.peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 12);
        return;
    }
    record_sent_packet(
        static_cast<std::uint32_t>(packet.size()),
        SendMode::kReliable,
        ChannelId::kSession);

    session->welcomed = true;
    send_clock_sync_ping(session, current_server_time_us());
    publish_snapshot();
}

void KernelEngine::handle_server_gameplay_catalog_manifest_request(
    const TransportEvent& transport_event) {
    GameplayCatalogManifestRequestPacket request;
    if (!decode_gameplay_catalog_manifest_request_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &request)) {
        return;
    }

    GameplayCatalogSyncErrorPacket error;
    bool has_error = false;
    if (request.protocol_version != kProtocolVersion ||
        request.snapshot_schema_version != kSnapshotSchemaVersion ||
        request.packet_schema_version != kPacketSchemaVersion) {
        error.error_code = GameplayCatalogSyncErrorCode::kVersionMismatch;
        has_error = true;
    } else if (gameplay_catalog_sync_bundle_.empty()) {
        error.error_code = GameplayCatalogSyncErrorCode::kBundleUnavailable;
        has_error = true;
    }

    std::vector<std::uint8_t> packet;
    if (has_error) {
        packet = encode_gameplay_catalog_sync_error_packet(
            error,
            next_packet_sequence_++);
    } else {
        GameplayCatalogManifestPacket manifest;
        manifest.catalog_version = gameplay_catalog_manifest_.catalog_version;
        manifest.catalog_hash = gameplay_catalog_manifest_.catalog_hash;
        manifest.bundle_size = gameplay_catalog_manifest_.bundle_size;
        std::copy(
            std::begin(gameplay_catalog_manifest_.bundle_sha256),
            std::end(gameplay_catalog_manifest_.bundle_sha256),
            manifest.bundle_sha256.begin());
        std::memcpy(
            manifest.entry_path,
            gameplay_catalog_manifest_.entry_path,
            sizeof(manifest.entry_path));
        std::memcpy(
            manifest.content_namespace,
            gameplay_catalog_manifest_.content_namespace,
            sizeof(manifest.content_namespace));
        packet = encode_gameplay_catalog_manifest_packet(
            manifest,
            next_packet_sequence_++);
    }
    if (!transport_->Send(
            transport_event.peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 28);
    } else {
        record_sent_packet(
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession);
    }
}

void KernelEngine::handle_server_gameplay_catalog_bundle_request(
    const TransportEvent& transport_event) {
    GameplayCatalogBundleRequestPacket request;
    if (!decode_gameplay_catalog_bundle_request_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &request)) {
        return;
    }
    if (gameplay_catalog_sync_bundle_.empty() ||
        !std::equal(
            request.bundle_sha256.begin(),
            request.bundle_sha256.end(),
            std::begin(gameplay_catalog_manifest_.bundle_sha256))) {
        const GameplayCatalogSyncErrorPacket error{
            gameplay_catalog_sync_bundle_.empty()
                ? GameplayCatalogSyncErrorCode::kBundleUnavailable
                : GameplayCatalogSyncErrorCode::kInvalidRequest};
        const std::vector<std::uint8_t> packet =
            encode_gameplay_catalog_sync_error_packet(
                error,
                next_packet_sequence_++);
        transport_->Send(
            transport_event.peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kSession);
        return;
    }
    gameplay_catalog_transfers_[transport_event.peer] = GameplayCatalogTransfer{};
}

void KernelEngine::handle_client_gameplay_catalog_manifest(
    const TransportEvent& transport_event) {
    GameplayCatalogManifestPacket manifest;
    if (!decode_gameplay_catalog_manifest_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &manifest)) {
        return;
    }
    if (gameplay_catalog_sync_state_ !=
            KernelGameplayCatalogSyncState_FetchingManifest ||
        manifest.bundle_size == 0 ||
        manifest.bundle_size > gameplay_catalog_sync_max_bundle_size_ ||
        manifest.entry_path[0] == '\0' ||
        !is_valid_content_namespace(manifest.content_namespace)) {
        fail_gameplay_catalog_sync(
            manifest.bundle_size > gameplay_catalog_sync_max_bundle_size_
                ? KernelGameplayCatalogSyncError_BundleTooLarge
                : KernelGameplayCatalogSyncError_InvalidManifest);
        return;
    }
    gameplay_catalog_manifest_ = KernelGameplayCatalogManifest{};
    gameplay_catalog_manifest_.struct_size =
        sizeof(KernelGameplayCatalogManifest);
    gameplay_catalog_manifest_.catalog_version = manifest.catalog_version;
    gameplay_catalog_manifest_.catalog_hash = manifest.catalog_hash;
    gameplay_catalog_manifest_.bundle_size = manifest.bundle_size;
    std::copy(
        manifest.bundle_sha256.begin(),
        manifest.bundle_sha256.end(),
        std::begin(gameplay_catalog_manifest_.bundle_sha256));
    std::memcpy(
        gameplay_catalog_manifest_.entry_path,
        manifest.entry_path,
        sizeof(gameplay_catalog_manifest_.entry_path));
    std::memcpy(
        gameplay_catalog_manifest_.content_namespace,
        manifest.content_namespace,
        sizeof(gameplay_catalog_manifest_.content_namespace));
    gameplay_catalog_sync_state_ =
        KernelGameplayCatalogSyncState_ManifestReady;
    gameplay_catalog_sync_elapsed_us_ = 0;
}

void KernelEngine::handle_client_gameplay_catalog_bundle_chunk(
    const TransportEvent& transport_event) {
    GameplayCatalogBundleChunkPacket chunk;
    if (!decode_gameplay_catalog_bundle_chunk_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &chunk)) {
        return;
    }
    if (gameplay_catalog_sync_state_ !=
            KernelGameplayCatalogSyncState_Downloading ||
        chunk.total_size != gameplay_catalog_manifest_.bundle_size ||
        chunk.offset != downloaded_gameplay_catalog_bundle_.size() ||
        chunk.bytes.empty() ||
        chunk.offset + chunk.bytes.size() > chunk.total_size ||
        !std::equal(
            chunk.bundle_sha256.begin(),
            chunk.bundle_sha256.end(),
            std::begin(gameplay_catalog_manifest_.bundle_sha256))) {
        fail_gameplay_catalog_sync(KernelGameplayCatalogSyncError_InvalidBundle);
        return;
    }
    downloaded_gameplay_catalog_bundle_.insert(
        downloaded_gameplay_catalog_bundle_.end(),
        chunk.bytes.begin(),
        chunk.bytes.end());
    gameplay_catalog_sync_elapsed_us_ = 0;
    if (downloaded_gameplay_catalog_bundle_.size() ==
        gameplay_catalog_manifest_.bundle_size) {
        const std::array<std::uint8_t, 32> digest = compute_sha256(
            downloaded_gameplay_catalog_bundle_.data(),
            downloaded_gameplay_catalog_bundle_.size());
        if (!std::equal(
                digest.begin(),
                digest.end(),
                std::begin(gameplay_catalog_manifest_.bundle_sha256))) {
            fail_gameplay_catalog_sync(
                KernelGameplayCatalogSyncError_InvalidBundle);
            return;
        }
        gameplay_catalog_sync_state_ =
            KernelGameplayCatalogSyncState_BundleReady;
    }
}

void KernelEngine::handle_client_gameplay_catalog_sync_error(
    const TransportEvent& transport_event) {
    GameplayCatalogSyncErrorPacket error;
    if (!decode_gameplay_catalog_sync_error_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &error)) {
        return;
    }
    switch (error.error_code) {
        case GameplayCatalogSyncErrorCode::kUnsupported:
            fail_gameplay_catalog_sync(
                KernelGameplayCatalogSyncError_Unsupported);
            break;
        case GameplayCatalogSyncErrorCode::kBundleUnavailable:
            fail_gameplay_catalog_sync(
                KernelGameplayCatalogSyncError_BundleUnavailable);
            break;
        case GameplayCatalogSyncErrorCode::kVersionMismatch:
            fail_gameplay_catalog_sync(
                KernelGameplayCatalogSyncError_VersionMismatch);
            break;
        case GameplayCatalogSyncErrorCode::kInvalidRequest:
            fail_gameplay_catalog_sync(
                KernelGameplayCatalogSyncError_InvalidState);
            break;
    }
}

void KernelEngine::handle_server_session_message(const TransportEvent& transport_event) {
    GameplayCatalogManifestRequestPacket manifest_request;
    if (decode_gameplay_catalog_manifest_request_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &manifest_request)) {
        handle_server_gameplay_catalog_manifest_request(transport_event);
        return;
    }
    GameplayCatalogBundleRequestPacket bundle_request;
    if (decode_gameplay_catalog_bundle_request_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &bundle_request)) {
        handle_server_gameplay_catalog_bundle_request(transport_event);
        return;
    }
    PingPongPacket ping;
    auto decode_start = std::chrono::steady_clock::now();
    if (decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &ping)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_server_ping_pong(transport_event);
        return;
    }
    record_packet_deserialization_cost(elapsed_cost_us(decode_start));

    handle_server_handshake(transport_event);
}

void KernelEngine::handle_client_session_message(const TransportEvent& transport_event) {
    GameplayCatalogManifestPacket manifest;
    if (decode_gameplay_catalog_manifest_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &manifest)) {
        handle_client_gameplay_catalog_manifest(transport_event);
        return;
    }
    GameplayCatalogBundleChunkPacket chunk;
    if (decode_gameplay_catalog_bundle_chunk_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &chunk)) {
        handle_client_gameplay_catalog_bundle_chunk(transport_event);
        return;
    }
    GameplayCatalogSyncErrorPacket sync_error;
    if (decode_gameplay_catalog_sync_error_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &sync_error)) {
        handle_client_gameplay_catalog_sync_error(transport_event);
        return;
    }
    WelcomePacket welcome;
    auto decode_start = std::chrono::steady_clock::now();
    if (decode_welcome_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &welcome)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        if (!apply_welcome(welcome)) {
            push_event(KernelEventType_Error, 0, kServerPeerId, 30);
            clear_client_session();
            prediction_failed_ = true;
            return;
        }
        push_event(KernelEventType_PlayerJoined, 0, local_client_peer_id_);
        return;
    }

    PingPongPacket ping;
    decode_start = std::chrono::steady_clock::now();
    if (decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &ping)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        handle_client_ping_pong(transport_event);
        return;
    }

    DisconnectPacket disconnect;
    decode_start = std::chrono::steady_clock::now();
    if (decode_disconnect_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &disconnect)) {
        record_packet_deserialization_cost(elapsed_cost_us(decode_start));
        clear_client_session();
        push_event(KernelEventType_Disconnected, 0, kServerPeerId, disconnect.reason_code);
        return;
    }

    record_packet_deserialization_cost(elapsed_cost_us(decode_start));
    push_event(KernelEventType_Error, 0, transport_event.peer, 13);
}

KernelEngine::PeerSession* KernelEngine::find_session(PeerId peer) {
    auto found = std::find_if(
        peer_sessions_.begin(),
        peer_sessions_.end(),
        [peer](const PeerSession& session) {
            return session.peer == peer;
        });
    if (found == peer_sessions_.end()) {
        return nullptr;
    }
    return &(*found);
}

const KernelEngine::PeerSession* KernelEngine::find_session(PeerId peer) const {
    auto found = std::find_if(
        peer_sessions_.begin(),
        peer_sessions_.end(),
        [peer](const PeerSession& session) {
            return session.peer == peer;
        });
    if (found == peer_sessions_.end()) {
        return nullptr;
    }
    return &(*found);
}

void KernelEngine::remove_session(PeerId peer) {
    world_.clear_action_graph_batches_for_peer(peer);
    peer_sessions_.erase(
        std::remove_if(
            peer_sessions_.begin(),
            peer_sessions_.end(),
            [peer](const PeerSession& session) {
                return session.peer == peer;
            }),
        peer_sessions_.end());
}

}  // namespace network_example
