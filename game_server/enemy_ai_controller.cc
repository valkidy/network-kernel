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
// The hardcoded YAML describes only the decision tree. Blackboard v1 values
// come from build_context(), for example hasVisibleEnemy, hp01, nearestEnemyId,
// hasAmmo, and isReloading. Stateful gameplay policy such as fire cadence,
// ammo/reload state, and patrol routes stays in GameServer or kernel state.
constexpr const char* kEnemyTreeYaml = R"yaml(
tree: EnemySoldierAI
root:
  type: Composite.Selector
  children:
    - type: Composite.Sequence
      name: Combat
      children:
        - type: Condition.HasVisibleEnemy
        - type: Composite.Selector
          name: CombatDecision
          children:
            - type: Composite.Sequence
              name: FleeCriticalHp
              children:
                - type: Condition.HpBelow
                  value: 0.2
                - type: Action.FleeFromTarget
                  target: nearestEnemyId
            - type: Composite.Sequence
              name: AttackHealthy
              children:
                - type: Condition.HpAbove
                  value: 0.5
                - type: Composite.Selector
                  name: AttackOrReload
                  children:
                    - type: Composite.Sequence
                      name: FireWithAmmo
                      children:
                        - type: Condition.HasAmmo
                        - type: Action.AttackTarget
                          target: nearestEnemyId
                    - type: Action.Reload
            - type: Action.StopMovement
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

KernelVec3 normalized_direction(const KernelVec3& from, const KernelVec3& to) {
    const KernelVec3 delta = subtract(to, from);
    const float distance_squared = length_squared(delta);
    if (distance_squared <= kEpsilon * kEpsilon) {
        return zero_vec3();
    }
    return scale(delta, 1.0f / std::sqrt(distance_squared));
}

KernelVec3 velocity_toward(const KernelVec3& from,
                           const KernelVec3& to,
                           float speed) {
    return scale(normalized_direction(from, to), speed);
}

KernelVec3 patrol_fallback_velocity(const Enemy& enemy, float speed) {
    const float direction = enemy.patrol_direction < 0 ? -1.0f : 1.0f;
    return KernelVec3{direction * speed, 0.0f, 0.0f};
}

KernelVec3 flee_velocity_from_target(const Enemy& enemy,
                                     const EnemyAiTarget& target,
                                     float speed) {
    KernelVec3 velocity = velocity_toward(target.position, enemy.position, speed);
    if (length_squared(velocity) <= kEpsilon * kEpsilon) {
        velocity = patrol_fallback_velocity(enemy, speed);
    }
    return velocity;
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
        std::clamp(
            static_cast<float>(enemy.hp) / static_cast<float>(enemy.max_hp),
            0.0f,
            1.0f));
    context.set_feature("hasAmmo", enemy.ammo > 0 && !enemy.is_reloading);
    context.set_feature("isReloading", enemy.is_reloading);
    context.set_feature(
        "enemyDistance",
        distance_squared == std::numeric_limits<float>::max()
            ? 0.0f
            : std::sqrt(distance_squared));
    context.set_feature("isAtTarget", distance_squared <= kEpsilon * kEpsilon);
    if (target != nullptr) {
        context.set_feature("nearestEnemyId", target->net_id);
    }
    return context;
}

EnemyAiDecision idle_decision(std::uint32_t target_player_net_id = 0) {
    return EnemyAiDecision{
        zero_vec3(),
        KernelVec3{1.0f, 0.0f, 0.0f},
        kEnemyAnimationIdle,
        target_player_net_id,
        false,
        false,
    };
}

EnemyAiDecision patrol_decision(const Enemy& enemy, float patrol_speed) {
    EnemyAiDecision decision = idle_decision();
    decision.velocity = KernelVec3{
        static_cast<float>(enemy.patrol_direction) * patrol_speed,
        0.0f,
        0.0f,
    };
    return decision;
}

EnemyAiDecision execute_command(const network_example::ai::AICommand& command,
                                const Enemy& enemy,
                                const EnemyAiTarget* target,
                                float move_speed,
                                float patrol_speed) {
    const std::uint32_t target_net_id = target != nullptr ? target->net_id : 0;
    if (command.type == "AttackTarget" && target != nullptr) {
        EnemyAiDecision decision = idle_decision(target_net_id);
        decision.aim_direction = normalized_direction(enemy.position, target->position);
        if (length_squared(decision.aim_direction) <= kEpsilon * kEpsilon) {
            decision.aim_direction = KernelVec3{1.0f, 0.0f, 0.0f};
        }
        decision.should_fire = true;
        return decision;
    }
    if (command.type == "FleeFromTarget" && target != nullptr) {
        return EnemyAiDecision{
            flee_velocity_from_target(enemy, *target, move_speed),
            KernelVec3{1.0f, 0.0f, 0.0f},
            kEnemyAnimationChasing,
            target_net_id,
            false,
            false,
        };
    }
    if (command.type == "Reload") {
        EnemyAiDecision decision = idle_decision(target_net_id);
        decision.should_reload = true;
        return decision;
    }
    if (command.type == "Patrol") {
        return patrol_decision(enemy, patrol_speed);
    }
    return idle_decision(target_net_id);
}

