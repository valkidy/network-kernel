#ifndef WORLD_PUBLIC_COMPONENTS_H_
#define WORLD_PUBLIC_COMPONENTS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example {

using NetId = std::uint32_t;
using PeerId = std::uint32_t;

enum class EntityType : std::uint16_t {
    kUnknown = 0,
    kActor = 1,
    kProjectile = 3,
};

enum class ActorType : std::uint16_t {
    kUnknown = 0,
    kPlayer = 1,
    kAgent = 2,
};

enum class ColliderShapeType : std::uint8_t {
    kAabb = 0,
    kSphere = 1,
    kOrientedBox = 2,
    kSegment = 3,
    kCone = 4,
};

struct ColliderWorldBounds {
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
};

struct ColliderInstance {
    std::uint32_t collider_id = 0;
    std::uint32_t collider_template_id = 0;
    NetId owner_net_id = 0;
    NetId entity_net_id = 0;
    EntityType entity_type = EntityType::kUnknown;
    ActorType actor_type = ActorType::kUnknown;
    ColliderShapeType shape_type = ColliderShapeType::kAabb;
    std::uint32_t purpose_flags = 0;
    std::uint32_t layer_mask = 0;
    glm::vec3 local_center{0.0f, 0.0f, 0.0f};
    glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 world_center{0.0f, 0.0f, 0.0f};
    glm::quat world_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    glm::vec3 segment_start{0.0f, 0.0f, 0.0f};
    glm::vec3 segment_end{0.0f, 0.0f, 0.0f};
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t remaining_ticks = 0;
    bool has_resolved_damage = false;
    ColliderWorldBounds world_bounds{};
};

struct NetworkIdentity {
    NetId net_id = 0;
    PeerId owner_peer = 0;
};

struct EntityKind {
    EntityType type = EntityType::kUnknown;
    ActorType actor_type = ActorType::kUnknown;
};

struct ActorTemplateRef {
    std::uint32_t actor_template_id = 0;
};

struct Transform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct Velocity {
    glm::vec3 linear{0.0f, 0.0f, 0.0f};
};

