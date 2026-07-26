#ifndef WORLD_PUBLIC_COMPONENTS_H_
#define WORLD_PUBLIC_COMPONENTS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example {

using NetId = std::uint32_t;
using PeerId = std::uint32_t;

enum class EntityType : std::uint16_t {
    kUnknown = 0,
    kActor = 1,
    kProp = 2,
    kProjectile = 3,
    kDirector = 5,
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
    kCapsule = 5,
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
    std::uint32_t hit_zone = 0;
    glm::vec3 local_center{0.0f, 0.0f, 0.0f};
    glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 world_center{0.0f, 0.0f, 0.0f};
    glm::quat world_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float capsule_half_height = 0.0f;
    glm::vec3 segment_start{0.0f, 0.0f, 0.0f};
    glm::vec3 segment_end{0.0f, 0.0f, 0.0f};
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t remaining_ticks = 0;
    bool has_resolved_damage = false;
    bool enabled = true;
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
struct ServerOnly {};

enum class AiControllerType : std::uint32_t {
    kNone = 0,
    kSentry = 1,
    kDirector = 2,
};

struct AgentRuntime {
    std::uint32_t ai_profile_id = 0;
    AiControllerType controller_type = AiControllerType::kNone;
    std::uint32_t tick_interval = 1;
    std::uint32_t next_tick = 0;
    std::uint32_t blackboard_id = 0;
};

struct AgentSentryRuntime {};

struct DirectorRuntime {
    std::uint32_t tick_interval = 1;
    std::uint32_t next_tick = 0;
    std::uint32_t spawn_target_count = 0;
    std::uint32_t spawn_entity_template_id = 0;
    std::uint32_t spawn_actor_template_id = 0;
    glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
    float spawn_radius = 0.0f;
    std::uint32_t spawn_seed = 1;
    std::uint32_t spawn_cursor = 0;
};

inline constexpr std::size_t kWeaponSlotCount = 4;
inline constexpr std::size_t kWeaponIdCount = 256;
inline constexpr std::uint8_t kWeaponSlot0 = 0;
inline constexpr std::uint8_t kWeaponSlot1 = 1;
inline constexpr std::uint8_t kWeaponSlot2 = 2;
inline constexpr std::uint8_t kWeaponSlot3 = 3;
inline constexpr std::uint8_t kWeaponId4 = 4;
inline constexpr std::uint8_t kWeaponId5 = 5;
inline constexpr std::uint8_t kWeaponId6 = 6;

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
    kNone = 1,
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
    // Select overlap, sweep, or ray from collider geometry and projectile motion.
    kAuto = 0,
    // Test the projectile volume only at its current transform.
    kOverlap = 1,
    // Test the projectile volume swept from its previous to current transform.
    kSweep = 2,
    // Test a thin segment/ray and ignore projectile volume extents.
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
    std::uint8_t active_weapon_slot = 0;
    std::uint8_t weapon_slot_count = 0;
    std::array<std::uint32_t, kWeaponSlotCount> weapon_ids{};
    std::array<std::uint16_t, kWeaponSlotCount> ammo{};
    // reserve_magazines counts spare full magazines, not spare bullets.
    // UINT16_MAX is allowed as an authored practical maximum for match lengths
    // that should not exhaust reserves in normal play. It is not a sentinel:
    // reload logic decrements it like any other count and must not special-case 65535.
    std::array<std::uint16_t, kWeaponSlotCount> reserve_magazines{};
    std::array<std::uint32_t, kWeaponSlotCount> next_primary_commit_tick{};
    NetId active_effect_net_id = 0;
    bool is_reloading = false;
};

inline std::size_t find_weapon_slot(
    const WeaponState& weapon,
    std::uint8_t weapon_id) {
    for (std::size_t slot = 0; slot < weapon.weapon_slot_count; ++slot) {
        if (weapon.weapon_ids[slot] == weapon_id) {
            return slot;
        }
    }
    return kWeaponSlotCount;
}

