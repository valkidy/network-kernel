#include "game_server/agent_chaser_controller.h"

#include "ai_intent.h"
#include "game_server/actor_intent_executor.h"
#include "game_server/agent_steering.h"
#include "game_server/ai_perception_adapter.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

bool is_zero_move(const KernelVec2& move) {
    return move.x == 0.0f && move.y == 0.0f;
}

// Hysteresis around the stop distance. Without the separate resume threshold an
// agent parked exactly on the boundary would start and stop every tick.
KernelVec2 chase_move(
    const AgentChaseTuning& chase,
    AgentRuntimeState* agent,
    const KernelVec3& target_position) {
    const float distance =
        agent_steering::horizontal_distance(agent->position, target_position);
    if (agent->chase_holding) {
        if (distance >= chase.resume_distance_meters) {
            agent->chase_holding = false;
        }
    } else if (distance <= chase.stop_distance_meters) {
        agent->chase_holding = true;
    }
    if (agent->chase_holding) {
        return KernelVec2{0.0f, 0.0f};
    }
    return agent_steering::horizontal_move_toward(
        agent->position,
        target_position,
        chase.input_magnitude);
}

}  // namespace

AgentChaserController::AgentChaserController(AgentChaserConfig config)
    : config_(config) {}

void AgentChaserController::tick(
    KernelHandle* kernel,
    std::vector<AgentRuntimeState>* agents,
    float) const {
    if (kernel == nullptr || agents == nullptr) {
        return;
    }

    const AgentSentryConfig& sentry_config = config_.sentry;

    ActorIntentExecutorConfig executor_config;
    executor_config.weapon_id = sentry_config.weapon_id;
    executor_config.ballistic_aim = sentry_config.ballistic_aim;
    const ActorIntentExecutor actor_executor(executor_config);

    for (AgentRuntimeState& agent : *agents) {
        const SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(kernel, agent.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const KernelServerEntityState& entity_state = perception.self_state;

        agent.position = entity_state.position;
        agent.hp = entity_state.hp;
        agent.velocity = agent_steering::zero_vec3();
        agent.animation_state = sentry_config.animation_idle;
        agent.sentry.self_id = agent.net_id;

        KernelQuat desired_rotation = entity_state.rotation;
        bool should_update_rotation = false;
        KernelVec2 move{0.0f, 0.0f};
        bool submitted_input = false;

        const bool has_visible_target = perception.has_visible_target;

        if (has_visible_target) {
            if (agent.sentry.target != perception.target_id) {
                agent.sentry.ballistic_retry_ticks = 0;
            }
            agent.sentry.target = perception.target_id;
            agent.sentry.lost_target_ticks = 0;
        } else {
            ++agent.sentry.lost_target_ticks;
            // Losing sight ends the pursuit outright; the agent stops where it
            // stands rather than walking to where the target was.
            agent.chase_holding = false;
        }

        if (agent.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            } else {
                should_update_rotation = agent_steering::update_patrol_facing(
                    sentry_config.patrol_rotation_interval_ticks,
                    sentry_config.patrol_rotation_min_degrees,
                    sentry_config.patrol_rotation_max_degrees,
                    &agent,
                    entity_state.rotation,
                    &desired_rotation);
            }
        }

        if (agent.sentry.state == AgentSentryState::kAlert) {
            if (has_visible_target) {
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        perception.target_position,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                move = chase_move(
                    config_.chase, &agent, perception.target_position);
                ++agent.sentry.state_ticks;
                if (agent.sentry.state_ticks >= sentry_config.alert_ticks) {
                    agent_steering::transition_to(
                        &agent, AgentSentryState::kAttack);
                }
            } else if (agent.sentry.lost_target_ticks >=
                       sentry_config.forget_ticks) {
                agent.sentry.target = 0;
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            }
        }

        if (agent.sentry.state == AgentSentryState::kAttack) {
            if (has_visible_target) {
                agent.sentry.lost_target_ticks = 0;
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        perception.target_position,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                move = chase_move(
                    config_.chase, &agent, perception.target_position);
                ai::ScopedIntent intent;
                intent.scope = ai::IntentScope::kActor;
                intent.type = "AttackTarget";
                intent.subject = agent.net_id;
                intent.params["target_id"] = agent.sentry.target;
                if (agent.sentry.ballistic_retry_ticks > 0) {
                    --agent.sentry.ballistic_retry_ticks;
                } else {
                    // The move rides on the attack input: a separate movement
                    // input in the same tick would carry a higher input_seq and
                    // the movement solver would take that one instead.
                    const ActorIntentExecutionResult execution =
                        actor_executor.execute(
                            kernel,
                            &agent,
                            intent,
                            perception,
                            move);
                    if (execution.ballistic_solution_unavailable) {
                        agent.sentry.ballistic_retry_ticks =
                            sentry_config.ballistic_retry_cooldown_ticks;
                    }
                    submitted_input = execution.submitted_input;
                }
            } else if (agent.sentry.lost_target_ticks >=
                       sentry_config.forget_ticks) {
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            }
        }

        if (!is_zero_move(move)) {
            agent.animation_state = sentry_config.animation_attack;
            agent.velocity = KernelVec3{
                move.x * sentry_config.move_speed_meters_per_second,
                entity_state.velocity.y,
                move.y * sentry_config.move_speed_meters_per_second,
            };
        }

        // Exactly one input per tick, always. An agent that submits none keeps
        // the horizontal velocity it already had, because the movement solver
        // only zeroes uncommanded *players*.
        if (!submitted_input) {
            KernelPlayerInput input{};
            input.input_seq = agent.next_input_seq;
            input.move = move;
            // Must match what the executor sends, or an in-flight action reads
            // this as a weapon change and cancels itself.
            input.selected_weapon =
                static_cast<std::uint8_t>(sentry_config.weapon_id);
            if (Kernel_ServerEnqueueEntityInput(
                    kernel,
                    KernelCommandSource_AI,
                    agent.net_id,
                    &input)) {
                ++agent.next_input_seq;
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
        Kernel_ServerEnqueueEntityState(
            kernel,
            KernelCommandSource_AI,
            agent.net_id,
            agent.animation_state,
            0);
    }
}

}  // namespace network_example::game_server