struct Health {
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PlayerTag {};
struct AgentTag {};
struct ProjectileTag {};

inline constexpr std::size_t kWeaponCount = 7;
inline constexpr std::uint8_t kWeaponSlot0 = 0;
inline constexpr std::uint8_t kWeaponSlot1 = 1;
inline constexpr std::uint8_t kWeaponSlot2 = 2;
inline constexpr std::uint8_t kWeaponSlot3 = 3;
inline constexpr std::uint8_t kWeaponSlot4 = 4;
inline constexpr std::uint8_t kWeaponSlot5 = 5;
inline constexpr std::uint8_t kWeaponSlot6 = 6;

enum class WeaponFireMode : std::uint8_t {
    kHitscan = 0,
    kShotgun = 1,
    kProjectile = 2,
};

enum class ProjectileMotionModel : std::uint8_t {
    kLinear = 0,
    kParabolic = 1,
    kHoming = 2,
};

enum class ProjectileSyncMode : std::uint8_t {
    kLocalPredictedDeterministic = 0,
    kHybridDeterministicThenSnapshot = 1,
    kServerSnapshotOnly = 2,
};

enum class MissileGuidancePhase : std::uint8_t {
    kBoost = 0,
    kGuided = 1,
    kLostTarget = 2,
    kExpired = 3,
};

enum class HomingMode : std::uint8_t {
    kFireAndForget = 0,
};

enum class ProjectileHitResponse : std::uint8_t {
    kDestroy = 0,
    kContinue = 1,
    kBounce = 2,
    kAttach = 3,
};

enum class ProjectileDamageShape : std::uint8_t {
    kDirectHit = 0,
    kPiercingSegment = 2,
};

enum class ProjectileType : std::uint8_t {
    kStandard = 0,
    kAreaEffect = 1,
    kBeam = 2,
};

enum class ProjectileDamageFalloff : std::uint8_t {
    kNone = 0,
    kLinear = 1,
};

enum class ProjectileCollisionQueryMode : std::uint8_t {
    kAuto = 0,
    kOverlap = 1,
    kSweep = 2,
    kRay = 3,
};

struct ProjectileCollisionGeometry {
    ColliderShapeType shape_type = ColliderShapeType::kSegment;
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float length = 0.0f;
    float angle_degrees = 0.0f;
};

inline constexpr std::uint32_t kCollisionLayerPlayerSide = 0x00000001u;
inline constexpr std::uint32_t kCollisionLayerHostileSide = 0x00000002u;
inline constexpr std::uint32_t kCollisionLayerProjectile = 0x00000004u;
inline constexpr std::uint32_t kCollisionLayerNeutral = 0x00000020u;
inline constexpr std::uint32_t kCollisionMaskNone = 0x00000000u;
inline constexpr std::uint32_t kCollisionMaskDamageable =
    kCollisionLayerPlayerSide | kCollisionLayerHostileSide | kCollisionLayerNeutral;

struct WeaponState {
    std::uint8_t weapon_id = 0;
    std::array<std::uint16_t, kWeaponCount> ammo{0, 0, 0, 0, 0, 0, 0};
    std::array<std::uint16_t, kWeaponCount> reserve_ammo{0, 0, 0, 0, 0, 0, 0};
    std::array<std::uint32_t, kWeaponCount> next_fire_tick{0, 0, 0, 0, 0, 0, 0};
    std::uint32_t reload_end_tick = 0;
    bool is_reloading = false;
};

struct WeaponMechanicsDefinition {
    std::uint8_t id = 0;
    WeaponFireMode mode = WeaponFireMode::kHitscan;
    std::uint16_t magazine_size = 0;
    std::uint16_t damage = 0;
    std::uint32_t cooldown_ticks = 0;
    std::uint32_t reload_ticks = 0;
    float max_range = 0.0f;
    std::uint8_t pellet_count = 1;
    float pellet_spread = 0.0f;
    std::uint32_t segment_collider_template_id = 0;
    std::uint32_t projectile_template_id = 0;
};

struct WeaponTuning {
    std::array<bool, kWeaponCount> configured{false, false, false, false, false, false, false};
    std::array<WeaponMechanicsDefinition, kWeaponCount> definitions{};
};

struct Hitbox {
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.5f, 0.5f, 0.5f};
    std::uint32_t collider_template_id = 0;
    std::uint8_t hit_zone = 0;
};

struct ProjectileState {
    std::uint8_t weapon_id = 0;
    std::uint32_t projectile_template_id = 0;
    std::uint16_t damage = 0;
    std::uint32_t spawn_tick = 0;
    std::uint32_t client_action_id = 0;
    NetId shooter_net_id = 0;
    ProjectileMotionModel motion_model = ProjectileMotionModel::kLinear;
    ProjectileHitResponse hit_response = ProjectileHitResponse::kDestroy;
    ProjectileDamageShape damage_shape = ProjectileDamageShape::kDirectHit;
    ProjectileCollisionQueryMode collision_query_mode =
        ProjectileCollisionQueryMode::kAuto;
    ProjectileCollisionGeometry collision_geometry{};
    bool has_collision_geometry = false;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    std::uint32_t max_hit_count = 1;
    std::uint32_t hit_count = 0;
    float max_lifetime_seconds = 0.0f;
    float age_seconds = 0.0f;
    glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
    glm::vec3 initial_velocity{0.0f, 0.0f, 0.0f};
    // Future non-deterministic physics projectiles should use authoritative
    // snapshots plus render-side correction after a physics module exists.
    glm::vec3 gravity{0.0f, 0.0f, 0.0f};
    glm::vec3 previous_position{0.0f, 0.0f, 0.0f};
};

