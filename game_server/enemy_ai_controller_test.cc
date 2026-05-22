#include "game_server/enemy_ai_controller.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.001f;
}

void assert_zero_velocity(const KernelVec3& velocity) {
    assert(near(velocity.x, 0.0f));
    assert(near(velocity.y, 0.0f));
    assert(near(velocity.z, 0.0f));
}

}  // namespace

int main() {
    network_example::game_server::EnemyAIController controller;
    network_example::game_server::Enemy enemy;
    enemy.net_id = 10;
    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.patrol_anchor = enemy.position;
    enemy.hp = 240;
    enemy.max_hp = 240;

    const network_example::game_server::EnemyAiDecision no_player =
        controller.decide(enemy, {});
    assert(no_player.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(no_player.target_player_net_id == 0);
    assert(no_player.should_fire == false);
    assert(no_player.should_reload == false);
    assert(no_player.velocity.x > 0.0f);
    assert(near(no_player.velocity.y, 0.0f));
    assert(near(no_player.velocity.z, 0.0f));

    const std::vector<network_example::game_server::EnemyAiTarget> players{
        {1, KernelVec3{10.0f, 0.0f, 0.0f}},
        {2, KernelVec3{3.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision attacking =
        controller.decide(enemy, players);
    assert(attacking.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(attacking.target_player_net_id == 2);
    assert(attacking.should_fire);
    assert(!attacking.should_reload);
    assert(attacking.aim_direction.x > 0.9f);
    assert_zero_velocity(attacking.velocity);

    enemy.ammo = 0;
    enemy.reserve_ammo = 3;
    const network_example::game_server::EnemyAiDecision reloading =
        controller.decide(enemy, players);
    assert(reloading.target_player_net_id == 2);
    assert(!reloading.should_fire);
    assert(reloading.should_reload);
    assert_zero_velocity(reloading.velocity);

    enemy.ammo = 3;
    enemy.hp = 20;
    const network_example::game_server::EnemyAiDecision fleeing =
        controller.decide(enemy, players);
    assert(fleeing.animation_state ==
           network_example::game_server::kEnemyAnimationChasing);
    assert(fleeing.target_player_net_id == 2);
    assert(!fleeing.should_fire);
    assert(!fleeing.should_reload);
    assert(near(fleeing.velocity.x, -2.5f));
    assert(near(fleeing.velocity.y, 0.0f));
    assert(near(fleeing.velocity.z, 0.0f));

    enemy.hp = 100;
    const network_example::game_server::EnemyAiDecision holding =
        controller.decide(enemy, players);
    assert(holding.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(holding.target_player_net_id == 2);
    assert(!holding.should_fire);
    assert(!holding.should_reload);
    assert_zero_velocity(holding.velocity);

    enemy.hp = 240;
    const std::vector<network_example::game_server::EnemyAiTarget> far_players{
        {3, KernelVec3{20.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision patrolling =
        controller.decide(enemy, far_players);
    assert(patrolling.animation_state == network_example::game_server::kEnemyAnimationIdle);
    assert(patrolling.target_player_net_id == 0);
    assert(patrolling.velocity.x > 0.0f);

    enemy.position = KernelVec3{3.0f, 0.0f, 0.0f};
    const network_example::game_server::EnemyAiDecision overlapping =
        controller.decide(enemy, players);
    assert(overlapping.animation_state ==
           network_example::game_server::kEnemyAnimationIdle);
    assert(overlapping.target_player_net_id == 2);
    assert(overlapping.should_fire);
    assert_zero_velocity(overlapping.velocity);

    return 0;
}
