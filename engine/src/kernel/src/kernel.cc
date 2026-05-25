#include "kernel/src/kernel.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "kernel/src/render_state_builder.h"
#include "protocol/public/network_packets.h"
#include "protocol/public/session_packets.h"
#include "transport/public/gns_transport.h"
#include "transport/public/loopback_transport.h"
#include "transport/public/network_simulator_transport.h"

namespace network_example {
namespace {

KernelConfig with_kernel_defaults(KernelConfig config) {
    config.tick = with_tick_defaults(config.tick);
    if (config.max_render_states == 0) {
        config.max_render_states = 256;
    }
    if (config.max_events == 0) {
        config.max_events = 256;
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

bool is_authoritative_combat_event(KernelEventType type) {
    return type == KernelEventType_FireConfirmed ||
           type == KernelEventType_HitConfirmed ||
           type == KernelEventType_DamageApplied ||
           type == KernelEventType_Explosion;
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
    return flags;
}

glm::vec3 input_move_to_world(const PlayerInput& input) {
    glm::vec3 move{input.move.x, 0.0f, input.move.y};
    const float length = glm::length(move);
    if (length > 1.0f) {
        move /= length;
    }
    return move;
}

glm::vec3 input_aim_to_world(const PlayerInput& input) {
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

void apply_input_prediction(
    EntitySnapshot* entity,
    const PlayerInput& input,
    float fixed_delta_seconds,
    float move_speed_meters_per_second) {
    if (entity == nullptr) {
        return;
    }
    entity->velocity = input_move_to_world(input) * move_speed_meters_per_second;
    entity->position += entity->velocity * fixed_delta_seconds;
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

KernelServerEntityState to_server_entity_state(
    const World& world,
    entt::entity entity) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(KernelServerEntityState);

    const NetworkIdentity& identity = world.registry().get<NetworkIdentity>(entity);
    const EntityKind& kind = world.registry().get<EntityKind>(entity);
    const Transform& transform = world.registry().get<Transform>(entity);
    state.net_id = identity.net_id;
    state.entity_type = static_cast<std::uint16_t>(kind.type);
    state.owner_peer = identity.owner_peer;
    state.position = to_kernel_vec3(transform.position);
    state.rotation = to_kernel_quat(transform.rotation);
    state.valid = true;

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
    return state;
}

std::uint32_t history_frame_count(const TickConfig& config) {
    const TickConfig tick = with_tick_defaults(config);
    return std::max(1u, (tick.server_tick_rate * tick.history_ms) / 1000u);
}

constexpr PeerId kLocalListenPeerId = 1;
constexpr PeerId kServerPeerId = 0;
constexpr std::uint32_t kClientNonce = 0x4d330001u;
constexpr std::uint32_t kMaxCompensationWindowUs = 100000u;
constexpr std::uint64_t kClockSyncIntervalUs = 1000000u;
constexpr double kClientClockOffsetSmoothingFactor = 0.25;
constexpr float kMaxHomingVisualExtrapolationSeconds = 0.2f;
constexpr float kDefaultEntityRelevanceDistanceMeters = 40.0f;
constexpr float kDefaultProjectileRelevanceDistanceMeters = 80.0f;

bool valid_weapon_id(std::uint8_t weapon_id) {
    return static_cast<std::size_t>(weapon_id) < kWeaponCount;
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
    if (damage_shape == KernelProjectileDamageShape_Explosion) {
        return ProjectileDamageShape::kExplosion;
    }
    if (damage_shape == KernelProjectileDamageShape_PiercingSegment) {
        return ProjectileDamageShape::kPiercingSegment;
    }
    return ProjectileDamageShape::kDirectHit;
}

std::uint8_t to_kernel_projectile_damage_shape(
    ProjectileDamageShape damage_shape) {
    switch (damage_shape) {
        case ProjectileDamageShape::kExplosion:
            return KernelProjectileDamageShape_Explosion;
        case ProjectileDamageShape::kPiercingSegment:
            return KernelProjectileDamageShape_PiercingSegment;
        case ProjectileDamageShape::kDirectHit:
        default:
            return KernelProjectileDamageShape_DirectHit;
    }
}

WeaponFireMode to_weapon_fire_mode(std::uint8_t fire_mode) {
    if (fire_mode == KernelWeaponFireMode_Shotgun) {
        return WeaponFireMode::kShotgun;
    }
    if (fire_mode == KernelWeaponFireMode_Projectile) {
        return WeaponFireMode::kProjectile;
    }
    if (fire_mode == KernelWeaponFireMode_AreaEffect) {
        return WeaponFireMode::kAreaEffect;
    }
    if (fire_mode == KernelWeaponFireMode_Beam) {
        return WeaponFireMode::kBeam;
    }
    return WeaponFireMode::kHitscan;
}

WeaponMechanicsDefinition to_weapon_mechanics(
    const KernelWeaponMechanicsDefinition& definition) {
    WeaponMechanicsDefinition mechanics{};
    mechanics.id = definition.weapon_id;
    mechanics.mode = to_weapon_fire_mode(definition.fire_mode);
    mechanics.magazine_size = definition.magazine_size;
    mechanics.damage = definition.damage;
    mechanics.cooldown_ticks = definition.cooldown_ticks;
    mechanics.reload_ticks = definition.reload_ticks;
    mechanics.max_range = definition.max_range;
    mechanics.pellet_count = definition.pellet_count;
    mechanics.pellet_spread = definition.pellet_spread;
    mechanics.projectile_speed = definition.projectile.speed;
    mechanics.projectile_lifetime_seconds =
        definition.projectile.lifetime_seconds;
    mechanics.explosion_radius = definition.projectile.explosion_radius;
    mechanics.projectile_motion_model =
        to_projectile_motion_model(definition.projectile.motion_model);
    mechanics.projectile_gravity = from_kernel_vec3(definition.projectile.gravity);
    mechanics.projectile_hit_response =
        to_projectile_hit_response(definition.projectile.hit_response);
    mechanics.projectile_damage_shape =
        to_projectile_damage_shape(definition.projectile.damage_shape);
    mechanics.projectile_collision_mask = definition.projectile.collision_mask;
    mechanics.projectile_max_hit_count = definition.projectile.max_hit_count;
    mechanics.area_effect_radius = definition.area_effect.radius;
    mechanics.area_effect_damage_per_interval =
        definition.area_effect.damage_per_interval;
    mechanics.area_effect_damage_interval_ticks =
        definition.area_effect.damage_interval_ticks;
    mechanics.area_effect_lifetime_ticks = definition.area_effect.lifetime_ticks;
    mechanics.area_effect_spawn_distance = definition.area_effect.spawn_distance;
    mechanics.area_effect_collision_mask = definition.area_effect.collision_mask;
    mechanics.beam_length = definition.beam.length;
    mechanics.beam_radius = definition.beam.radius;
    mechanics.beam_damage_per_second = definition.beam.damage_per_second;
    mechanics.beam_lifetime_ticks = definition.beam.lifetime_ticks;
    mechanics.beam_collision_mask = definition.beam.collision_mask;
    mechanics.homing_mode = to_homing_mode(definition.projectile.homing.homing_mode);
    mechanics.homing_sync_mode =
        to_projectile_sync_mode(definition.projectile.homing.sync_mode);
    mechanics.homing_boost_ticks = definition.projectile.homing.boost_ticks;
    mechanics.homing_lock_on_range = definition.projectile.homing.lock_on_range;
    mechanics.homing_lose_target_range = definition.projectile.homing.lose_target_range;
    mechanics.homing_lock_cone_degrees = definition.projectile.homing.lock_cone_degrees;
    mechanics.homing_max_turn_rate_degrees_per_second =
        definition.projectile.homing.max_turn_rate_degrees_per_second;
    mechanics.homing_acceleration = definition.projectile.homing.acceleration;
    mechanics.homing_max_speed = definition.projectile.homing.max_speed;
    return mechanics;
}

KernelWeaponMechanicsDefinition to_kernel_weapon_mechanics(
    const WeaponMechanicsDefinition& mechanics) {
    KernelWeaponMechanicsDefinition definition{};
    definition.struct_size = sizeof(KernelWeaponMechanicsDefinition);
    definition.weapon_id = mechanics.id;
    definition.fire_mode = static_cast<std::uint8_t>(mechanics.mode);
    definition.magazine_size = mechanics.magazine_size;
    definition.damage = mechanics.damage;
    definition.cooldown_ticks = mechanics.cooldown_ticks;
    definition.reload_ticks = mechanics.reload_ticks;
    definition.max_range = mechanics.max_range;
    definition.pellet_count = mechanics.pellet_count;
    definition.pellet_spread = mechanics.pellet_spread;
    definition.projectile.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    definition.projectile.motion_model =
        to_kernel_projectile_motion_model(mechanics.projectile_motion_model);
    definition.projectile.hit_response =
        to_kernel_projectile_hit_response(mechanics.projectile_hit_response);
    definition.projectile.damage_shape =
        to_kernel_projectile_damage_shape(mechanics.projectile_damage_shape);
    definition.projectile.speed = mechanics.projectile_speed;
    definition.projectile.lifetime_seconds =
        mechanics.projectile_lifetime_seconds;
    definition.projectile.explosion_radius = mechanics.explosion_radius;
    definition.projectile.gravity = to_kernel_vec3(mechanics.projectile_gravity);
    definition.projectile.collision_mask = mechanics.projectile_collision_mask;
    definition.projectile.max_hit_count = mechanics.projectile_max_hit_count;
    if (mechanics.projectile_motion_model == ProjectileMotionModel::kHoming) {
        definition.projectile.homing.struct_size =
            sizeof(KernelHomingMechanicsDefinition);
        definition.projectile.homing.homing_mode =
            to_kernel_homing_mode(mechanics.homing_mode);
        definition.projectile.homing.sync_mode =
            to_kernel_projectile_sync_mode(mechanics.homing_sync_mode);
        definition.projectile.homing.boost_ticks = mechanics.homing_boost_ticks;
        definition.projectile.homing.lock_on_range = mechanics.homing_lock_on_range;
        definition.projectile.homing.lose_target_range =
            mechanics.homing_lose_target_range;
        definition.projectile.homing.lock_cone_degrees =
            mechanics.homing_lock_cone_degrees;
        definition.projectile.homing.max_turn_rate_degrees_per_second =
            mechanics.homing_max_turn_rate_degrees_per_second;
        definition.projectile.homing.acceleration = mechanics.homing_acceleration;
        definition.projectile.homing.max_speed = mechanics.homing_max_speed;
    }
    definition.area_effect.struct_size =
        sizeof(KernelAreaEffectMechanicsDefinition);
    definition.area_effect.radius = mechanics.area_effect_radius;
    definition.area_effect.damage_per_interval =
        mechanics.area_effect_damage_per_interval;
    definition.area_effect.damage_interval_ticks =
        mechanics.area_effect_damage_interval_ticks;
    definition.area_effect.lifetime_ticks = mechanics.area_effect_lifetime_ticks;
    definition.area_effect.spawn_distance = mechanics.area_effect_spawn_distance;
    definition.area_effect.collision_mask = mechanics.area_effect_collision_mask;
    definition.beam.struct_size = sizeof(KernelBeamMechanicsDefinition);
    definition.beam.length = mechanics.beam_length;
    definition.beam.radius = mechanics.beam_radius;
    definition.beam.damage_per_second = mechanics.beam_damage_per_second;
    definition.beam.lifetime_ticks = mechanics.beam_lifetime_ticks;
    definition.beam.collision_mask = mechanics.beam_collision_mask;
    return definition;
}

bool validate_homing_mechanics(const KernelHomingMechanicsDefinition& homing) {
    return homing.struct_size >= sizeof(KernelHomingMechanicsDefinition) &&
           homing.homing_mode == KernelHomingMode_FireAndForget &&
           homing.sync_mode == KernelProjectileSyncMode_HybridDeterministicThenSnapshot &&
           homing.lock_on_range > 0.0f &&
           homing.lose_target_range >= homing.lock_on_range &&
           homing.lock_cone_degrees > 0.0f &&
           homing.lock_cone_degrees <= 180.0f &&
           homing.max_turn_rate_degrees_per_second > 0.0f &&
           homing.acceleration > 0.0f &&
           homing.max_speed > 0.0f;
}

bool validate_weapon_mechanics(const KernelWeaponMechanicsDefinition& definition) {
    if (definition.struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
        !valid_weapon_id(definition.weapon_id) ||
        definition.magazine_size == 0 ||
        definition.damage == 0 ||
        definition.cooldown_ticks == 0 ||
        definition.reload_ticks == 0) {
        return false;
    }
    if (definition.fire_mode > KernelWeaponFireMode_Beam) {
        return false;
    }
    if (definition.fire_mode == KernelWeaponFireMode_Projectile) {
        if (definition.projectile.struct_size <
                sizeof(KernelProjectileMechanicsDefinition) ||
            definition.projectile.motion_model > KernelProjectileMotionModel_Homing ||
            definition.projectile.hit_response > KernelProjectileHitResponse_Attach ||
            definition.projectile.damage_shape > KernelProjectileDamageShape_PiercingSegment ||
            definition.projectile.hit_response == KernelProjectileHitResponse_Bounce ||
            definition.projectile.hit_response == KernelProjectileHitResponse_Attach ||
            definition.projectile.collision_mask == 0 ||
            definition.projectile.max_hit_count == 0 ||
            definition.projectile.speed <= 0.0f ||
            definition.projectile.lifetime_seconds <= 0.0f) {
            return false;
        }
        if (definition.projectile.motion_model == KernelProjectileMotionModel_Homing) {
            return validate_homing_mechanics(definition.projectile.homing);
        }
        return definition.projectile.homing.struct_size == 0;
    }
    if (definition.fire_mode == KernelWeaponFireMode_AreaEffect) {
        return definition.area_effect.struct_size >=
                   sizeof(KernelAreaEffectMechanicsDefinition) &&
               definition.area_effect.radius > 0.0f &&
               definition.area_effect.damage_per_interval > 0 &&
               definition.area_effect.damage_interval_ticks > 0 &&
               definition.area_effect.lifetime_ticks > 0 &&
               definition.area_effect.spawn_distance >= 0.0f &&
               definition.area_effect.collision_mask != 0;
    }
    if (definition.fire_mode == KernelWeaponFireMode_Beam) {
        return definition.beam.struct_size >=
                   sizeof(KernelBeamMechanicsDefinition) &&
               definition.beam.length > 0.0f &&
               definition.beam.radius > 0.0f &&
               definition.beam.damage_per_second > 0 &&
               definition.beam.lifetime_ticks > 0 &&
               definition.beam.collision_mask != 0;
    }
    if (definition.max_range <= 0.0f) {
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
    if (!valid_weapon_id(weapon_id)) {
        return nullptr;
    }
    const std::optional<entt::entity> entity = world.find_entity(net_id);
    if (!entity.has_value() ||
        !world.registry().all_of<WeaponTuning>(*entity)) {
        return nullptr;
    }
    const WeaponTuning& tuning = world.registry().get<WeaponTuning>(*entity);
    const std::size_t index = static_cast<std::size_t>(weapon_id);
    return tuning.configured[index] ? &tuning.definitions[index] : nullptr;
}

}  // namespace

KernelEngine::KernelEngine(KernelConfig config)
    : config_(with_kernel_defaults(config)),
      tick_loop_(config_.tick),
      history_buffer_(history_frame_count(config_.tick)),
      transport_(std::make_unique<NetworkSimulatorTransport>()) {
    render_states_.reserve(config_.max_render_states);
    events_.reserve(config_.max_events);
}

bool KernelEngine::start_client(const char* address) {
    auto gns_transport = std::make_unique<GnsTransport>();
    if (!gns_transport->StartClient(address)) {
        push_event(KernelEventType_Error, 0, 0, 1);
        return false;
    }

    loopback_transport_ = nullptr;
    transport_ = std::move(gns_transport);
    reset_runtime_state(KernelMode_Client);
    return true;
}

bool KernelEngine::start_listen_server(std::uint16_t port) {
    auto loopback_transport = std::make_unique<LoopbackTransport>();
    if (!loopback_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 2);
        return false;
    }

    loopback_transport_ = loopback_transport.get();
    transport_ = std::move(loopback_transport);
    reset_runtime_state(KernelMode_ListenServer);

    const NetId player =
        world_.spawn_player(kLocalListenPeerId, glm::vec3{0.0f, 0.0f, 0.0f});
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
        static_cast<std::uint32_t>(EntityType::kPlayer));
    publish_snapshot();
    poll_client_transport();
    rebuild_render_states();
    return true;
}

bool KernelEngine::start_dedicated_server(std::uint16_t port) {
    auto gns_transport = std::make_unique<GnsTransport>();
    loopback_transport_ = nullptr;
    if (!gns_transport->StartServer(port)) {
        push_event(KernelEventType_Error, 0, 0, 3);
        return false;
    }

    transport_ = std::move(gns_transport);
    reset_runtime_state(KernelMode_DedicatedServer);
    publish_snapshot();
    rebuild_render_states();
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
    }

    poll_transport();
    const std::uint32_t ticks_to_run = tick_loop_.accumulate(delta_seconds);
    for (std::uint32_t tick = 0; tick < ticks_to_run; ++tick) {
        simulate_tick();
    }
    poll_client_transport();
    rebuild_render_states();
}

void KernelEngine::submit_input(PeerId local_player_id, const PlayerInput& input) {
    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        const PlayerInput input_to_send = prepare_client_input(input);
        predict_local_input(input_to_send);
        predict_local_projectile(input_to_send);
        rebuild_render_states();
        const std::vector<std::uint8_t> packet =
            encode_input_packet(local_player_id, input_to_send, next_packet_sequence_++);
        if (!loopback_transport_->SendClient(
                local_player_id,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kInput)) {
            push_event(KernelEventType_Error, 0, local_player_id, 4);
        }
        return;
    }