inline std::uint8_t active_weapon_id(const WeaponState& weapon) {
    if (weapon.active_weapon_slot >= weapon.weapon_slot_count) {
        return 0;
    }
    return static_cast<std::uint8_t>(
        weapon.weapon_ids[weapon.active_weapon_slot]);
}

inline bool projected_primary_commit_is_blocked(
    std::uint32_t current_tick,
    std::uint32_t commit_offset_ticks,
    std::uint32_t next_primary_commit_tick) {
    if (next_primary_commit_tick == 0u) {
        return false;
    }
    const std::uint32_t projected_commit_tick =
        current_tick + commit_offset_ticks;
    return static_cast<std::int32_t>(
               projected_commit_tick - next_primary_commit_tick) < 0;
}

struct RuntimeActionTemplate {
    std::uint32_t action_template_id = 0;
    std::uint8_t trigger_mode = 0;
    std::uint8_t flags = 0;
    std::uint16_t ammo_cost_per_commit = 0;
    std::uint32_t commit_offset_ticks = 0;
    std::uint32_t commit_interval_ticks = 0;
    std::uint32_t max_commit_count = 0;
    std::uint32_t recovery_ticks = 0;
    std::uint32_t hold_input_timeout_ticks = 0;
};

struct ActionIntentState {
    std::uint32_t action_instance_id = 0;
    std::uint16_t binding_id = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved = 0;
};

struct ContinuousActionInputState {
    std::uint32_t action_instance_id = 0;
    std::uint8_t held = 0;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
};

struct ActionInputState {
    std::uint32_t buttons = 0;
    std::uint32_t last_input_tick = 0;
    ActionIntentState intent{};
    ContinuousActionInputState continuous{};
    std::uint8_t selected_weapon = 0;
    glm::vec3 aim_direction{1.0f, 0.0f, 0.0f};
};

struct ActionRuntimeState {
    std::uint32_t action_template_id = 0;
    std::uint32_t action_instance_id = 0;
    std::uint32_t next_generated_instance_id = 1;
    std::uint32_t start_tick = 0;
    std::uint32_t next_commit_tick = 0;
    std::uint32_t last_commit_tick = 0;
    std::uint32_t recovery_end_tick = 0;
    std::uint32_t commit_count = 0;
    std::uint32_t last_advanced_tick = UINT32_MAX;
    std::uint16_t binding_id = 0;
    std::uint8_t source_weapon_id = 0;
    std::uint8_t phase = 0;
    bool cancel_after_first_commit = false;
};

struct WeaponMechanicsDefinition {
    WeaponMechanicsDefinition() = default;
    WeaponMechanicsDefinition(
        std::uint8_t definition_id,
        WeaponFireMode fire_mode,
        std::uint16_t definition_magazine_size,
        std::uint16_t definition_damage,
        std::uint32_t /*legacy_fire_interval_ticks*/,
        std::uint32_t /*legacy_reload_ticks*/,
        float definition_max_range = 0.0f)
        : id(definition_id),
          mode(fire_mode),
          magazine_size(definition_magazine_size),
          damage(definition_damage),
          max_range(definition_max_range) {}

    std::uint8_t id = 0;
    WeaponFireMode mode = WeaponFireMode::kHitscan;
    std::uint16_t magazine_size = 0;
    std::uint16_t reserve_magazines = 0;
    std::uint16_t damage = 0;
    float max_range = 0.0f;
    std::uint8_t pellet_count = 1;
    float pellet_spread = 0.0f;
    std::uint32_t segment_collider_template_id = 0;
    std::uint32_t projectile_template_id = 0;
    std::uint32_t fire_action_template_id = 0;
    std::uint32_t reload_action_template_id = 0;
};

