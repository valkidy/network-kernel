#ifndef GAME_SERVER_GAMEPLAY_CONFIG_H_
#define GAME_SERVER_GAMEPLAY_CONFIG_H_

#include <array>
#include <string>
#include <vector>

#include "game_server/enemy_ai_controller.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint8_t kWeaponRifle = 0;
inline constexpr std::uint8_t kWeaponShotgun = 1;
inline constexpr std::uint8_t kWeaponGrenade = 2;
inline constexpr std::uint8_t kWeaponRocket = 3;
inline constexpr std::uint8_t kWeaponFireFloor = 4;
inline constexpr std::size_t kWeaponCount = KERNEL_MAX_WEAPONS;

struct EntityHealthDefinition {
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PlayerGameplayDefinition {
    std::uint16_t entity_type = kEntityTypePlayer;
    EntityHealthDefinition health{100, 100};
    KernelVec3 hitbox_center{0.0f, 0.9f, 0.0f};
    KernelVec3 hitbox_half_extents{0.35f, 0.9f, 0.35f};
    float move_speed_meters_per_second = 5.0f;
};

struct EnemyGameplayDefinition {
    std::uint16_t entity_type = kEntityTypeEnemy;
    EntityHealthDefinition health{kEnemyInitialHp, kEnemyInitialHp};
    KernelVec3 spawn_position{6.0f, 0.0f, 0.0f};
    KernelVec3 hitbox_center{0.0f, 0.8f, 0.0f};
    KernelVec3 hitbox_half_extents{0.4f, 0.8f, 0.4f};
    std::uint8_t weapon_id = kEnemyRocketWeaponId;
    std::uint16_t animation_idle = kEnemyAnimationIdle;
    std::uint16_t animation_chasing = kEnemyAnimationChasing;
    EnemyAiConfig ai{};
};

struct WeaponCatalogConfig {
    std::array<KernelWeaponMechanicsDefinition, kWeaponCount> definitions{};
    std::array<std::string, kWeaponCount> names{};
};

struct GameServerGameplayConfig {
    WeaponCatalogConfig weapons;
    PlayerGameplayDefinition player;
    EnemyGameplayDefinition enemy;
};

GameServerGameplayConfig default_game_server_gameplay_config();
GameServerGameplayConfig load_gameplay_config_from_weapon_template_directory(
    const std::string& directory);
std::vector<std::string> validate_gameplay_config(
    const GameServerGameplayConfig& config);

KernelCombatStateDefinition make_player_combat_state(
    const GameServerGameplayConfig& config);
KernelCombatStateDefinition make_enemy_combat_state(
    const GameServerGameplayConfig& config);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAMEPLAY_CONFIG_H_