    if (config_.mode == KernelMode_Client) {
        if (!has_welcome_) {
            push_event(KernelEventType_Error, 0, 0, 8);
            return;
        }

        const PlayerInput input_to_send = prepare_client_input(input);
        predict_local_input(input_to_send);
        predict_local_projectile(input_to_send);
        rebuild_render_states();
        const std::vector<std::uint8_t> packet =
            encode_input_packet(local_client_peer_id_, input_to_send, next_packet_sequence_++);
        if (!transport_->Send(
                kServerPeerId,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kInput)) {
            push_event(KernelEventType_Error, 0, local_client_peer_id_, 4);
        }
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
    rebuild_render_states_at_time(client_render_time_us, false);
    const std::uint32_t count =
        std::min(max_states, static_cast<std::uint32_t>(render_states_.size()));
    std::memcpy(out_states, render_states_.data(), sizeof(RenderEntityState) * count);
    return count;
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

KernelLocalPlayerInfo KernelEngine::local_player_info() const {
    return KernelLocalPlayerInfo{
        local_client_peer_id_,
        local_player_net_id_,
        has_welcome_,
        running_ && has_welcome_ && local_client_peer_id_ != 0 &&
            local_player_net_id_ != 0,
    };
}

bool KernelEngine::server_create_entity(
    const KernelServerEntityCreateInfo& create_info,
    NetId* out_net_id) {
    if (!running_ || !is_server_mode(config_.mode) || out_net_id == nullptr ||
        create_info.struct_size < sizeof(KernelServerEntityCreateInfo)) {
        return false;
    }

    const EntityType type = static_cast<EntityType>(create_info.entity_type);
    NetId net_id = 0;
    if (type == EntityType::kEnemy) {
        net_id = world_.spawn_enemy(from_kernel_vec3(create_info.position));
    } else {
        return false;
    }

    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    Transform& transform = world_.registry().get<Transform>(*entity);
    transform.rotation = from_kernel_quat(create_info.rotation);
    ReplicationState& replication =
        world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = create_info.animation_state;
    replication.visual_flags = create_info.visual_flags;

    *out_net_id = net_id;
    push_event(
        KernelEventType_EntitySpawned,
        net_id,
        create_info.owner_peer,
        static_cast<std::uint32_t>(type));
    publish_snapshot();
    return true;
}

bool KernelEngine::server_destroy_entity(NetId net_id, std::uint32_t reason) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0) {
        return false;
    }
    if (!world_.destroy(net_id)) {
        return false;
    }
    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        send_entity_despawn(kLocalListenPeerId, net_id, reason);
    }
    for (PeerSession& session : peer_sessions_) {
        if (session.relevant_entities.erase(net_id) > 0) {
            send_entity_despawn(session.peer, net_id, reason);
        }
    }
    push_event(KernelEventType_EntityDestroyed, net_id, 0, reason);
    publish_snapshot();
    return true;
}

