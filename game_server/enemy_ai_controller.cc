#include "game_server/enemy_ai_controller.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace network_example::game_server {
namespace {

constexpr std::uint32_t kMaxQueriedPlayers = 64;
constexpr float kEpsilon = 0.0001f;

KernelVec3 zero_vec3() {
    return KernelVec3{0.0f, 0.0f, 0.0f};
}

KernelVec3 subtract(const KernelVec3& lhs, const KernelVec3& rhs) {
    return KernelVec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float length_squared(const KernelVec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

KernelVec3 scale(const KernelVec3& value, float scalar) {
    return KernelVec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

std::vector<EnemyAiTarget> query_players(KernelHandle* kernel) {
    std::array<KernelServerEntityState, kMaxQueriedPlayers> states{};
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }

    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        kEntityTypePlayer,
        states.data(),
        static_cast<std::uint32_t>(states.size()));

    std::vector<EnemyAiTarget> players;
    players.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!states[index].valid) {
            continue;
        }
        players.push_back(EnemyAiTarget{states[index].net_id, states[index].position});
    }
    return players;
}

}  // namespace

EnemyAIController::EnemyAIController(EnemyAiConfig config) : config_(config) {}

EnemyAiDecision EnemyAIController::decide(
    const Enemy& enemy,
    const std::vector<EnemyAiTarget>& players) const {
    const EnemyAiTarget* nearest_player = nullptr;
    float nearest_distance_squared = std::numeric_limits<float>::max();
    for (const EnemyAiTarget& player : players) {
        const float distance_squared =
            length_squared(subtract(player.position, enemy.position));
        if (distance_squared < nearest_distance_squared) {
            nearest_distance_squared = distance_squared;
            nearest_player = &player;
        }
    }

    if (nearest_player == nullptr ||
        nearest_distance_squared > config_.chase_range * config_.chase_range) {
        return EnemyAiDecision{zero_vec3(), kEnemyAnimationIdle, 0};
    }

    const KernelVec3 to_player = subtract(nearest_player->position, enemy.position);
    const float distance = std::sqrt(nearest_distance_squared);
    KernelVec3 velocity = zero_vec3();
    if (distance > kEpsilon) {
        velocity = scale(to_player, config_.move_speed / distance);
    }
    return EnemyAiDecision{
        velocity,
        kEnemyAnimationChasing,
        nearest_player->net_id,
    };
}

void EnemyAIController::tick(KernelHandle* kernel, std::vector<Enemy>* enemies) const {
    if (kernel == nullptr || enemies == nullptr) {
        return;
    }

    const std::vector<EnemyAiTarget> players = query_players(kernel);
    for (Enemy& enemy : *enemies) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(KernelServerEntityState);
        if (!Kernel_ServerGetEntityState(kernel, enemy.net_id, &state) || !state.valid) {
            continue;
        }

        enemy.position = state.position;
        const EnemyAiDecision decision = decide(enemy, players);
        enemy.velocity = decision.velocity;
        enemy.animation_state = decision.animation_state;
        enemy.target_player_net_id = decision.target_player_net_id;

        Kernel_ServerSetEntityVelocity(kernel, enemy.net_id, &enemy.velocity);
        Kernel_ServerSetEntityState(kernel, enemy.net_id, enemy.animation_state, 0);
    }
}

}  // namespace network_example::game_server
