#ifndef GAME_SERVER_ENEMY_AI_CONTROLLER_H_
#define GAME_SERVER_ENEMY_AI_CONTROLLER_H_

#include <vector>

#include "game_server/enemy.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

struct EnemyAiConfig {
    float chase_range = 12.0f;
    float move_speed = 2.5f;
};

class EnemyAIController {
public:
    explicit EnemyAIController(EnemyAiConfig config = {});

    EnemyAiDecision decide(
        const Enemy& enemy,
        const std::vector<EnemyAiTarget>& players) const;
    void tick(KernelHandle* kernel, std::vector<Enemy>* enemies) const;

private:
    EnemyAiConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ENEMY_AI_CONTROLLER_H_