bool KernelEngine::server_set_entity_transform(
    NetId net_id,
    const KernelVec3& position,
    const KernelQuat& rotation) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Transform>(*entity)) {
        return false;
    }
    Transform& transform = world_.registry().get<Transform>(*entity);
    transform.position = from_kernel_vec3(position);
    transform.rotation = from_kernel_quat(rotation);
    return true;
}

bool KernelEngine::server_set_entity_velocity(
    NetId net_id,
    const KernelVec3& velocity) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() || !world_.registry().all_of<Velocity>(*entity)) {
        return false;
    }
    world_.registry().get<Velocity>(*entity).linear = from_kernel_vec3(velocity);
    return true;
}

bool KernelEngine::server_set_entity_state(
    NetId net_id,
    std::uint16_t animation_state,
    std::uint32_t visual_flags) {
    if (!running_ || !is_server_mode(config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    ReplicationState& replication =
        world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = animation_state;
    replication.visual_flags = visual_flags;
    return true;
}

bool KernelEngine::server_submit_entity_input(NetId net_id, const PlayerInput& input) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, Transform, WeaponState, Hitbox>(
            *entity)) {
        return false;
    }

    pending_inputs_.push_back(QueuedInput{
        0,
        input,
        tick_loop_.current_tick(),
        current_server_time_us(),
        true,
        net_id,
    });
    return true;
}

