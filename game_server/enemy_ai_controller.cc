#include "game_server/enemy_ai_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "ai/ai_context.h"
#include "ai/ai_tree.h"
#include "ai/yaml_loader.h"

namespace network_example::game_server {
namespace {

constexpr std::uint32_t kMaxQueriedPlayers = 64;
constexpr float kEpsilon = 0.0001f;
constexpr float kEnemyMaxHp = 50.0f;
constexpr const char* kEnemyTreeYaml = R"yaml(
tree: EnemySoldierAI
root:
  type: Composite.Selector
  children:
    - type: Composite.Sequence
      name: Combat
      children:
        - type: Condition.HasVisibleEnemy
        - type: Composite.UtilitySelector
          name: CombatDecision
          children:
            - name: Attack
              score: Score.AttackWhenHealthy
              node:
                type: Action.AttackTarget
                target: nearestEnemyId
            - name: Flee
              score: Score.FleeWhenCriticalHp
              node:
                type: Action.FleeFromTarget
                target: nearestEnemyId
            - name: RequestHelp
              score: Score.RequestHelpWhenInjured
              node:
                type: Action.RequestHelp
                target: nearestEnemyId
    - type: Action.Patrol
)yaml";

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

KernelVec3 normalized_direction(const KernelVec3& from,
                                const KernelVec3& to,
                                float speed) {
    const KernelVec3 delta = subtract(to, from);
    const float distance_squared = length_squared(delta);
    if (distance_squared <= kEpsilon * kEpsilon) {
        return zero_vec3();
    }
    return scale(delta, speed / std::sqrt(distance_squared));
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

network_example::ai::AITreeInstance make_enemy_tree() {
    network_example::ai::YamlLoadResult loaded =
        network_example::ai::load_tree_from_yaml(kEnemyTreeYaml);
    return network_example::ai::AITreeInstance(std::move(loaded.root));
}

const EnemyAiTarget* nearest_target(const Enemy& enemy,
                                    const std::vector<EnemyAiTarget>& players,
                                    float chase_range,
                                    float* out_distance_squared) {
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
    if (out_distance_squared != nullptr) {
        *out_distance_squared = nearest_distance_squared;
    }
    if (nearest_player == nullptr ||
        nearest_distance_squared > chase_range * chase_range) {
        return nullptr;
    }
    return nearest_player;
}

network_example::ai::AIContext build_context(
    const Enemy& enemy,
    const EnemyAiTarget* target,
    float distance_squared) {
    network_example::ai::AIContext context;
    context.set_feature("hasVisibleEnemy", target != nullptr);
    context.set_feature(
        "hp01",
        std::clamp(static_cast<float>(enemy.hp) / kEnemyMaxHp, 0.0f, 1.0f));
    context.set_feature("isAtTarget", distance_squared <= kEpsilon * kEpsilon);
    if (target != nullptr) {
        context.set_feature("nearestEnemyId", target->net_id);
    }
    return context;
}

EnemyAiDecision idle_decision(std::uint32_t target_player_net_id = 0) {
    return EnemyAiDecision{
        zero_vec3(),
        kEnemyAnimationIdle,
        target_player_net_id,
    };
}

EnemyAiDecision execute_command(const network_example::ai::AICommand& command,
                                const Enemy& enemy,
                                const EnemyAiTarget* target,
                                float move_speed) {
    const std::uint32_t target_net_id = target != nullptr ? target->net_id : 0;
    if (command.type == "AttackTarget" && target != nullptr) {
        return EnemyAiDecision{
            normalized_direction(enemy.position, target->position, move_speed),
            kEnemyAnimationChasing,
            target_net_id,
        };
    }
    if (command.type == "FleeFromTarget" && target != nullptr) {
        return EnemyAiDecision{
            normalized_direction(target->position, enemy.position, move_speed),
            kEnemyAnimationChasing,
            target_net_id,
        };
    }
    return idle_decision(target_net_id);
}

}  // namespace

EnemyAIController::EnemyAIController(EnemyAiConfig config) : config_(config) {}

EnemyAiDecision EnemyAIController::decide(
    const Enemy& enemy,
    const std::vector<EnemyAiTarget>& players) const {
    float nearest_distance_squared = std::numeric_limits<float>::max();
    const EnemyAiTarget* nearest_player = nearest_target(
        enemy, players, config_.chase_range, &nearest_distance_squared);

    network_example::ai::AITreeInstance tree = make_enemy_tree();
    if (!tree.has_root()) {
        return idle_decision();
    }

    network_example::ai::AIContext context =
        build_context(enemy, nearest_player, nearest_distance_squared);
    network_example::ai::AICommandBuffer commands;
    tree.tick(context, &commands);
    if (commands.empty()) {
        return idle_decision(nearest_player != nullptr ? nearest_player->net_id : 0);
    }
    return execute_command(
        commands.commands().front(), enemy, nearest_player, config_.move_speed);
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
        enemy.hp = state.hp;
        const EnemyAiDecision decision = decide(enemy, players);
        enemy.velocity = decision.velocity;
        enemy.animation_state = decision.animation_state;
        enemy.target_player_net_id = decision.target_player_net_id;

        Kernel_ServerSetEntityVelocity(kernel, enemy.net_id, &enemy.velocity);
        Kernel_ServerSetEntityState(kernel, enemy.net_id, enemy.animation_state, 0);
    }
}

}  // namespace network_example::game_server