struct WeaponTuning {
    std::array<bool, kWeaponIdCount> configured{};
    std::array<WeaponMechanicsDefinition, kWeaponIdCount> definitions{};
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
    std::uint32_t action_instance_id = 0;
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
    std::uint32_t max_lifetime_ticks = 0;
    std::uint32_t age_ticks = 0;
    glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
    glm::vec3 initial_velocity{0.0f, 0.0f, 0.0f};
    // Future non-deterministic physics projectiles should use authoritative
    // snapshots plus render-side correction after a physics module exists.
    glm::vec3 gravity{0.0f, 0.0f, 0.0f};
    glm::vec3 previous_position{0.0f, 0.0f, 0.0f};
};

enum class TriggerEventType : std::uint8_t {
    kCollision,
    kProjectileImpact,
    kActivated,
    kItemUsed,
    kHealthDepleted,
    kDestroyEntity,
    kExpired,
};

struct OnCollisionTriggerTag {};
struct OnProjectileImpactTriggerTag {};
struct OnActivatedTriggerTag {};
struct OnHealthDepletedTriggerTag {};
struct OnDestroyEntityTriggerTag {};
struct OnExpiredTriggerTag {};

struct ProjectileImpactPayload {
    std::uint32_t projectile_template_id = 0;
    std::uint32_t action_instance_id = 0;
    std::uint8_t source_weapon_id = 0;
    bool historical = false;
};

struct TriggerEvent {
    TriggerEventType type = TriggerEventType::kCollision;
    NetId subject = 0;
    NetId instigator = 0;
    NetId target = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    std::optional<ProjectileImpactPayload> projectile_impact;
};

enum class ActionAuthoritySource : std::uint8_t {
    kAuthoritativeSimulation,
    kClientPrediction,
};

struct ActionExecutionProvenance {
    std::uint64_t request_id = 0;
    std::uint32_t action_instance_id = 0;
    std::uint32_t server_tick = 0;
    NetId instigator = 0;
    PeerId owner_peer = 0;
    std::uint8_t source_weapon_id = 0;
    ActionAuthoritySource authority_source =
        ActionAuthoritySource::kAuthoritativeSimulation;
};

struct EntityIdValue {
    NetId value = 0;
};

struct ProjectileTemplateIdValue {
    std::uint32_t value = 0;
};

struct EntityTemplateIdValue {
    std::uint32_t value = 0;
};

using ActionGraphParameterValue = std::variant<
    std::monostate,
    EntityIdValue,
    ProjectileTemplateIdValue,
    EntityTemplateIdValue,
    glm::vec3,
    float>;

enum class EntityRefSource : std::uint8_t {
    kSelf,
    kEventSubject,
    kEventTarget,
    kEventInstigator,
};

struct EntityRefExpression {
    EntityRefSource source = EntityRefSource::kSelf;
};

enum class EventVec3Source : std::uint8_t {
    kPosition,
    kDirection,
};

struct EventVec3Expression {
    EventVec3Source source = EventVec3Source::kPosition;
};

using ActionGraphParameterExpression = std::variant<
    ActionGraphParameterValue,
    EntityRefExpression,
    EventVec3Expression>;

struct ActionGraphParameterDefinition {
    std::string name;
    ActionGraphParameterValue default_value;
};

struct ActionGraphParameterBinding {
    std::string name;
    ActionGraphParameterExpression expression;
};

struct ActionSpawnProjectileDefinition {
    std::string projectile_template_parameter;
    std::string position_parameter;
    std::string direction_parameter;
};

struct ActionApplyDamageDefinition {
    std::string target_parameter;
    std::string amount_parameter;
};

struct ActionSpawnEntityDefinition {
    std::string entity_template_parameter;
    std::string position_parameter;
    std::string owner_parameter;
};

using ActionGraphAction = std::variant<
    ActionSpawnProjectileDefinition,
    ActionApplyDamageDefinition,
    ActionSpawnEntityDefinition>;

struct ActionGraphTemplate {
    std::string id;
    std::vector<ActionGraphParameterDefinition> parameters;
    std::vector<ActionGraphAction> actions;
};

