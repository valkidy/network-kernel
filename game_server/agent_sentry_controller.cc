#include "game_server/agent_sentry_controller.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace network_example::game_server {
namespace {

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
    if (distance_squared <= 0.0001f * 0.0001f) {
        return KernelVec3{1.0f, 0.0f, 0.0f};
    }
    return scale(delta, 1.0f / std::sqrt(distance_squared));
}

void update_timers(const AgentSentryConfig& config, Enemy* enemy, float delta_seconds) {
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
}

bool query_vision_state(
    KernelHandle* kernel,
    std::uint32_t agent_net_id,
    KernelVisionStateView* out_state) {
    if (kernel == nullptr || out_state == nullptr) {
        return false;
    }
    KernelVisionStateQuery query{};
    query.struct_size = sizeof(query);
    query.agent_net_id = agent_net_id;
    out_state->struct_size = sizeof(*out_state);
    return Kernel_QueryVisionState(kernel, &query, out_state, 1) == 1 &&
           out_state->valid != 0u;
}

bool get_entity_position(
    KernelHandle* kernel,
    std::uint32_t net_id,
    KernelVec3* out_position) {
    if (kernel == nullptr || out_position == nullptr || net_id == 0) {
        return false;
    }
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state) || state.valid == 0u) {
        return false;
    }
    *out_position = state.position;
    return true;
}

void request_fire(
    KernelHandle* kernel,
    const AgentSentryConfig& config,
    Enemy* enemy,
    const KernelVec3& target_position) {
    if (enemy->is_reloading || enemy->fire_cooldown_seconds > 0.0f ||
        enemy->ammo == 0) {
        return;
    }

    PlayerInput input{};
    input.input_seq = enemy->next_input_seq++;
    input.buttons = InputButton_Fire;
    input.selected_weapon = config.weapon_id;
    input.aim_dir = normalized_direction(enemy->position, target_position);
    if (Kernel_ServerSubmitEntityInput(kernel, enemy->net_id, &input)) {
        --enemy->ammo;
        enemy->fire_cooldown_seconds = config.fire_interval_seconds;
    }
}

void transition_to(Enemy* enemy, AgentSentryState state) {
    if (enemy->sentry.state == state) {
        return;
    }
    enemy->sentry.state = state;
    enemy->sentry.state_timer = 0.0f;
    enemy->sentry.time_without_target = 0.0f;
}

}  // namespace

AgentSentryController::AgentSentryController(AgentSentryConfig config)
    : config_(config) {}

void AgentSentryController::tick(
    KernelHandle* kernel,
    std::vector<Enemy>* enemies,
    float delta_seconds) const {
    if (kernel == nullptr || enemies == nullptr) {
        return;
    }

    for (Enemy& enemy : *enemies) {
        KernelServerEntityState entity_state{};
        entity_state.struct_size = sizeof(entity_state);
        if (!Kernel_ServerGetEntityState(kernel, enemy.net_id, &entity_state) ||
            entity_state.valid == 0u) {
            continue;
        }

        enemy.position = entity_state.position;
        enemy.hp = entity_state.hp;
        enemy.velocity = zero_vec3();
        enemy.animation_state = config_.animation_idle;
        enemy.sentry.self_id = enemy.net_id;
        update_timers(config_, &enemy, delta_seconds);

        KernelVisionStateView vision_state{};
        const bool has_vision_state =
            query_vision_state(kernel, enemy.net_id, &vision_state);
        const bool has_visible_target =
            has_vision_state && vision_state.current_target_candidate != 0;

        if (has_visible_target) {
            enemy.sentry.target = vision_state.current_target_candidate;
            enemy.sentry.time_without_target = 0.0f;
        } else {
            enemy.sentry.time_without_target += delta_seconds;
        }

        if (enemy.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                transition_to(&enemy, AgentSentryState::kAlert);
            }
        } else if (enemy.sentry.state == AgentSentryState::kAlert) {
            if (has_visible_target) {
                enemy.sentry.state_timer += delta_seconds;
                if (enemy.sentry.state_timer >= config_.alert_seconds) {
                    transition_to(&enemy, AgentSentryState::kAttack);
                }
            } else if (enemy.sentry.time_without_target >= config_.forget_seconds) {
                enemy.sentry.target = 0;
                transition_to(&enemy, AgentSentryState::kIdle);
            }
        } else if (enemy.sentry.state == AgentSentryState::kAttack) {
            if (!has_visible_target) {
                transition_to(&enemy, AgentSentryState::kAlert);
            } else {
                enemy.animation_state = config_.animation_attack;
                KernelVec3 target_position = vision_state.last_known_target_position;
                get_entity_position(kernel, enemy.sentry.target, &target_position);
                request_fire(kernel, config_, &enemy, target_position);
            }
        }

        enemy.target_player_net_id = enemy.sentry.target;
        Kernel_ServerSetEntityVelocity(kernel, enemy.net_id, &enemy.velocity);
        Kernel_ServerSetEntityState(kernel, enemy.net_id, enemy.animation_state, 0);
    }
}

}  // namespace network_example::game_server
