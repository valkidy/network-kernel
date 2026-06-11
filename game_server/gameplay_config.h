#ifndef GAME_SERVER_GAMEPLAY_CONFIG_H_
#define GAME_SERVER_GAMEPLAY_CONFIG_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game_server/enemy_ai_controller.h"
#include "kernel/public/kernel_types.h"

struct KernelHandle;

namespace network_example::game_server {

inline constexpr std::uint8_t kWeaponRifle = 0;
inline constexpr std::uint8_t kWeaponShotgun = 1;
inline constexpr std::uint8_t kWeaponGrenade = 2;
inline constexpr std::uint8_t kWeaponRocket = 3;
inline constexpr std::uint8_t kWeaponFireFloor = 4;
inline constexpr std::uint8_t kWeaponBeamRifle = 5;
inline constexpr std::uint8_t kWeaponHomingMissile = 6;
inline constexpr std::size_t kWeaponCount = KERNEL_MAX_WEAPONS;

struct EntityHealthDefinition {
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PlayerGameplayDefinition {
    std::uint32_t actor_template_id = 0;
};

struct EnemyGameplayDefinition {
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
    EntityHealthDefinition health{};
    KernelVec3 hitbox_center{};
    KernelVec3 hitbox_half_extents{};
    float move_speed_meters_per_second = 0.0f;
    std::array<std::uint8_t, 4> weapon_slots{};
    std::uint8_t weapon_slot_count = 0;
    std::uint8_t active_weapon_slot = 0;
    std::uint16_t animation_idle = 0;
    std::uint16_t animation_chasing = 0;
    EnemyAiConfig ai{};
};

struct WeaponCatalogConfig {
    std::uint32_t catalog_version = 1;
    std::uint64_t catalog_hash = 0;
    std::array<KernelWeaponMechanicsDefinition, kWeaponCount> definitions{};
    std::array<std::string, kWeaponCount> names{};
    std::array<std::uint8_t, kWeaponCount> projectile_sync_modes{};
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

struct GameServerGameplayConfig {
    WeaponCatalogConfig weapons;
    PlayerGameplayDefinition player;
    EnemyGameplayDefinition enemy;
    std::vector<ActorTemplateConfig> actor_templates;
    ColliderCatalogConfig colliders;
};

struct KernelGameplayCatalogStorage {
    std::vector<KernelProjectileTemplateDefinition> projectile_templates;
    std::vector<KernelColliderTemplateDefinition> collider_templates;
    std::vector<KernelColliderBindingDefinition> collider_bindings;
    KernelGameplayCatalogDefinition definition{};
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
KernelCombatStateDefinition make_enemy_combat_state(
    const GameServerGameplayConfig& config);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAMEPLAY_CONFIG_H_