bool KernelEngine::server_set_entity_combat_state(
    NetId net_id,
    const KernelCombatStateDefinition& combat_state) {
    if (!running_ || !is_server_mode(config_.mode) || net_id == 0 ||
        combat_state.struct_size < sizeof(KernelCombatStateDefinition) ||
        !valid_weapon_id(combat_state.active_weapon_id)) {
        return false;
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

    MovementState& movement = world_.registry().get_or_emplace<MovementState>(*entity);
    movement.speed_meters_per_second = combat_state.move_speed_meters_per_second;

    WeaponState& weapon = world_.registry().get_or_emplace<WeaponState>(*entity);
    weapon.weapon_id = combat_state.active_weapon_id;
    for (std::size_t index = 0; index < kWeaponCount; ++index) {
        weapon.ammo[index] = combat_state.ammo[index];
        weapon.reserve_ammo[index] = combat_state.reserve_ammo[index];
    }
    if (net_id == local_player_net_id_) {
        local_player_move_speed_meters_per_second_ =
            combat_state.move_speed_meters_per_second;
    }
    publish_snapshot();
    rebuild_render_states();
    return true;
}

bool KernelEngine::server_set_entity_weapon_mechanics(
    NetId net_id,
    const KernelWeaponMechanicsDefinition& weapon_mechanics) {
    if (!running_ || !is_server_mode(config_.mode) ||
        !validate_weapon_mechanics(weapon_mechanics)) {
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
    if (!running_ || !is_server_mode(config_.mode) || !valid_weapon_id(weapon_id)) {
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
        out_weapon_mechanics->struct_size < sizeof(KernelWeaponMechanicsDefinition) ||
        !valid_weapon_id(weapon_id)) {
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

bool KernelEngine::server_get_area_effect_state(
    NetId net_id,
    KernelAreaEffectState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < sizeof(KernelAreaEffectState)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, AreaEffectState, AreaEffectTag>(
            *entity)) {
        return false;
    }
    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const AreaEffectState& area_effect =
        world_.registry().get<AreaEffectState>(*entity);
    std::memset(out_state, 0, sizeof(KernelAreaEffectState));
    out_state->struct_size = sizeof(KernelAreaEffectState);
    out_state->net_id = identity.net_id;
    out_state->owner_peer = identity.owner_peer;
    out_state->radius = area_effect.radius;
    out_state->damage_per_interval = area_effect.damage_per_interval;
    out_state->damage_interval_ticks = area_effect.damage_interval_ticks;
    out_state->expire_tick = area_effect.expire_tick;
    out_state->source_code = area_effect.source_code;
    out_state->collision_mask = area_effect.collision_mask;
    out_state->valid = true;
    return true;
}

bool KernelEngine::server_get_beam_state(
    NetId net_id,
    KernelBeamState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < sizeof(KernelBeamState)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value() ||
        !world_.registry().all_of<NetworkIdentity, BeamState, BeamTag>(*entity)) {
        return false;
    }
    const NetworkIdentity& identity =
        world_.registry().get<NetworkIdentity>(*entity);
    const BeamState& beam = world_.registry().get<BeamState>(*entity);
    std::memset(out_state, 0, sizeof(KernelBeamState));
    out_state->struct_size = sizeof(KernelBeamState);
    out_state->net_id = identity.net_id;
    out_state->owner_peer = identity.owner_peer;
    out_state->shooter_net_id = beam.shooter_net_id;
    out_state->origin = to_kernel_vec3(beam.origin);
    out_state->direction = to_kernel_vec3(beam.direction);
    out_state->length = beam.length;
    out_state->radius = beam.radius;
    out_state->damage_per_second = beam.damage_per_second;
    out_state->expire_tick = beam.expire_tick;
    out_state->source_code = beam.source_code;
    out_state->collision_mask = beam.collision_mask;
    out_state->valid = true;
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
    out_state->max_turn_rate_degrees_per_second =
        homing.max_turn_rate_degrees_per_second;
    out_state->acceleration = homing.acceleration;
    out_state->max_speed = homing.max_speed;
    out_state->valid = true;
    return true;
}

bool KernelEngine::server_get_entity_state(
    NetId net_id,
    KernelServerEntityState* out_state) const {
    if (!running_ || !is_server_mode(config_.mode) || out_state == nullptr ||
        out_state->struct_size < sizeof(KernelServerEntityState)) {
        return false;
    }
    const std::optional<entt::entity> entity = world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    *out_state = to_server_entity_state(world_, *entity);
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
        out_states[count++] = to_server_entity_state(world_, entity);
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

void KernelEngine::reset_runtime_state(KernelMode mode) {
    config_.mode = mode;
    tick_loop_ = TickLoop(config_.tick);
    world_ = World{};
    history_buffer_ = HistoryBuffer(history_frame_count(config_.tick));
    damage_pipeline_.clear();
    pending_inputs_.clear();
    events_.clear();
    pending_presentation_events_.clear();
    render_states_.clear();
    latest_snapshot_ = WorldSnapshot{};
    latest_client_snapshot_ = WorldSnapshot{};
    client_snapshot_buffer_.clear();
    peer_sessions_.clear();
    local_listen_session_ = PeerSession{};
    client_replicated_entities_.clear();
    client_despawned_entities_.clear();
    pending_prediction_inputs_.clear();
    predicted_projectiles_.clear();
    entity_ids_by_net_id_.clear();
    predicted_local_entity_ = EntitySnapshot{};
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
    next_entity_id_ = 1;
    next_predicted_entity_id_ = UINT64_C(0x8000000000000000);
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    next_packet_sequence_ = 1;
    next_clock_sync_nonce_ = 1;
    client_local_time_us_ = 0;
    client_clock_offset_us_ = 0;
    local_player_move_speed_meters_per_second_ = 0.0f;
    current_render_time_us_ = 0;
    local_client_peer_id_ = 0;
    client_handshake_sent_ = false;
    has_welcome_ = false;
    has_client_snapshot_ = false;
    has_predicted_local_entity_ = false;
    has_client_clock_sync_ = false;
    has_client_render_time_ = false;
    running_ = true;
}

void KernelEngine::poll_transport() {
    TransportEvent transport_event;
    while (transport_->PollEvent(transport_event)) {
        if (transport_event.type == TransportEventType::kConnected) {
            push_event(KernelEventType_Connected, 0, transport_event.peer);
            if (config_.mode == KernelMode_Client) {
                send_client_handshake();
            }
        } else if (transport_event.type == TransportEventType::kDisconnected) {
            if (config_.mode == KernelMode_DedicatedServer) {
                handle_server_disconnect(transport_event);
            } else if (config_.mode == KernelMode_Client) {
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
            config_.mode == KernelMode_DedicatedServer) {
            handle_server_session_message(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSession &&
            config_.mode == KernelMode_Client) {
            handle_client_session_message(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kReliableEvent &&
            config_.mode == KernelMode_Client) {
            handle_client_reliable_event(transport_event);
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kSnapshot &&
            config_.mode == KernelMode_Client) {
            WorldSnapshot snapshot;
            if (!decode_snapshot_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &snapshot)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 6);
                continue;
            }
            handle_client_snapshot(std::move(snapshot));
        } else if (
            transport_event.type == TransportEventType::kMessage &&
            transport_event.channel == ChannelId::kInput &&
            (config_.mode == KernelMode_DedicatedServer ||
             config_.mode == KernelMode_ListenServer)) {
            const PeerSession* session = find_session(transport_event.peer);
            if (config_.mode == KernelMode_DedicatedServer &&
                (session == nullptr || !session->welcomed)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 10);
                continue;
            }

            PeerId player_id = 0;
            PlayerInput input{};
            if (!decode_input_packet(
                    transport_event.payload.data(),
                    transport_event.payload.size(),
                    &player_id,
                    &input)) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 5);
                continue;
            }
            if (player_id != transport_event.peer) {
                push_event(KernelEventType_Error, 0, transport_event.peer, 11);
                continue;
            }
            const std::uint64_t received_server_time_us = current_server_time_us();
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
            PeerSession* mutable_session = find_session(transport_event.peer);
            if (mutable_session != nullptr) {
                mutable_session->last_processed_input_seq =
                    std::max(mutable_session->last_processed_input_seq, input.input_seq);
            } else if (config_.mode == KernelMode_ListenServer) {
                local_last_processed_input_seq_ =
                    std::max(local_last_processed_input_seq_, input.input_seq);
            }
        }
    }
}

void KernelEngine::handle_server_disconnect(const TransportEvent& transport_event) {
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

void KernelEngine::handle_client_reliable_event(const TransportEvent& transport_event) {
    KernelEvent event{};
    if (decode_reliable_event_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &event)) {
        if (event.presentation_time_us != 0) {
            pending_presentation_events_.push_back(event);
            return;
        }
        events_.push_back(event);
        return;
    }

    EntitySpawnPacket spawn{};
    if (decode_entity_spawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &spawn)) {
        handle_client_spawn(spawn);
        return;
    }

    EntityDespawnPacket despawn{};
    if (decode_entity_despawn_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &despawn)) {
        handle_client_despawn(despawn);
        return;
    }

    push_event(KernelEventType_Error, 0, transport_event.peer, 14);
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
    }
}

