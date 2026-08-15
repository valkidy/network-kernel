#ifndef GAME_SERVER_GAMEPLAY_CONFIG_H_
#define GAME_SERVER_GAMEPLAY_CONFIG_H_

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "game_server/agent_sentry_controller.h"
#include "kernel/public/kernel_types.h"

struct KernelHandle;

namespace network_example::game_server {

inline constexpr std::uint8_t kWeaponRifle = 0;
inline constexpr std::uint8_t kWeaponShotgun = 1;
inline constexpr std::uint8_t kWeaponSpammer = 2;
inline constexpr std::uint8_t kWeaponRocket = 3;
inline constexpr std::uint8_t kWeaponFireFloor = 4;
inline constexpr std::uint8_t kWeaponBeamRifle = 5;
inline constexpr std::uint8_t kWeaponHomingMissile = 6;
inline constexpr std::uint8_t kWeaponGrenade = 7;
inline constexpr std::size_t kWeaponIdCount = 256;
inline constexpr std::uint32_t kDefaultDirectorEntityTemplateId = 100;

struct EntityHealthDefinition {
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PlayerGameplayDefinition {
    std::uint32_t actor_template_id = 0;
};

struct AgentSpawnDefinition {
    std::uint32_t actor_template_id = 0;
    KernelVec3 spawn_position{6.0f, 0.0f, 0.0f};
    std::uint32_t spawn_count = 1;
    float spawn_radius = 0.0f;
    std::uint32_t spawn_seed = 1;
    bool override_director_spawn = false;
};

struct TriggerBindingConfig {
    std::string action_graph_ref;
    std::vector<std::pair<std::string, std::string>> parameters;
};

struct InventorySlotConfig {
    std::string item_template_ref;
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
};

struct SkeletonManifestBoneConfig {
    std::string name;
    std::int32_t parent_index = -1;
};

struct SkeletonAssetConfig {
    std::uint32_t skeleton_asset_id = 0;
    std::string name;
    std::uint64_t content_hash = 0;
    std::string manifest_reference;
    std::string runtime_reference;
    std::vector<std::uint8_t> runtime_skeleton;
    std::vector<SkeletonManifestBoneConfig> bones;
};

struct SkeletonLegConfig {
    std::string id;
    std::string hip_bone;
    std::string knee_bone;
    std::string foot_bone;
    std::uint32_t hip_bone_index = 0;
    std::uint32_t knee_bone_index = 0;
    std::uint32_t foot_bone_index = 0;
    std::uint32_t gait_group = 0;
    KernelVec3 pole_local{};
    KernelVec3 mid_axis_local{0.0f, 0.0f, 1.0f};
    float step_height_meters = 0.0f;
    float max_reach_ratio = 0.0f;
};

struct SkeletonBindingConfig {
    bool enabled = false;
    std::uint32_t skeleton_asset_id = 0;
    std::uint64_t content_hash = 0;
    std::uint32_t bone_count = 0;
    std::string runtime_asset;
    std::string source_manifest;
    std::string root_bone;
    std::string body_bone;
    std::uint32_t root_bone_index = 0;
    std::uint32_t body_bone_index = 0;
    std::string locomotion_type;
    std::string forward_axis;
    float input_deadzone = 0.0f;
    float step_threshold_meters = 0.0f;
    std::uint32_t step_duration_ticks = 0;
    std::uint32_t max_swinging_legs = 0;
    float body_follow_speed = 0.0f;
    float slope_alignment = 0.0f;
    float stance_crouch_meters = 0.0f;
    std::uint8_t foothold_query_type = KernelFootholdQueryType_None;
    float foothold_query_start_height_meters = 0.0f;
    float foothold_query_distance_meters = 0.0f;
    std::vector<KernelVec2> foothold_candidate_offsets;
    std::vector<SkeletonLegConfig> legs;
    std::vector<std::uint32_t> processing_order;
};

struct ActorTemplateConfig {
    std::uint32_t actor_template_id = 0;
    std::string name;
    std::uint16_t entity_type = 0;
    std::uint16_t actor_type = 0;
    bool server_only = false;
    KernelVec3 transform_position{};
    std::uint32_t collider_template_id = 0;
    EntityHealthDefinition health{};
    KernelVec3 hitbox_center{};
    KernelVec3 hitbox_half_extents{};
    float move_speed_meters_per_second = 0.0f;
    std::uint8_t movement_controller_type = KernelMovementControllerType_None;
    std::uint32_t movement_collider_template_id = 0;
    KernelVec3 movement_gravity{0.0f, -9.81f, 0.0f};
    float movement_max_slope_degrees = 50.0f;
    float movement_step_height = 0.4f;
    float movement_ground_probe_distance = 0.25f;
    float movement_ground_snap_distance = 0.5f;
    float movement_max_yaw_degrees_per_second = 0.0f;
    float impulse_resistance = 0.0f;
    // KERNEL_MOVEMENT_LAYER_* bits; 0 keeps the engine default.
    std::uint32_t movement_collision_mask = 0u;
    std::array<std::uint32_t, KERNEL_MAX_WEAPON_SLOTS> weapon_ids{};
    std::uint8_t weapon_slot_count = 0;
    std::uint8_t active_weapon_slot = 0;
    std::uint16_t inventory_slot_capacity = 0;
    std::vector<InventorySlotConfig> inventory_slots;
    std::uint16_t animation_idle = 0;
    std::uint16_t animation_chasing = 0;
    AgentSentryConfig sentry{};
    KernelAgentVisionConfig vision{};
    std::uint32_t ai_controller_type = KernelAiControllerType_None;
    std::uint32_t ai_tick_interval = 1;
    std::uint32_t director_spawn_target_count = 0;
    std::uint32_t director_spawn_entity_template_id = 0;
    std::uint32_t director_spawn_actor_template_id = 0;
    std::string director_spawn_entity_template_ref;
    KernelVec3 director_spawn_position{};
    float director_spawn_radius = 0.0f;
    std::uint32_t director_spawn_seed = 1;
    TriggerBindingConfig activated_trigger;
    TriggerBindingConfig collision_trigger;
    std::uint32_t collision_trigger_mask = KERNEL_COLLISION_MASK_NONE;
    TriggerBindingConfig health_depleted_trigger;
    TriggerBindingConfig destroy_entity_trigger;
    KernelPropDefinition prop{};
    SkeletonBindingConfig skeleton;
};

using EntityTemplateConfig = ActorTemplateConfig;

struct WeaponCatalogConfig {
    std::uint32_t catalog_version = 8;
    std::uint64_t catalog_hash = 0;
    std::array<bool, kWeaponIdCount> configured{};
    std::array<KernelWeaponMechanicsDefinition, kWeaponIdCount> definitions{};
    std::array<std::string, kWeaponIdCount> names{};
    std::array<std::uint8_t, kWeaponIdCount> projectile_sync_modes{};
    std::array<std::uint32_t, kWeaponIdCount> collider_template_ids{};
};

struct ActionTemplateConfig {
    std::string name;
    KernelActionTemplateDefinition definition{};
    std::string source_path;
    std::uint32_t source_kind = KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN;
    std::uint32_t commit_interval_line = 0;
    std::uint32_t commit_interval_column = 0;
};

struct ColliderTemplateConfig {
    std::string name;
    KernelColliderTemplateDefinition definition{};
};

struct ColliderBindingConfig {
    KernelColliderBindingDefinition definition{};
};

struct ColliderCatalogConfig {
    std::vector<ColliderTemplateConfig> templates;
    std::vector<ColliderBindingConfig> bindings;
};

struct ActionGraphParameterConfig {
    std::string name;
    bool has_default = false;
    std::string default_value;
    std::optional<KernelVec3> default_vec3;
};

struct ActionGraphActionConfig {
    std::string action_type;
    std::string projectile_template_parameter;
    std::string entity_template_parameter;
    std::string position_parameter;
    std::string direction_parameter;
    std::string owner_parameter;
    std::string target_parameter;
    std::string amount_parameter;
    std::string strength_parameter;
    std::string status_parameter;
    std::string operation_parameter;
    std::string value_parameter;
    std::uint32_t collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    std::string item_template_ref;
    std::uint32_t quantity = 0;
    std::uint32_t condition_type = KernelActionConditionType_Always;
};

struct ActionGraphTemplateConfig {
    std::string id;
    std::vector<ActionGraphParameterConfig> parameters;
    std::vector<ActionGraphActionConfig> actions;
};

struct StatusEffectTemplateConfig {
    std::uint32_t status_effect_id = 0;
    std::string name;
    std::uint32_t channel_id = 0;
    std::string channel_name;
    std::uint32_t duration_ticks = 0;
    std::uint32_t interval_ticks = 0;
    std::uint8_t replacement_policy =
        KernelStatusEffectReplacementPolicy_Replace;
    std::uint16_t max_stacks = 1u;
    bool refresh_on_stack = false;
    TriggerBindingConfig on_apply_trigger;
    TriggerBindingConfig on_tick_trigger;
    TriggerBindingConfig on_expire_trigger;
};

struct ItemTemplateConfig {
    std::string name;
    std::string entity_template_ref;
    std::string charge_field_ref;
    TriggerBindingConfig item_used_trigger;
    KernelItemTemplateDefinition definition{};
};

using ProjectileTriggerBindingConfig = TriggerBindingConfig;

struct ProjectileTemplateConfig {
    std::string name;
    KernelProjectileTemplateDefinition definition{};
    ProjectileTriggerBindingConfig projectile_impact_trigger;
    ProjectileTriggerBindingConfig expired_trigger;
};

struct PropPopulationRuleConfig {
    std::string name;
    KernelPropPopulationRuleDefinition definition{};
};

struct StaticCollisionSceneConfig {
    std::string entry_path;
    std::uint32_t scene_id = 0;
    std::uint32_t collider_id = 0;
    std::uint32_t collision_layer = 0;
};

struct GameServerGameplayConfig {
    WeaponCatalogConfig weapons;
    std::vector<ActionTemplateConfig> action_templates;
    PlayerGameplayDefinition player;
    AgentSpawnDefinition agent;
    std::vector<EntityTemplateConfig> entity_templates;
    std::vector<ActorTemplateConfig> actor_templates;
    ColliderCatalogConfig colliders;
    std::vector<ActionGraphTemplateConfig> action_graph_templates;
    std::vector<StatusEffectTemplateConfig> status_effect_templates;
    std::vector<ItemTemplateConfig> item_templates;
    std::vector<ProjectileTemplateConfig> projectile_templates;
    std::vector<PropPopulationRuleConfig> prop_population_rules;
    std::vector<SkeletonAssetConfig> skeleton_assets;
    StaticCollisionSceneConfig static_collision_scene;
};

struct KernelGameplayCatalogStorage {
    std::vector<KernelEntityTemplateDefinition> entity_templates;
    std::vector<KernelActorTemplateDefinition> actor_templates;
    std::vector<KernelProjectileTemplateDefinition> projectile_templates;
    std::vector<KernelColliderTemplateDefinition> collider_templates;
    std::vector<KernelColliderBindingDefinition> collider_bindings;
    std::vector<KernelActionTemplateDefinition> action_templates;
    std::vector<KernelItemTemplateDefinition> item_templates;
    std::vector<KernelPropPopulationRuleDefinition> prop_population_rules;
    std::vector<KernelStatusEffectDefinition> status_effects;
    std::vector<std::vector<std::uint8_t>> skeleton_asset_bytes;
    std::vector<KernelSkeletonAssetDefinition> skeleton_assets;
    KernelGameplayCatalogDefinition definition{};
};

struct DataLoadError : public std::runtime_error {
    DataLoadError(
        std::uint32_t error_code,
        std::string diagnostic,
        std::string path = {},
        std::string field = {},
        std::uint32_t source_kind = KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN,
        std::uint32_t template_kind = KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN,
        std::uint32_t template_id = 0,
        std::uint32_t field_id = 0,
        std::int32_t line = -1,
        std::int32_t column = -1);

