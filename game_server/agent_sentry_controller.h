#ifndef GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_
#define GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_

#include <cstdint>
#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

struct AgentSentryConfig {
    float alert_seconds = 3.0f;
    float forget_seconds = 5.0f;
    float fire_interval_seconds = 1.0f;
    float reload_seconds = 1.0f;
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