void KernelEngine::apply_welcome(const WelcomePacket& welcome) {
    local_client_peer_id_ = welcome.assigned_peer_id;
    local_player_net_id_ = welcome.assigned_player_net_id;

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
    session->last_clock_sync_rtt_us =
        server_elapsed >= client_elapsed ? server_elapsed - client_elapsed : 0;
    session->pending_clock_sync_nonce = 0;
    session->pending_clock_sync_server_time_us = 0;
    session->has_clock_sync = true;
}

void KernelEngine::handle_client_spawn(const EntitySpawnPacket& packet) {
    client_despawned_entities_.erase(packet.net_id);
    auto found = std::find_if(
        client_replicated_entities_.begin(),
        client_replicated_entities_.end(),
        [&packet](const ClientReplicatedEntity& entity) {
            return entity.net_id == packet.net_id;
        });
    if (found == client_replicated_entities_.end()) {
        client_replicated_entities_.push_back(ClientReplicatedEntity{
            packet.net_id,
            packet.entity_type,
            packet.owner_peer,
            packet.position,
            packet.rotation,
            false,
        });
    } else {
        found->type = packet.entity_type;
        found->owner_peer = packet.owner_peer;
        found->position = packet.position;
        found->rotation = packet.rotation;
        found->active = false;
    }
    events_.push_back(KernelEvent{
        KernelEventType_EntitySpawned,
        packet.server_tick,
        packet.net_id,
        packet.owner_peer,
        static_cast<std::uint32_t>(packet.entity_type),
    });
}

void KernelEngine::handle_client_despawn(const EntityDespawnPacket& packet) {
    client_despawned_entities_.insert(packet.net_id);
    client_replicated_entities_.erase(
        std::remove_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&packet](const ClientReplicatedEntity& entity) {
                return entity.net_id == packet.net_id;
            }),
        client_replicated_entities_.end());
    events_.push_back(KernelEvent{
        KernelEventType_EntityDestroyed,
        packet.server_tick,
        packet.net_id,
        0,
        packet.reason,
    });
}