    std::uint32_t error_code = KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN;
    std::string path;
    std::string field;
    std::uint32_t source_kind = KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN;
    std::uint32_t template_kind = KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN;
    std::uint32_t template_id = 0;
    std::uint32_t field_id = 0;
    std::int32_t line = -1;
    std::int32_t column = -1;
};

GameServerGameplayConfig default_game_server_gameplay_config();
std::uint64_t compute_gameplay_catalog_hash(const WeaponCatalogConfig& weapons);
std::uint64_t compute_gameplay_catalog_hash(
    const GameServerGameplayConfig& config);
GameServerGameplayConfig load_gameplay_config_from_weapon_template_directory(
    const std::string& directory);
GameServerGameplayConfig load_gameplay_config_from_catalog_file(
    const std::string& path);
GameServerGameplayConfig load_gameplay_config_from_bundle_memory(
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const std::string& entry_path);
std::vector<std::uint8_t> load_gameplay_bundle_entry_bytes(
    const std::uint8_t* bundle_bytes,
    std::uint32_t bundle_size,
    const std::string& entry_path);
KernelGameplayCatalogStorage build_kernel_gameplay_catalog(
    const GameServerGameplayConfig& config);
bool load_kernel_gameplay_catalog(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config);
std::vector<std::string> validate_gameplay_config(
    const GameServerGameplayConfig& config);
const ActorTemplateConfig* find_actor_template(
    const GameServerGameplayConfig& config,
    std::uint32_t actor_template_id);
std::uint8_t active_weapon_id(const ActorTemplateConfig& actor_template);

KernelCombatStateDefinition make_player_combat_state(
    const GameServerGameplayConfig& config);
KernelCombatStateDefinition make_agent_combat_state(
    const GameServerGameplayConfig& config);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAMEPLAY_CONFIG_H_
