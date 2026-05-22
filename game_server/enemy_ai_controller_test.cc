#include "game_server/enemy_ai_controller.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.001f;
}

}  // namespace

int main() {
    network_example::game_server::EnemyAIController controller;
    network_example::game_server::Enemy enemy;
    enemy.net_id = 10;
    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.hp = 50;

    const network_example::game_server::EnemyAiDecision no_player =
        controller.decide(enemy, {});
    assert(no_player.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(no_player.target_player_net_id == 0);
    assert(near(no_player.velocity.x, 0.0f));
    assert(near(no_player.velocity.y, 0.0f));
    assert(near(no_player.velocity.z, 0.0f));

    const std::vector<network_example::game_server::EnemyAiTarget> players{
        {1, KernelVec3{10.0f, 0.0f, 0.0f}},
        {2, KernelVec3{3.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision chasing =
        controller.decide(enemy, players);
    assert(chasing.animation_state ==
           network_example::game_server::kEnemyAnimationChasing);
    assert(chasing.target_player_net_id == 2);
    assert(near(chasing.velocity.x, 2.5f));
    assert(near(chasing.velocity.y, 0.0f));
    assert(near(chasing.velocity.z, 0.0f));

    enemy.hp = 4;
    const network_example::game_server::EnemyAiDecision fleeing =
        controller.decide(enemy, players);
    assert(fleeing.animation_state ==
           network_example::game_server::kEnemyAnimationChasing);
    assert(fleeing.target_player_net_id == 2);
    assert(near(fleeing.velocity.x, -2.5f));
    assert(near(fleeing.velocity.y, 0.0f));
    assert(near(fleeing.velocity.z, 0.0f));

    enemy.hp = 20;
    const network_example::game_server::EnemyAiDecision requesting_help =
        controller.decide(enemy, players);
    assert(requesting_help.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(requesting_help.target_player_net_id == 2);
    assert(near(requesting_help.velocity.x, 0.0f));
    assert(near(requesting_help.velocity.y, 0.0f));
    assert(near(requesting_help.velocity.z, 0.0f));

    enemy.hp = 50;
    const std::vector<network_example::game_server::EnemyAiTarget> far_players{
        {3, KernelVec3{20.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision idle =
        controller.decide(enemy, far_players);
    assert(idle.animation_state == network_example::game_server::kEnemyAnimationIdle);
    assert(idle.target_player_net_id == 0);
    assert(near(idle.velocity.x, 0.0f));

    enemy.position = KernelVec3{3.0f, 0.0f, 0.0f};
    const network_example::game_server::EnemyAiDecision overlapping =
        controller.decide(enemy, players);
    assert(overlapping.animation_state ==
           network_example::game_server::kEnemyAnimationChasing);
    assert(overlapping.target_player_net_id == 2);
    assert(near(overlapping.velocity.x, 0.0f));

    return 0;
}