void KernelEngine::clear_client_session() {
    local_client_peer_id_ = 0;
    local_player_net_id_ = 0;
    local_last_processed_input_seq_ = 0;
    pending_prediction_inputs_.clear();
    predicted_projectiles_.clear();
    pending_presentation_events_.clear();
    client_snapshot_buffer_.clear();
    client_replicated_entities_.clear();
    client_despawned_entities_.clear();
    latest_client_snapshot_ = WorldSnapshot{};
    predicted_local_entity_ = EntitySnapshot{};
    local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
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

void KernelEngine::poll_client_transport() {
    if (loopback_transport_ == nullptr) {
        return;
    }

    TransportEvent transport_event;
    while (loopback_transport_->PollClientEvent(transport_event)) {
        if (transport_event.type != TransportEventType::kMessage) {
            continue;
        }
        if (transport_event.channel == ChannelId::kReliableEvent) {
            handle_client_reliable_event(transport_event);
            continue;
        }
        if (transport_event.channel != ChannelId::kSnapshot) {
            continue;
        }

        WorldSnapshot snapshot;
        if (!decode_snapshot_packet(
                transport_event.payload.data(),
                transport_event.payload.size(),
                &snapshot)) {
            push_event(KernelEventType_Error, 0, transport_event.peer, 6);
            continue;
        }
        handle_client_snapshot(std::move(snapshot));
    }
}

void KernelEngine::handle_client_snapshot(WorldSnapshot snapshot) {
    if (has_client_snapshot_ &&
        snapshot.header.server_tick < latest_client_snapshot_.header.server_tick) {
        store_client_snapshot(std::move(snapshot));
        return;
    }
    latest_client_snapshot_ = snapshot;
    has_client_snapshot_ = true;
    store_client_snapshot(std::move(snapshot));
    reconcile_local_prediction(latest_client_snapshot_);
    reconcile_predicted_projectiles(latest_client_snapshot_);
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

void KernelEngine::reconcile_local_prediction(const WorldSnapshot& snapshot) {
    if (local_player_net_id_ == 0) {
        return;
    }

    const EntitySnapshot* authoritative =
        find_snapshot_entity(snapshot, local_player_net_id_);
    if (authoritative == nullptr) {
        return;
    }

    pending_prediction_inputs_.erase(
        std::remove_if(
            pending_prediction_inputs_.begin(),
            pending_prediction_inputs_.end(),
            [&snapshot](const PlayerInput& input) {
                return input.input_seq <= snapshot.header.last_processed_input_seq;
            }),
        pending_prediction_inputs_.end());

    const glm::vec3 previous_render_position =
        has_predicted_local_entity_
            ? predicted_local_entity_.position + local_correction_offset_
            : authoritative->position;

    EntitySnapshot replayed = *authoritative;
    for (const PlayerInput& input : pending_prediction_inputs_) {
        apply_input_prediction(
            &replayed,
            input,
            tick_loop_.fixed_delta_seconds(),
            local_player_move_speed_meters_per_second_);
    }

    predicted_local_entity_ = replayed;
    has_predicted_local_entity_ = true;

    const glm::vec3 correction = previous_render_position - predicted_local_entity_.position;
    local_correction_offset_ =
        glm::length(correction) > 2.0f ? glm::vec3{0.0f, 0.0f, 0.0f} : correction;
}

void KernelEngine::reconcile_predicted_projectiles(const WorldSnapshot& snapshot) {
    std::unordered_set<NetId> snapshot_projectiles;
    const float fixed_delta_seconds = tick_loop_.fixed_delta_seconds();
    const std::uint32_t local_tick =
        local_prediction_server_tick(snapshot.header.server_tick);
    const std::uint32_t fast_forward_ticks =
        local_tick > snapshot.header.server_tick
            ? local_tick - snapshot.header.server_tick
            : 0;
    const float snapshot_age_seconds =
        static_cast<float>(fast_forward_ticks) * fixed_delta_seconds;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (entity.type != EntityType::kProjectile) {
            continue;
        }
        snapshot_projectiles.insert(entity.net_id);
        if (entity.client_action_id == 0) {
            continue;
        }
        PredictedProjectile* predicted =
            find_predicted_projectile(entity.owner_peer, entity.client_action_id);
        if (predicted == nullptr) {
            continue;
        }
        const float authoritative_age_seconds =
            predicted->motion_model == ProjectileMotionModel::kHoming
                ? std::min(snapshot_age_seconds, kMaxHomingVisualExtrapolationSeconds)
                : snapshot_age_seconds;
        const glm::vec3 previous_render_position =
            predicted->position + predicted->correction_offset;
        const glm::vec3 authoritative_now = projectile_position_at(
            entity.position,
            entity.velocity,
            predicted->motion_model,
            predicted->gravity,
            authoritative_age_seconds);
        const glm::vec3 authoritative_velocity_now = projectile_velocity_at(
            entity.velocity,
            predicted->motion_model,
            predicted->gravity,
            authoritative_age_seconds);
        predicted->net_id = entity.net_id;
        predicted->position = authoritative_now;
        predicted->rotation = entity.rotation;
        predicted->velocity = authoritative_velocity_now;
        predicted->spawn_position = entity.position;
        predicted->initial_velocity = entity.velocity;
        predicted->age_seconds = authoritative_age_seconds;
        predicted->spawn_tick = entity.spawn_tick;
        predicted->bound = true;
        const glm::vec3 correction = previous_render_position - authoritative_now;
        predicted->correction_offset =
            glm::length(correction) > 2.0f ? glm::vec3{0.0f, 0.0f, 0.0f}
                                           : correction;
        entity_ids_by_net_id_[entity.net_id] = predicted->entity_id;
    }

    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [&snapshot](const PredictedProjectile& projectile) {
                return !projectile.bound &&
                       projectile.input_seq != 0 &&
                       projectile.input_seq <= snapshot.header.last_processed_input_seq;
            }),
        predicted_projectiles_.end());
    predicted_projectiles_.erase(
        std::remove_if(
            predicted_projectiles_.begin(),
            predicted_projectiles_.end(),
            [&snapshot_projectiles](const PredictedProjectile& projectile) {
                return projectile.bound &&
                       snapshot_projectiles.find(projectile.net_id) ==
                           snapshot_projectiles.end();
            }),
        predicted_projectiles_.end());
}

void KernelEngine::predict_local_input(const PlayerInput& input) {
    if (local_player_net_id_ == 0) {
        return;
    }

    if (!has_predicted_local_entity_) {
        if (const EntitySnapshot* latest =
                find_snapshot_entity(latest_client_snapshot_, local_player_net_id_)) {
            predicted_local_entity_ = *latest;
        } else {
            predicted_local_entity_.net_id = local_player_net_id_;
            predicted_local_entity_.type = EntityType::kPlayer;
        }
        has_predicted_local_entity_ = true;
    }

    apply_input_prediction(
        &predicted_local_entity_,
        input,
        tick_loop_.fixed_delta_seconds(),
        local_player_move_speed_meters_per_second_);
    pending_prediction_inputs_.push_back(input);
}

