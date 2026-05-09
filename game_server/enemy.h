#ifndef GAME_SERVER_ENEMY_H_
#define GAME_SERVER_ENEMY_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypePlayer = 1;
inline constexpr std::uint16_t kEntityTypeEnemy = 2;

inline constexpr std::uint16_t kEnemyAnimationIdle = 0;
inline constexpr std::uint16_t kEnemyAnimationChasing = 1;

struct Enemy {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t animation_state = kEnemyAnimationIdle;
    std::uint32_t target_player_net_id = 0;
};

struct EnemyAiTarget {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
};

struct EnemyAiDecision {
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t animation_state = kEnemyAnimationIdle;
    std::uint32_t target_player_net_id = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_H_
