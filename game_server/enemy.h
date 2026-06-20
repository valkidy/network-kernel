#ifndef GAME_SERVER_ENEMY_H_
#define GAME_SERVER_ENEMY_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypeActor = 1;
inline constexpr std::uint16_t kActorTypePlayer = KernelActorType_Player;
inline constexpr std::uint16_t kActorTypeAgent = KernelActorType_Agent;

inline constexpr std::uint16_t kEnemyAnimationIdle = 0;
inline constexpr std::uint16_t kEnemyAnimationChasing = 1;
inline constexpr std::uint16_t kEnemyInitialHp = 240;
inline constexpr std::uint8_t kAgentSpammerWeaponId = 2;
inline constexpr std::uint16_t kAgentSpammerMagazine = 120;
inline constexpr std::uint16_t kAgentSpammerReserveAmmo = 240;

enum class AgentSentryState : std::uint8_t {
    kIdle = 0,
    kAlert = 1,
    kAttack = 2,
};

struct AgentSentryRuntime {
    std::uint32_t self_id = 0;
    AgentSentryState state = AgentSentryState::kIdle;
    float state_timer = 0.0f;
    float time_without_target = 0.0f;
    std::uint32_t alert_rotation_tick = 0;
    std::uint32_t alert_rotation_step = 0;
    std::uint32_t target = 0;
};

struct Enemy {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 patrol_anchor{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t hp = kEnemyInitialHp;
    std::uint16_t max_hp = kEnemyInitialHp;
    std::uint16_t ammo = kAgentSpammerMagazine;
    std::uint16_t reserve_ammo = kAgentSpammerReserveAmmo;
    std::uint16_t animation_state = kEnemyAnimationIdle;
    std::uint32_t target_player_net_id = 0;
    std::uint32_t next_input_seq = 1;
    float fire_cooldown_seconds = 0.0f;
    float reload_remaining_seconds = 0.0f;
    AgentSentryRuntime sentry{};
    int patrol_direction = 1;
    bool is_reloading = false;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_H_