void KernelEngine::predict_local_projectile(const PlayerInput& input) {
    if (local_client_peer_id_ == 0 || input.client_action_id == 0 ||
        (input.buttons & InputButton_Fire) == 0 ||
        find_predicted_projectile(local_client_peer_id_, input.client_action_id) != nullptr) {
        return;
    }
    const WeaponMechanicsDefinition* weapon =
        entity_weapon_mechanics(world_, local_player_net_id_, input.selected_weapon);
    if (weapon == nullptr || weapon->mode != WeaponFireMode::kProjectile) {
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
    const glm::vec3 velocity = direction * weapon->projectile_speed;
    const ProjectileMotionModel motion_model = weapon->projectile_motion_model;
    const glm::vec3 gravity = weapon->projectile_gravity;
    predicted_projectiles_.push_back(PredictedProjectile{
        allocate_predicted_entity_id(),
        0,
        local_client_peer_id_,
        input.input_seq,
        input.client_action_id,
        tick_loop_.current_tick(),
        0.0f,
        origin,
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        velocity,
        origin,
        velocity,
        gravity,
        motion_model,
        glm::vec3{0.0f, 0.0f, 0.0f},
        false,
    });
}

PlayerInput KernelEngine::prepare_client_input(const PlayerInput& input) {
    PlayerInput input_to_send = input;
    if (input_to_send.client_action_time_us == 0) {
        input_to_send.client_action_time_us = client_local_action_time_us();
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

    const std::uint64_t interpolation_delay_us =
        tick_time_us(
            tick_loop_.snapshot_interval_ticks() * 2u,
            tick_loop_.fixed_delta_seconds());
    std::uint64_t target_server_time_us = 0;
    if (has_client_clock_sync_) {
        const std::uint64_t server_now_us =
            offset_time_us(client_render_time_us, client_clock_offset_us_);
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
            tick_time_us(target_tick, tick_loop_.fixed_delta_seconds());
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

void KernelEngine::append_predicted_local_render_state(bool consume_correction) {
    if (!has_predicted_local_entity_) {
        return;
    }

    EntitySnapshot local = predicted_local_entity_;
    local.position += local_correction_offset_;
    render_states_.push_back(render_state_from_snapshot_entity(
        local,
        entity_id_for_net_id(local.net_id)));

    if (consume_correction) {
        local_correction_offset_ *= 0.5f;
        if (glm::length(local_correction_offset_) < 0.001f) {
            local_correction_offset_ = glm::vec3{0.0f, 0.0f, 0.0f};
        }
    }
}

void KernelEngine::append_predicted_projectile_render_states(bool consume_correction) {
    for (PredictedProjectile& projectile : predicted_projectiles_) {
        const glm::vec3 render_position =
            projectile.position + projectile.correction_offset;
        render_states_.push_back(RenderEntityState{
            projectile.entity_id,
            projectile.net_id,
            static_cast<std::uint16_t>(EntityType::kProjectile),
            projectile.owner_peer,
            to_kernel_vec3(render_position),
            to_kernel_quat(projectile.rotation),
            to_kernel_vec3(projectile.velocity),
            0,
            0,
            0,
            kVisualFlagMoving,
            projectile.spawn_tick,
            projectile.client_action_id,
        });
        if (consume_correction) {
            projectile.correction_offset *= 0.5f;
            if (glm::length(projectile.correction_offset) < 0.001f) {
                projectile.correction_offset = glm::vec3{0.0f, 0.0f, 0.0f};
            }
        }
    }
}

void KernelEngine::advance_predicted_projectiles(float fixed_delta_seconds) {
    for (PredictedProjectile& projectile : predicted_projectiles_) {
        projectile.age_seconds += fixed_delta_seconds;
        projectile.position = projectile_position_at(
            projectile.spawn_position,
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            projectile.age_seconds);
        projectile.velocity = projectile_velocity_at(
            projectile.initial_velocity,
            projectile.motion_model,
            projectile.gravity,
            projectile.age_seconds);
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
    std::uint32_t client_action_id) {
    if (client_action_id == 0) {
        return nullptr;
    }
    auto found = std::find_if(
        predicted_projectiles_.begin(),
        predicted_projectiles_.end(),
        [owner_peer, client_action_id](const PredictedProjectile& projectile) {
            return projectile.owner_peer == owner_peer &&
                   projectile.client_action_id == client_action_id;
        });
    if (found == predicted_projectiles_.end()) {
        return nullptr;
    }
    return &(*found);
}

void KernelEngine::simulate_tick() {
    const float fixed_delta = tick_loop_.fixed_delta_seconds();
    const std::uint64_t server_time_us =
        tick_time_us(tick_loop_.current_tick(), fixed_delta);
    const std::size_t first_tick_event = events_.size();
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
    simulate_player_movement(world_, pending_inputs_, fixed_delta);
    simulate_velocity_movement(world_, fixed_delta);
    simulate_weapons(world_, {}, tick_loop_.current_tick(), &events_);
    for (const QueuedInput& pending_input : pending_inputs_) {
        const HistoryFrame* rewind_frame = nullptr;
        std::uint32_t rewind_tick = tick_loop_.current_tick();
        if (pending_input.controlled_net_id == 0 &&
            (pending_input.input.buttons & InputButton_Fire) != 0) {
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
                compensated_action_time_us(pending_input)},
            &events_);
    }
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
        &damage_pipeline_);
    simulate_beams(
        world_,
        tick_loop_.current_tick(),
        fixed_delta,
        server_time_us,
        &events_,
        &damage_pipeline_);
    const std::vector<ConfirmedDamage> ready_damage =
        damage_pipeline_.drain_ready_damage(world_, server_time_us);
    apply_damage_applications(
        world_,
        ready_damage,
        tick_loop_.current_tick(),
        &events_);
    destroy_dead_entities(world_, tick_loop_.current_tick(), &events_);
    const std::size_t last_tick_event = events_.size();
    broadcast_combat_events(first_tick_event, last_tick_event);
    send_due_clock_sync_pings(server_time_us);
    history_buffer_.write_frame(world_, tick_loop_.current_tick());
    if (tick_loop_.should_write_snapshot()) {
        publish_snapshot();
    }
    pending_inputs_.clear();
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
        if (is_entity_relevant_to_session(session, entity, player_entity)) {
            filtered.entities.push_back(entity);
        }
    }
    return filtered;
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
    if (distance <= kDefaultEntityRelevanceDistanceMeters) {
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
    for (const EntitySnapshot& entity : snapshot.entities) {
        next_relevant.insert(entity.net_id);
        if (session->relevant_entities.find(entity.net_id) ==
            session->relevant_entities.end()) {
            send_entity_spawn(session->peer, entity);
        }
    }

    for (NetId net_id : session->relevant_entities) {
        if (next_relevant.find(net_id) == next_relevant.end()) {
            send_entity_despawn(
                session->peer,
                net_id,
                KernelDespawnReason_OutOfRange);
        }
    }
    session->relevant_entities = std::move(next_relevant);
}

void KernelEngine::send_entity_spawn(PeerId peer, const EntitySnapshot& entity) {
    PeerId owner_peer = 0;
    glm::vec3 spawn_position = entity.position;
    const std::optional<entt::entity> world_entity = world_.find_entity(entity.net_id);
    if (world_entity.has_value() &&
        world_.registry().all_of<NetworkIdentity>(*world_entity)) {
        owner_peer = world_.registry().get<NetworkIdentity>(*world_entity).owner_peer;
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
            owner_peer,
            tick_loop_.current_tick(),
            spawn_position,
            entity.rotation,
        },
        next_packet_sequence_++);
    if (!transport_->Send(
            peer,
            packet.data(),
            static_cast<std::uint32_t>(packet.size()),
            SendMode::kReliable,
            ChannelId::kReliableEvent)) {
        push_event(KernelEventType_Error, entity.net_id, peer, 16);
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
    }
}

void KernelEngine::rebuild_render_states() {
    rebuild_render_states_at_time(client_local_time_us_, true);
}

void KernelEngine::rebuild_render_states_at_time(
    std::uint64_t client_render_time_us,
    bool consume_correction) {
    if ((config_.mode == KernelMode_ListenServer || config_.mode == KernelMode_Client) &&
        has_client_snapshot_) {
        rebuild_render_states_from_snapshot(client_render_time_us, consume_correction);
        return;
    }
    rebuild_render_states_from_world();
}

void KernelEngine::rebuild_render_states_from_world() {
    render_states_.clear();
    auto view = world_.registry().view<const NetworkIdentity, const EntityKind, const Transform>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        render_states_.push_back(render_state_from_world_entity(
            world_,
            entity,
            entity_id_for_net_id(identity.net_id)));
    }
}

void KernelEngine::rebuild_render_states_from_snapshot(
    std::uint64_t client_render_time_us,
    bool consume_correction) {
    render_states_.clear();
    append_predicted_local_render_state(consume_correction);
    append_predicted_projectile_render_states(consume_correction);

    WorldSnapshot render_snapshot;
    const WorldSnapshot& snapshot =
        build_interpolated_snapshot(client_render_time_us, &render_snapshot)
            ? render_snapshot
            : latest_client_snapshot_;
    current_render_time_us_ =
        tick_time_us(snapshot.header.server_tick, tick_loop_.fixed_delta_seconds());
    has_client_render_time_ = true;
    std::unordered_set<NetId> rendered_entities;
    for (const EntitySnapshot& entity : snapshot.entities) {
        if (has_predicted_local_entity_ && entity.net_id == local_player_net_id_) {
            continue;
        }
        if (client_despawned_entities_.find(entity.net_id) !=
            client_despawned_entities_.end()) {
            continue;
        }
        if (has_predicted_projectile_net_id(entity.net_id)) {
            rendered_entities.insert(entity.net_id);
            continue;
        }
        render_states_.push_back(render_state_from_snapshot_entity(
            entity,
            entity_id_for_net_id(entity.net_id)));
        rendered_entities.insert(entity.net_id);
        auto replicated = std::find_if(
            client_replicated_entities_.begin(),
            client_replicated_entities_.end(),
            [&entity](const ClientReplicatedEntity& replicated_entity) {
                return replicated_entity.net_id == entity.net_id;
            });
        if (replicated != client_replicated_entities_.end()) {
            replicated->active = true;
            replicated->position = entity.position;
            replicated->rotation = entity.rotation;
        }
    }

    for (const ClientReplicatedEntity& entity : client_replicated_entities_) {
        if (entity.net_id == local_player_net_id_ ||
            rendered_entities.find(entity.net_id) != rendered_entities.end() ||
            client_despawned_entities_.find(entity.net_id) !=
                client_despawned_entities_.end()) {
            continue;
        }
        render_states_.push_back(RenderEntityState{
            entity_id_for_net_id(entity.net_id),
            entity.net_id,
            static_cast<std::uint16_t>(entity.type),
            entity.owner_peer,
            to_kernel_vec3(entity.position),
            to_kernel_quat(entity.rotation),
            KernelVec3{0.0f, 0.0f, 0.0f},
            0,
            0,
            0,
            0,
            0,
            0,
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
    spdlog::debug(
        "{}",
        fmt::format(
            "snapshot tick={} entities={}",
            latest_snapshot_.header.server_tick,
            latest_snapshot_.entities.size()));

    if (config_.mode == KernelMode_ListenServer && loopback_transport_ != nullptr) {
        local_listen_session_.peer = kLocalListenPeerId;
        local_listen_session_.player = local_player_net_id_;
        local_listen_session_.last_processed_input_seq = local_last_processed_input_seq_;
        local_listen_session_.welcomed = local_player_net_id_ != 0;
        const WorldSnapshot peer_snapshot =
            build_relevant_snapshot(local_listen_session_, server_time_ms);
        sync_session_relevance(&local_listen_session_, peer_snapshot);
        const std::vector<std::uint8_t> packet =
            encode_snapshot_packet(peer_snapshot, next_packet_sequence_++);
        if (!loopback_transport_->Send(
                kLocalListenPeerId,
                packet.data(),
                static_cast<std::uint32_t>(packet.size()),
                SendMode::kUnreliable,
                ChannelId::kSnapshot)) {
            push_event(KernelEventType_Error, 0, kLocalListenPeerId, 7);
        }
    }

    if (config_.mode == KernelMode_DedicatedServer) {
        for (PeerSession& session : peer_sessions_) {
            if (!session.welcomed) {
                continue;
            }
            const WorldSnapshot peer_snapshot =
                build_relevant_snapshot(session, server_time_ms);
            sync_session_relevance(&session, peer_snapshot);
            const std::vector<std::uint8_t> packet =
                encode_snapshot_packet(peer_snapshot, next_packet_sequence_++);
            if (!transport_->Send(
                    session.peer,
                    packet.data(),
                    static_cast<std::uint32_t>(packet.size()),
                    SendMode::kUnreliable,
                    ChannelId::kSnapshot)) {
                push_event(KernelEventType_Error, 0, session.peer, 7);
            }
        }
    }
}

void KernelEngine::send_client_handshake() {
    if (client_handshake_sent_) {
        return;
    }

    const std::vector<std::uint8_t> packet = encode_handshake_packet(
        HandshakePacket{kClientNonce},
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
    client_handshake_sent_ = true;
}

void KernelEngine::send_clock_sync_ping(
    PeerSession* session,
    std::uint64_t server_time_us) {
    if (session == nullptr || !session->welcomed || transport_ == nullptr) {
        return;
    }

    const std::uint32_t nonce = next_clock_sync_nonce_++;
    const PingPongPacket ping{nonce, server_time_us, 0, 0};
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

    session->pending_clock_sync_nonce = nonce;
    session->pending_clock_sync_server_time_us = server_time_us;
    session->last_clock_sync_sent_server_time_us = server_time_us;
}

void KernelEngine::send_due_clock_sync_pings(std::uint64_t server_time_us) {
    if (config_.mode != KernelMode_DedicatedServer) {
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
    if (!decode_handshake_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &handshake)) {
        push_event(KernelEventType_Error, 0, transport_event.peer, 9);
        return;
    }

    PeerSession* session = find_session(transport_event.peer);
    if (session == nullptr) {
        const NetId player = world_.spawn_player(
            transport_event.peer,
            glm::vec3{0.0f, 0.0f, 0.0f});
        peer_sessions_.push_back(PeerSession{transport_event.peer, player, 0, false});
        session = &peer_sessions_.back();
        push_event(KernelEventType_PlayerJoined, player, transport_event.peer);
        push_event(
            KernelEventType_EntitySpawned,
            player,
            transport_event.peer,
            static_cast<std::uint32_t>(EntityType::kPlayer));
    }

    const WelcomePacket welcome{
        transport_event.peer,
        session->player,
        tick_loop_.current_tick(),
        config_.tick.server_tick_rate,
        config_.tick.snapshot_rate,
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

    session->welcomed = true;
    send_clock_sync_ping(session, current_server_time_us());
    publish_snapshot();
}

void KernelEngine::handle_server_session_message(const TransportEvent& transport_event) {
    PingPongPacket ping;
    if (decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &ping)) {
        handle_server_ping_pong(transport_event);
        return;
    }

    handle_server_handshake(transport_event);
}

void KernelEngine::handle_client_session_message(const TransportEvent& transport_event) {
    WelcomePacket welcome;
    if (decode_welcome_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &welcome)) {
        apply_welcome(welcome);
        push_event(KernelEventType_PlayerJoined, 0, local_client_peer_id_);
        return;
    }

    PingPongPacket ping;
    if (decode_ping_pong_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &ping)) {
        handle_client_ping_pong(transport_event);
        return;
    }

    DisconnectPacket disconnect;
    if (decode_disconnect_packet(
            transport_event.payload.data(),
            transport_event.payload.size(),
            &disconnect)) {
        clear_client_session();
        push_event(KernelEventType_Disconnected, 0, kServerPeerId, disconnect.reason_code);
        return;
    }

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
