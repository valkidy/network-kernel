#ifndef GAME_SERVER_ENEMY_AI_CONTROLLER_H_
#define GAME_SERVER_ENEMY_AI_CONTROLLER_H_

#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

struct EnemyAiConfig {
    float chase_range = 12.0f;
    float move_speed = 2.5f;
    float patrol_speed = 1.25f;
    float patrol_half_extent = 2.0f;
    float fire_interval_seconds = 1.0f;
    float reload_seconds = 1.0f;
};

class EnemyAIController {
public:
    explicit EnemyAIController(EnemyAiConfig config = {});

    EnemyAiDecision decide(
        const Enemy& enemy,
        const std::vector<EnemyAiTarget>& players) const;
    void tick(
        KernelHandle* kernel,
        std::vector<Enemy>* enemies,
        float delta_seconds) const;

private:
    EnemyAiConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_AI_CONTROLLER_H_