void update_patrol_direction(const EnemyAiConfig& config, Enemy* enemy) {
    if (enemy->position.x >= enemy->patrol_anchor.x + config.patrol_half_extent) {
        enemy->patrol_direction = -1;
    } else if (
        enemy->position.x <= enemy->patrol_anchor.x - config.patrol_half_extent) {
        enemy->patrol_direction = 1;
    }
}

void update_timers(const EnemyAiConfig& config, Enemy* enemy, float delta_seconds) {
    enemy->fire_cooldown_seconds =
        std::max(0.0f, enemy->fire_cooldown_seconds - delta_seconds);
    if (!enemy->is_reloading) {
        return;
    }

    enemy->reload_remaining_seconds -= delta_seconds;
    if (enemy->reload_remaining_seconds > 0.0f) {
        return;
    }

    const std::uint16_t missing_ammo =
        static_cast<std::uint16_t>(config.magazine_size - enemy->ammo);
    const std::uint16_t loaded_ammo = std::min(missing_ammo, enemy->reserve_ammo);
    enemy->ammo = static_cast<std::uint16_t>(enemy->ammo + loaded_ammo);
    enemy->reserve_ammo =
        static_cast<std::uint16_t>(enemy->reserve_ammo - loaded_ammo);
    enemy->reload_remaining_seconds = 0.0f;
    enemy->is_reloading = false;
    if (loaded_ammo == 0) {
        enemy->fire_cooldown_seconds =
            std::max(enemy->fire_cooldown_seconds, config.fire_interval_seconds);
    }
}

PlayerInput make_enemy_input(
    const EnemyAiConfig& config,
    Enemy* enemy,
    std::uint32_t buttons,
    const KernelVec3& aim_direction) {
    PlayerInput input{};
    input.input_seq = enemy->next_input_seq++;
    input.buttons = buttons;
    input.selected_weapon = config.weapon_id;
    input.aim_dir = aim_direction;
    return input;
}

void submit_enemy_action(
    KernelHandle* kernel,
    const EnemyAiConfig& config,
    const EnemyAiDecision& decision,
    Enemy* enemy) {
    if (decision.should_reload && !enemy->is_reloading &&
        enemy->ammo < config.magazine_size &&
        enemy->reserve_ammo > 0) {
        PlayerInput reload_input = make_enemy_input(
            config,
            enemy,
            InputButton_Reload,
            decision.aim_direction);
        if (Kernel_ServerSubmitEntityInput(kernel, enemy->net_id, &reload_input)) {
            enemy->is_reloading = true;
            enemy->reload_remaining_seconds = config.reload_seconds;
        }
        return;
    }

    if (!decision.should_fire || enemy->is_reloading ||
        enemy->fire_cooldown_seconds > 0.0f || enemy->ammo == 0) {
        return;
    }

    PlayerInput fire_input = make_enemy_input(
        config,
        enemy,
        InputButton_Fire,
        decision.aim_direction);
    if (Kernel_ServerSubmitEntityInput(kernel, enemy->net_id, &fire_input)) {
        --enemy->ammo;
        enemy->fire_cooldown_seconds = config.fire_interval_seconds;
    }
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
        commands.commands().front(),
        enemy,
        nearest_player,
        config_.move_speed,
        config_.patrol_speed);
}

void EnemyAIController::tick(
    KernelHandle* kernel,
    std::vector<Enemy>* enemies,
    float delta_seconds) const {
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
        update_timers(config_, &enemy, delta_seconds);
        update_patrol_direction(config_, &enemy);
        const EnemyAiDecision decision = decide(enemy, players);
        submit_enemy_action(kernel, config_, decision, &enemy);
        enemy.velocity = decision.velocity;
        enemy.animation_state = decision.animation_state;
        enemy.target_player_net_id = decision.target_player_net_id;

        Kernel_ServerSetEntityVelocity(kernel, enemy.net_id, &enemy.velocity);
        Kernel_ServerSetEntityState(kernel, enemy.net_id, enemy.animation_state, 0);
    }
}

}  // namespace network_example::game_server
