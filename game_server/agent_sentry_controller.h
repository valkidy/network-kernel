#ifndef GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_
#define GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_

#include <cstdint>
#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

struct AgentSentryConfig {
    std::uint32_t alert_ticks = 90;
    std::uint32_t forget_ticks = 150;
    std::uint32_t patrol_rotation_interval_ticks = 30;
    float patrol_rotation_min_degrees = 15.0f;
    float patrol_rotation_max_degrees = 30.0f;
    std::uint8_t weapon_id = kAgentSpammerWeaponId;
    std::uint16_t magazine_size = kAgentSpammerMagazine;
    std::uint16_t animation_idle = kEnemyAnimationIdle;
    std::uint16_t animation_attack = kEnemyAnimationChasing;
};

class AgentSentryController {
public:
    explicit AgentSentryController(AgentSentryConfig config = {});

    void tick(
        KernelHandle* kernel,
        std::vector<Enemy>* enemies,
        float delta_seconds) const;

private:
    AgentSentryConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_
