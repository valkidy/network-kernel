#ifndef GAME_SERVER_AGENT_RUNTIME_H_
#define GAME_SERVER_AGENT_RUNTIME_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypeActor = 1;
inline constexpr std::uint16_t kActorTypePlayer = KernelActorType_Player;
inline constexpr std::uint16_t kActorTypeAgent = KernelActorType_Agent;

inline constexpr std::uint16_t kAgentAnimationIdle = 0;
inline constexpr std::uint16_t kAgentAnimationChasing = 1;
inline constexpr std::uint16_t kAgentInitialHp = 240;
inline constexpr std::uint8_t kAgentSpammerWeaponId = 2;

enum class AgentSentryState : std::uint8_t {
    kIdle = 0,
    kAlert = 1,
    kAttack = 2,
};

struct AgentSentryRuntimeState {
    std::uint32_t self_id = 0;
    AgentSentryState state = AgentSentryState::kIdle;
    std::uint32_t state_ticks = 0;
    std::uint32_t lost_target_ticks = 0;
    std::uint32_t patrol_rotation_tick = 0;
    std::uint32_t patrol_rotation_step = 0;
    std::uint32_t target = 0;
};

struct AgentRuntimeState {
    std::uint32_t net_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 patrol_anchor{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t hp = kAgentInitialHp;
    std::uint16_t max_hp = kAgentInitialHp;
    std::uint16_t animation_state = kAgentAnimationIdle;
    std::uint32_t target_player_net_id = 0;
    std::uint32_t next_input_seq = 1;
    AgentSentryRuntimeState sentry{};
    int patrol_direction = 1;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_H_
