#include "game_server/agent_sentry_controller.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ai_intent.h"
#include "game_server/actor_intent_executor.h"
#include "game_server/agent_steering.h"
#include "game_server/ai_perception_adapter.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {

void AgentSentryController::tick(
    KernelHandle* kernel,
    std::vector<AgentRuntimeState>* agents,
    float) const {
    if (kernel == nullptr || agents == nullptr) {
        return;
    }

    for (AgentRuntimeState& agent : *agents) {
        // Per agent, not per controller: the weapon and the ballistic profile
        // belong to this agent's own actor template, so the executor is built
        // inside the loop.
        const AgentSentryConfig& config = agent.sentry_config;
        ActorIntentExecutorConfig executor_config;
        executor_config.weapon_id = config.weapon_id;
        executor_config.ballistic_aim = config.ballistic_aim;
        const ActorIntentExecutor actor_executor(executor_config);

        const SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(kernel, agent.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const KernelServerEntityState& entity_state = perception.self_state;

        agent.position = entity_state.position;
        agent.hp = entity_state.hp;
        agent.velocity = agent_steering::zero_vec3();
        agent.animation_state = 0;
        agent.sentry.self_id = agent.net_id;

        if (config.passive_patrol) {
            if (entity_state.position.x >=
                agent.patrol_anchor.x + config.patrol_extent_x_meters) {
                agent.patrol_direction = -1;
            } else if (entity_state.position.x <=
                       agent.patrol_anchor.x - config.patrol_extent_x_meters) {
                agent.patrol_direction = 1;
            }

            KernelPlayerInput input{};
            input.input_seq = agent.next_input_seq;
            input.move = KernelVec2{
                static_cast<float>(agent.patrol_direction) *
                    config.patrol_input_magnitude,
                0.0f,
            };
            if (Kernel_ServerSubmitEntityInput(kernel, agent.net_id, &input)) {
                ++agent.next_input_seq;
            }
            agent.velocity = KernelVec3{
                input.move.x * config.move_speed_meters_per_second,
                entity_state.velocity.y,
                0.0f,
            };
            agent.sentry.target = 0;
            agent.target_player_net_id = 0;
            agent.sentry.state = AgentSentryState::kIdle;
            agent.animation_state = config.animation_idle;
            Kernel_ServerEnqueueEntityState(
                kernel,
                KernelCommandSource_AI,
                agent.net_id,
                agent.animation_state,
                0);
            continue;
        }

        KernelQuat desired_rotation = entity_state.rotation;
        bool should_update_rotation = false;

        const bool has_visible_target = perception.has_visible_target;

        if (has_visible_target) {
            if (agent.sentry.target != perception.target_id) {
                agent.sentry.ballistic_retry_ticks = 0;
            }
            agent.sentry.target = perception.target_id;
            agent.sentry.lost_target_ticks = 0;
        } else {
            ++agent.sentry.lost_target_ticks;
        }

        if (agent.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            } else {
                should_update_rotation = agent_steering::update_patrol_facing(
                    config.patrol_rotation_interval_ticks,
                    config.patrol_rotation_min_degrees,
                    config.patrol_rotation_max_degrees,
                    &agent,
                    entity_state.rotation,
                    &desired_rotation);
            }
        }

        if (agent.sentry.state == AgentSentryState::kAlert) {
            if (has_visible_target) {
                const KernelVec3 target = perception.target_position;
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        target,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                ++agent.sentry.state_ticks;
                if (agent.sentry.state_ticks >= config.alert_ticks) {
                    agent_steering::transition_to(
                        &agent, AgentSentryState::kAttack);
                }
            } else if (agent.sentry.lost_target_ticks >= config.forget_ticks) {
                agent.sentry.target = 0;
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            }
        }

        if (agent.sentry.state == AgentSentryState::kAttack) {
            if (has_visible_target) {
                agent.sentry.lost_target_ticks = 0;
                const KernelVec3 target = perception.target_position;
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        target,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                ai::ScopedIntent intent;
                intent.scope = ai::IntentScope::kActor;
                intent.type = "AttackTarget";
                intent.subject = agent.net_id;
                intent.params["target_id"] = agent.sentry.target;
                if (agent.sentry.ballistic_retry_ticks > 0) {
                    --agent.sentry.ballistic_retry_ticks;
                } else {
                    const ActorIntentExecutionResult execution =
                        actor_executor.execute(
                            kernel,
                            &agent,
                            intent,
                            perception);
                    if (execution.ballistic_solution_unavailable) {
                        agent.sentry.ballistic_retry_ticks =
                            config.ballistic_retry_cooldown_ticks;
                    }
                }
            } else if (agent.sentry.lost_target_ticks >= config.forget_ticks) {
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            }
        }

        agent.target_player_net_id = agent.sentry.target;
        if (should_update_rotation) {
            Kernel_ServerEnqueueEntityTransform(
                kernel,
                KernelCommandSource_AI,
                agent.net_id,
                &entity_state.position,
                &desired_rotation);
        }
        Kernel_ServerEnqueueEntityVelocity(
            kernel,
            KernelCommandSource_AI,
            agent.net_id,
            &agent.velocity);
        Kernel_ServerEnqueueEntityState(
            kernel,
            KernelCommandSource_AI,
            agent.net_id,
            agent.animation_state,
            0);
    }
}

}  // namespace network_example::game_server
