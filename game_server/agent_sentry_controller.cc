#include "game_server/agent_sentry_controller.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ai_intent.h"
#include "game_server/actor_intent_executor.h"
#include "game_server/ai_perception_adapter.h"
#include "kernel/src/kernel_api_internal.h"

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

bool facing_rotation_from_vision_toward(
    const KernelVec3& from,
    const KernelVec3& to,
    const KernelVec3& current_forward,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation) {
    if (out_rotation == nullptr) {
        return false;
    }
    KernelVec3 target_delta = subtract(to, from);
    target_delta.y = 0.0f;
    KernelVec3 forward = current_forward;
    forward.y = 0.0f;
    if (length_squared(target_delta) <= 0.0001f * 0.0001f ||
        length_squared(forward) <= 0.0001f * 0.0001f) {
        return facing_rotation_toward(from, to, out_rotation);
    }
    const float target_yaw = std::atan2(target_delta.z, target_delta.x);
    const float forward_yaw = std::atan2(forward.z, forward.x);
    const float delta_degrees = (target_yaw - forward_yaw) * (180.0f / kPi);
    *out_rotation = apply_yaw_delta(current_rotation, delta_degrees);
    return true;
}

void transition_to(Enemy* enemy, AgentSentryState state) {
    if (enemy->sentry.state == state) {
        return;
    }
    enemy->sentry.state = state;
    enemy->sentry.state_ticks = 0;
    enemy->sentry.lost_target_ticks = 0;
    enemy->sentry.patrol_rotation_tick = 0;
    enemy->sentry.patrol_rotation_step = 0;
}

bool update_patrol_facing(
    const AgentSentryConfig& config,
    Enemy* enemy,
    const KernelQuat& current_rotation,
    KernelQuat* out_rotation) {
    if (enemy == nullptr || out_rotation == nullptr ||
        config.patrol_rotation_interval_ticks == 0 ||
        config.patrol_rotation_min_degrees <= 0.0f ||
        config.patrol_rotation_max_degrees < config.patrol_rotation_min_degrees) {
        return false;
    }
    ++enemy->sentry.patrol_rotation_tick;
    if (enemy->sentry.patrol_rotation_tick < config.patrol_rotation_interval_ticks) {
        return false;
    }
    enemy->sentry.patrol_rotation_tick = 0;
    ++enemy->sentry.patrol_rotation_step;
    std::uint32_t random_bits =
        enemy->net_id * 747796405u + enemy->sentry.patrol_rotation_step * 2891336453u;
    random_bits ^= random_bits >> 16u;
    random_bits *= 2246822519u;
    random_bits ^= random_bits >> 13u;
    const bool positive_direction = (random_bits & 1u) == 0u;
    const float unit =
        static_cast<float>((random_bits >> 1u) & 0xffffu) / 65535.0f;
    const float magnitude =
        config.patrol_rotation_min_degrees +
        (config.patrol_rotation_max_degrees - config.patrol_rotation_min_degrees) *
            unit;
    const float delta_degrees = positive_direction ? magnitude : -magnitude;
    *out_rotation = apply_yaw_delta(current_rotation, delta_degrees);
    return true;
}

}  // namespace

AgentSentryController::AgentSentryController(AgentSentryConfig config)
    : config_(config) {}

void AgentSentryController::tick(
    KernelHandle* kernel,
    std::vector<Enemy>* enemies,
    float) const {
    if (kernel == nullptr || enemies == nullptr) {
        return;
    }

    const ActorIntentExecutor actor_executor(
        ActorIntentExecutorConfig{config_.weapon_id});

    for (Enemy& enemy : *enemies) {
        const SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(kernel, enemy.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const KernelServerEntityState& entity_state = perception.self_state;

        enemy.position = entity_state.position;
        enemy.hp = entity_state.hp;
        enemy.velocity = zero_vec3();
        enemy.animation_state = config_.animation_idle;
        enemy.sentry.self_id = enemy.net_id;
        KernelQuat desired_rotation = entity_state.rotation;
        bool should_update_rotation = false;

        const bool has_visible_target = perception.has_visible_target;

        if (has_visible_target) {
            enemy.sentry.target = perception.target_id;
            enemy.sentry.lost_target_ticks = 0;
        } else {
            ++enemy.sentry.lost_target_ticks;
        }

        if (enemy.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                transition_to(&enemy, AgentSentryState::kAlert);
            } else {
                should_update_rotation = update_patrol_facing(
                    config_,
                    &enemy,
                    entity_state.rotation,
                    &desired_rotation);
            }
        }

        if (enemy.sentry.state == AgentSentryState::kAlert) {
            if (has_visible_target) {
                const KernelVec3 target = perception.target_position;
                should_update_rotation =
                    facing_rotation_from_vision_toward(
                        enemy.position,
                        target,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                ++enemy.sentry.state_ticks;
                if (enemy.sentry.state_ticks >= config_.alert_ticks) {
                    transition_to(&enemy, AgentSentryState::kAttack);
                }
            } else if (enemy.sentry.lost_target_ticks >= config_.forget_ticks) {
                enemy.sentry.target = 0;
                transition_to(&enemy, AgentSentryState::kIdle);
            }
        }

        if (enemy.sentry.state == AgentSentryState::kAttack) {
            if (has_visible_target) {
                enemy.sentry.lost_target_ticks = 0;
                enemy.animation_state = config_.animation_attack;
                const KernelVec3 target = perception.target_position;
                should_update_rotation =
                    facing_rotation_from_vision_toward(
                        enemy.position,
                        target,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                ai::ScopedIntent intent;
                intent.scope = ai::IntentScope::kActor;
                intent.type = "AttackTarget";
                intent.subject = enemy.net_id;
                intent.params["target_id"] = enemy.sentry.target;
                actor_executor.execute(kernel, &enemy, intent, perception);
            } else if (enemy.sentry.lost_target_ticks >= config_.forget_ticks) {
                transition_to(&enemy, AgentSentryState::kAlert);
            }
        }

        enemy.target_player_net_id = enemy.sentry.target;
        if (should_update_rotation) {
            Kernel_ServerEnqueueEntityTransform(
                kernel,
                KernelCommandSource_AI,
                enemy.net_id,
                &entity_state.position,
                &desired_rotation);
        }
        Kernel_ServerEnqueueEntityVelocity(
            kernel,
            KernelCommandSource_AI,
            enemy.net_id,
            &enemy.velocity);
        Kernel_ServerEnqueueEntityState(
            kernel,
            KernelCommandSource_AI,
            enemy.net_id,
            enemy.animation_state,
            0);
    }
}

}  // namespace network_example::game_server