struct RuntimeProjectileTemplate {
    std::uint32_t projectile_template_id = 0;
    std::uint8_t weapon_id = 0;
    ProjectileType projectile_type = ProjectileType::kStandard;
    ProjectileMotionModel motion_model = ProjectileMotionModel::kLinear;
    ProjectileSyncMode sync_mode = ProjectileSyncMode::kHybridDeterministicThenSnapshot;
    ProjectileHitResponse hit_response = ProjectileHitResponse::kDestroy;
    ProjectileDamageShape damage_shape = ProjectileDamageShape::kDirectHit;
    ProjectileCollisionQueryMode collision_query_mode =
        ProjectileCollisionQueryMode::kAuto;
    ProjectileCollisionGeometry collision_geometry{};
    bool has_collision_geometry = false;
    bool impact_destroy_self = true;
    ProjectileDamageFalloff damage_falloff = ProjectileDamageFalloff::kNone;
    std::uint16_t damage = 0;
    float speed = 0.0f;
    float lifetime_seconds = 0.0f;
    glm::vec3 gravity{0.0f, 0.0f, 0.0f};
    std::uint32_t collider_template_id = 0;
    float area_radius = 0.0f;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    std::uint32_t max_hit_count = 1;
    std::uint32_t impact_spawn_projectile_template_id = 0;
    std::uint32_t expire_spawn_projectile_template_id = 0;
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t damage_interval_ticks = 1;
    float beam_length = 0.0f;
    float beam_radius = 0.0f;
    HomingMode homing_mode = HomingMode::kFireAndForget;
    std::uint32_t homing_boost_ticks = 0;
    float homing_lock_on_range = 0.0f;
    float homing_lose_target_range = 0.0f;
    float homing_lock_cone_degrees = 0.0f;
    float homing_max_turn_rate_degrees_per_second = 0.0f;
    float homing_acceleration = 0.0f;
    float homing_max_speed = 0.0f;
};

struct HomingState {
    HomingMode homing_mode = HomingMode::kFireAndForget;
    ProjectileSyncMode sync_mode = ProjectileSyncMode::kHybridDeterministicThenSnapshot;
    MissileGuidancePhase phase = MissileGuidancePhase::kBoost;
    NetId target_net_id = 0;
    std::uint32_t boost_ticks = 0;
    std::uint32_t guidance_start_tick = 0;
    float lock_on_range = 0.0f;
    float lose_target_range = 0.0f;
    float lock_cone_degrees = 0.0f;
    float max_turn_rate_degrees_per_second = 0.0f;
    float acceleration = 0.0f;
    float max_speed = 0.0f;
};

struct ProjectileAreaEffectRuntime {
    float radius = 0.0f;
    std::uint16_t damage_per_interval = 0;
    std::uint32_t damage_interval_ticks = 1;
    std::uint32_t expire_tick = 0;
    std::uint8_t source_code = 0;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    ProjectileDamageFalloff damage_falloff = ProjectileDamageFalloff::kNone;
    std::unordered_map<NetId, std::uint32_t> next_damage_tick_by_target;
};

struct ProjectileInteractionRule {
    std::uint8_t lhs_weapon_id = 0;
    std::uint8_t rhs_weapon_id = 0;
    bool symmetric = true;
    bool destroy_lhs = true;
    bool destroy_rhs = true;
    std::uint32_t spawn_projectile_template_id = 0;
};

struct ProjectileBeamRuntime {
    NetId shooter_net_id = 0;
    glm::vec3 origin{0.0f, 0.0f, 0.0f};
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    float length = 0.0f;
    float radius = 0.0f;
    std::uint16_t damage_per_second = 0;
    std::uint32_t expire_tick = 0;
    std::uint8_t source_code = 0;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    std::unordered_map<NetId, std::uint32_t> damage_remainder_by_target;
};

struct MovementState {
    float speed_meters_per_second = 0.0f;
};

inline constexpr std::uint32_t kVisualFlagMoving = 0x00000001u;
inline constexpr std::uint32_t kVisualFlagReloading = 0x00000002u;
inline constexpr std::uint32_t kVisualFlagDead = 0x00000004u;
inline constexpr std::uint32_t kVisualFlagHpUnknown = 0x00000008u;

struct ReplicationState {
    std::uint16_t animation_state = 0;
    std::uint32_t visual_flags = 0;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_COMPONENTS_H_
