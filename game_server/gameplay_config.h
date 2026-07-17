#ifndef GAME_SERVER_GAMEPLAY_CONFIG_H_
#define GAME_SERVER_GAMEPLAY_CONFIG_H_

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
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
    std::array<std::uint32_t, KERNEL_MAX_WEAPON_SLOTS> weapon_ids{};
    std::uint8_t weapon_slot_count = 0;
    std::uint8_t active_weapon_slot = 0;
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
};

using EntityTemplateConfig = ActorTemplateConfig;

struct WeaponCatalogConfig {
    std::uint32_t catalog_version = 2;
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

struct ProjectileTemplateConfig {
    std::string name;
    KernelProjectileTemplateDefinition definition{};
    std::string impact_projectile_template_ref;
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
    std::vector<ProjectileTemplateConfig> projectile_templates;
    StaticCollisionSceneConfig static_collision_scene;
};

struct KernelGameplayCatalogStorage {
    std::vector<KernelEntityTemplateDefinition> entity_templates;
    std::vector<KernelActorTemplateDefinition> actor_templates;
    std::vector<KernelProjectileTemplateDefinition> projectile_templates;
    std::vector<KernelColliderTemplateDefinition> collider_templates;
    std::vector<KernelColliderBindingDefinition> collider_bindings;
    std::vector<KernelActionTemplateDefinition> action_templates;
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
