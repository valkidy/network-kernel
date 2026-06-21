#include "game_server/agent_sentry_controller.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace network_example::game_server {
namespace {

constexpr float kPi = 3.14159265358979323846f;

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

KernelQuat normalized(KernelQuat value) {
    const float length_squared =
        value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    if (length_squared <= 0.0001f * 0.0001f) {
        return KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return KernelQuat{
        value.x * inverse_length,
        value.y * inverse_length,
        value.z * inverse_length,
        value.w * inverse_length,
    };
}

KernelQuat multiply(const KernelQuat& lhs, const KernelQuat& rhs) {
    return normalized(KernelQuat{
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    });
}

KernelQuat yaw_rotation(float yaw_radians) {
    const float half_yaw = yaw_radians * 0.5f;
    return KernelQuat{0.0f, -std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
}

KernelQuat apply_yaw_delta(const KernelQuat& rotation, float delta_degrees) {
    const float yaw_radians = delta_degrees * (kPi / 180.0f);
    return multiply(yaw_rotation(yaw_radians), rotation);
}

bool facing_rotation_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    KernelQuat* out_rotation) {
    if (out_rotation == nullptr) {
        return false;
    }
    KernelVec3 delta = subtract(to, from);
    delta.y = 0.0f;
    if (length_squared(delta) <= 0.0001f * 0.0001f) {
        return false;
    }
    *out_rotation = yaw_rotation(std::atan2(delta.z, delta.x));
    return true;
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
    enemy->sentry.alert_rotation_tick = 0;
    enemy->sentry.alert_rotation_step = 0;
}

bool update_alert_facing(
    const AgentSentryConfig& config,
    Enemy* enemy,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation) {
    if (enemy == nullptr || out_rotation == nullptr ||
        config.alert_rotation_interval_ticks == 0 ||
        config.alert_rotation_degrees <= 0.0f) {
        return false;
    }
    ++enemy->sentry.alert_rotation_tick;
    if (enemy->sentry.alert_rotation_tick < config.alert_rotation_interval_ticks) {
        return false;
    }
    enemy->sentry.alert_rotation_tick = 0;
    ++enemy->sentry.alert_rotation_step;
    std::uint32_t random_bits =
        enemy->net_id * 747796405u + enemy->sentry.alert_rotation_step * 2891336453u;
    random_bits ^= random_bits >> 16u;
    random_bits *= 2246822519u;
    random_bits ^= random_bits >> 13u;
    const bool positive_direction = (random_bits & 1u) == 0u;
    const float delta_degrees =
        positive_direction ? config.alert_rotation_degrees
                           : -config.alert_rotation_degrees;
    *out_rotation = apply_yaw_delta(current_rotation, delta_degrees);
    return true;
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
        KernelQuat desired_rotation = entity_state.rotation;
        bool should_update_rotation = false;

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
            if (enemy.sentry.state == AgentSentryState::kAlert) {
                should_update_rotation = update_alert_facing(
                    config_,
                    &enemy,
                    entity_state.rotation,
                    &desired_rotation);
            }
        } else if (enemy.sentry.state == AgentSentryState::kAttack) {
            if (!has_visible_target) {
                transition_to(&enemy, AgentSentryState::kAlert);
            } else {
                enemy.animation_state = config_.animation_attack;
                KernelVec3 target_position = vision_state.last_known_target_position;
                get_entity_position(kernel, enemy.sentry.target, &target_position);
                should_update_rotation =
                    facing_rotation_toward(
                        enemy.position,
                        target_position,
                        &desired_rotation) ||
                    should_update_rotation;
                request_fire(kernel, config_, &enemy, target_position);
            }
        }

        enemy.target_player_net_id = enemy.sentry.target;
        if (should_update_rotation) {
            Kernel_ServerSetEntityTransform(
                kernel,
                enemy.net_id,
                &entity_state.position,
                &desired_rotation);
        }
        Kernel_ServerSetEntityVelocity(kernel, enemy.net_id, &enemy.velocity);
        Kernel_ServerSetEntityState(kernel, enemy.net_id, enemy.animation_state, 0);
    }
}

}  // namespace network_example::game_server
