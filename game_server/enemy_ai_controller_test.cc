#include "game_server/enemy_ai_controller.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.001f;
}

void assert_zero_velocity(const KernelVec3& velocity) {
    require(near(velocity.x, 0.0f));
    require(near(velocity.y, 0.0f));
    require(near(velocity.z, 0.0f));
}

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float dot(const KernelVec3& lhs, const KernelVec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

void assert_flees_away_from_target(const KernelVec3& velocity,
                                   const KernelVec3& enemy_position,
                                   const KernelVec3& target_position) {
    const KernelVec3 away_from_target = subtract(enemy_position, target_position);
    require(dot(velocity, away_from_target) > 0.0f);
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
    require(no_player.animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(no_player.target_player_net_id == 0);
    require(no_player.should_fire == false);
    require(no_player.should_reload == false);
    require(no_player.velocity.x > 0.0f);
    require(near(no_player.velocity.y, 0.0f));
    require(near(no_player.velocity.z, 0.0f));

    const std::vector<network_example::game_server::EnemyAiTarget> players{
        {1, KernelVec3{10.0f, 0.0f, 0.0f}},
        {2, KernelVec3{3.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision attacking =
        controller.decide(enemy, players);
    require(attacking.animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(attacking.target_player_net_id == 2);
    require(attacking.should_fire);
    require(!attacking.should_reload);
    require(attacking.aim_direction.x > 0.9f);
    assert_zero_velocity(attacking.velocity);

    enemy.ammo = 0;
    enemy.reserve_ammo = 3;
    const network_example::game_server::EnemyAiDecision reloading =
        controller.decide(enemy, players);
    require(reloading.target_player_net_id == 2);
    require(!reloading.should_fire);
    require(reloading.should_reload);
    assert_zero_velocity(reloading.velocity);

    enemy.ammo = 3;
    enemy.hp = 20;
    const network_example::game_server::EnemyAiDecision fleeing =
        controller.decide(enemy, players);
    require(fleeing.animation_state ==
            network_example::game_server::kEnemyAnimationChasing);
    require(fleeing.target_player_net_id == 2);
    require(!fleeing.should_fire);
    require(!fleeing.should_reload);
    require(near(fleeing.velocity.x, -2.5f));
    require(near(fleeing.velocity.y, 0.0f));
    require(near(fleeing.velocity.z, 0.0f));
    assert_flees_away_from_target(
        fleeing.velocity,
        enemy.position,
        players[1].position);

    enemy.position = KernelVec3{3.0f, 0.0f, 4.0f};
    const std::vector<network_example::game_server::EnemyAiTarget> diagonal_players{
        {3, KernelVec3{0.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision diagonal_fleeing =
        controller.decide(enemy, diagonal_players);
    require(diagonal_fleeing.animation_state ==
            network_example::game_server::kEnemyAnimationChasing);
    require(diagonal_fleeing.target_player_net_id == 3);
    assert_flees_away_from_target(
        diagonal_fleeing.velocity,
        enemy.position,
        diagonal_players[0].position);
    require(diagonal_fleeing.velocity.x > 0.0f);
    require(diagonal_fleeing.velocity.z > 0.0f);

    enemy.hp = 100;
    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    const network_example::game_server::EnemyAiDecision holding =
        controller.decide(enemy, players);
    require(holding.animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(holding.target_player_net_id == 2);
    require(!holding.should_fire);
    require(!holding.should_reload);
    assert_zero_velocity(holding.velocity);

    enemy.hp = 20;
    enemy.position = KernelVec3{3.0f, 0.0f, 0.0f};
    const network_example::game_server::EnemyAiDecision overlapping_low_hp =
        controller.decide(enemy, players);
    require(overlapping_low_hp.animation_state ==
            network_example::game_server::kEnemyAnimationChasing);
    require(overlapping_low_hp.target_player_net_id == 2);
    require(!overlapping_low_hp.should_fire);
    require(!overlapping_low_hp.should_reload);
    require(overlapping_low_hp.velocity.x != 0.0f);
    require(near(overlapping_low_hp.velocity.y, 0.0f));
    require(near(overlapping_low_hp.velocity.z, 0.0f));

    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.hp = 240;
    const std::vector<network_example::game_server::EnemyAiTarget> far_players{
        {3, KernelVec3{20.0f, 0.0f, 0.0f}},
    };
    const network_example::game_server::EnemyAiDecision patrolling =
        controller.decide(enemy, far_players);
    require(patrolling.animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(patrolling.target_player_net_id == 0);
    require(patrolling.velocity.x > 0.0f);

    enemy.position = KernelVec3{3.0f, 0.0f, 0.0f};
    const network_example::game_server::EnemyAiDecision overlapping =
        controller.decide(enemy, players);
    require(overlapping.animation_state ==
            network_example::game_server::kEnemyAnimationIdle);
    require(overlapping.target_player_net_id == 2);
    require(overlapping.should_fire);
    assert_zero_velocity(overlapping.velocity);

    return 0;
}