struct CompiledActionGraphBinding {
    TriggerEventType event_type = TriggerEventType::kCollision;
    ActionGraphTemplate graph;
    std::vector<ActionGraphParameterBinding> parameters;
};

struct ActionGraphActivatedBinding {
    CompiledActionGraphBinding binding;
};

struct ActionGraphCollisionBinding {
    CompiledActionGraphBinding binding;
};

struct ActionGraphHealthDepletedBinding {
    CompiledActionGraphBinding binding;
};

struct ActionGraphDestroyEntityBinding {
    CompiledActionGraphBinding binding;
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
    std::uint32_t lifetime_ticks = 0;
    glm::vec3 gravity{0.0f, 0.0f, 0.0f};
    std::uint32_t collider_template_id = 0;
    float area_radius = 0.0f;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    std::uint32_t max_hit_count = 1;
    std::uint32_t impact_spawn_projectile_template_id = 0;
    std::uint32_t expire_spawn_projectile_template_id = 0;
    std::optional<CompiledActionGraphBinding> projectile_impact_binding;
    std::optional<CompiledActionGraphBinding> expired_binding;
    std::uint32_t damage_interval_ticks = 1;
    float beam_length = 0.0f;
    float beam_radius = 0.0f;
    HomingMode homing_mode = HomingMode::kFireAndForget;
    std::uint32_t homing_boost_ticks = 0;
    float homing_lock_on_range = 0.0f;
    float homing_lose_target_range = 0.0f;
    float homing_lock_cone_degrees = 0.0f;
    float homing_max_turn_degrees_per_tick = 0.0f;
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
    float max_turn_degrees_per_tick = 0.0f;
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
    std::uint16_t damage_per_tick = 0;
    std::uint32_t expire_tick = 0;
    std::uint8_t source_code = 0;
    std::uint32_t collision_mask = kCollisionMaskDamageable;
    std::unordered_map<NetId, std::uint32_t> damage_remainder_by_target;
};

struct MovementState {
    float speed_meters_per_second = 0.0f;
    enum class ControllerType : std::uint8_t {
        kNone = 0,
        kGrounded = 1,
        kKinematic = 2,
        kCharacter = 3,
    } controller_type = ControllerType::kNone;
    enum class GroundState : std::uint8_t {
        kAirborne = 0,
        kGrounded = 1,
        kSteepGround = 2,
    } ground_state = GroundState::kAirborne;
    std::uint32_t movement_collider_template_id = 0;
    std::uint32_t movement_collider_id = 0;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    NetId supporting_entity_net_id = 0;
    std::uint32_t supporting_collider_id = 0;
    float max_slope_degrees = 50.0f;
    float step_height = 0.4f;
    float ground_probe_distance = 0.25f;
    float ground_snap_distance = 0.5f;
    glm::vec3 last_queried_position{0.0f};
    bool has_last_queried_position = false;
    bool landed_this_tick = false;
};

// Composable presentation hints only; gameplay systems must not treat them as
// authority. Zero means no active flags, and published bits never change meaning.
inline constexpr std::uint32_t kVisualFlagMoving = 0x00000001u;
inline constexpr std::uint32_t kVisualFlagReloading = 0x00000002u;
inline constexpr std::uint32_t kVisualFlagDead = 0x00000004u;
inline constexpr std::uint32_t kVisualFlagHpUnknown = 0x00000008u;
inline constexpr std::uint32_t kVisualFlagGrounded = 0x00000010u;
inline constexpr std::uint32_t kVisualFlagFalling = 0x00000020u;
inline constexpr std::uint32_t kVisualFlagLanded = 0x00000040u;
inline constexpr std::uint32_t kVisualFlagAiming = 0x00000100u;
inline constexpr std::uint32_t kVisualFlagFiring = 0x00000200u;

struct ReplicationState {
    std::uint16_t animation_state = 0;
    std::uint32_t visual_flags = 0;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_COMPONENTS_H_
