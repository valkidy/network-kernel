#ifndef GAME_SERVER_ENEMY_H_
#define GAME_SERVER_ENEMY_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypePlayer = 1;
inline constexpr std::uint16_t kEntityTypeEnemy = 2;

inline constexpr std::uint16_t kEnemyAnimationIdle = 0;
inline constexpr std::uint16_t kEnemyAnimationChasing = 1;
inline constexpr std::uint16_t kEnemyInitialHp = 240;
inline constexpr std::uint8_t kEnemyRifleWeaponId = 0;
inline constexpr std::uint16_t kEnemyRifleMagazine = 3;
inline constexpr std::uint16_t kEnemyRifleReserveAmmo = 6;

struct Enemy {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 patrol_anchor{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t hp = kEnemyInitialHp;
    std::uint16_t max_hp = kEnemyInitialHp;
    std::uint16_t ammo = kEnemyRifleMagazine;
    std::uint16_t reserve_ammo = kEnemyRifleReserveAmmo;
    std::uint16_t animation_state = kEnemyAnimationIdle;
    std::uint32_t target_player_net_id = 0;
    std::uint32_t next_input_seq = 1;
    float fire_cooldown_seconds = 0.0f;
    float reload_remaining_seconds = 0.0f;
    int patrol_direction = 1;
    bool is_reloading = false;
};

struct EnemyAiTarget {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
};

struct EnemyAiDecision {
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    KernelVec3 aim_direction{1.0f, 0.0f, 0.0f};
    std::uint16_t animation_state = kEnemyAnimationIdle;
    std::uint32_t target_player_net_id = 0;
    bool should_fire = false;
    bool should_reload = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_H_
