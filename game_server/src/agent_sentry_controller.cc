#include "game_server/src/agent_sentry_controller.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ai_intent.h"
#include "game_server/src/actor_intent_executor.h"
#include "game_server/src/agent_steering.h"
#include "game_server/src/ai_perception_adapter.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {

void AgentSentryController::tick(
    KernelHandle* kernel,
    const PerceptionFrame& frame,
    const AgentBatch& agents,
    float) const {
    if (kernel == nullptr) {
        return;
    }

    for (AgentRuntimeState* agent_pointer : agents) {
        AgentRuntimeState& agent = *agent_pointer;
        // Per agent, not per controller: the weapon and the ballistic profile
        // belong to this agent's own actor template, so the executor is built
        // inside the loop.
        const AgentSentryConfig& config = agent.sentry_config;
        ActorIntentExecutorConfig executor_config;
        executor_config.weapon_id = config.weapon_id;
        executor_config.ballistic_aim = config.ballistic_aim;
        executor_config.charge_ticks = config.charge_ticks;
        const ActorIntentExecutor actor_executor(executor_config);

        const SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(
                kernel, frame, agent.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const KernelServerEntityState& entity_state = perception.self_state;

        agent.position = entity_state.position;
        agent.hp = entity_state.hp;
        // Horizontal only. This velocity is enqueued unconditionally at the
        // bottom of the loop, so zeroing all three axes here told the kernel
        // every tick that the agent's vertical speed was zero -- gravity could
        // never accumulate and a sentry spawned in the air simply hung there.
        // Y belongs to the movement solver; the controller only ever decides
        // where the agent walks. The patrol branch below already did this.
        agent.velocity = KernelVec3{0.0f, entity_state.velocity.y, 0.0f};
        agent.animation_state = 0;
        agent.sentry.self_id = agent.net_id;

        KernelQuat desired_rotation = entity_state.rotation;
        bool should_update_rotation = false;

        const bool has_visible_target = perception.has_visible_target;

        if (has_visible_target) {
            if (agent.sentry.target != perception.target_id) {
                agent.sentry.ballistic_retry_ticks = 0;
            }
            agent.sentry.target = perception.target_id;
            agent.sentry.lost_target_ticks = 0;
            agent.sentry.attention_point = perception.target_position;
            agent.sentry.has_attention_point = true;
        } else {
            ++agent.sentry.lost_target_ticks;
        }

        // A sentry that saw it again picks up where it left off.
        if (agent.sentry.state == AgentSentryState::kSearch && has_visible_target) {
            agent_steering::transition_to(&agent, AgentSentryState::kAttack);
        }

        // A hit (AI plan D8). Idle or searching, it alerts -- the short alert --
        // facing where the hit came from; alert already, it turns there; an
        // attacking sentry keeps the target it has.
        if (perception.damaged) {
            const AgentSentryState state = agent.sentry.state;
            if (state == AgentSentryState::kIdle || state == AgentSentryState::kSearch) {
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
                agent.sentry.alerted_by_damage = true;
            }
            if (agent.sentry.state == AgentSentryState::kAlert && !has_visible_target) {
                agent.sentry.attention_point = perception.damage_source;
                agent.sentry.has_attention_point = true;
            }
        }

        if (agent.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                agent.sentry.alerted_by_damage = false;
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
                if (agent.sentry.state_ticks >=
                    (agent.sentry.alerted_by_damage ? config.alert_on_damage_ticks
                                                    : config.alert_ticks)) {
                    agent_steering::transition_to(
                        &agent, AgentSentryState::kAttack);
                }
            } else if (agent.sentry.lost_target_ticks >= config.forget_ticks) {
                // Lost before the alert ran out: no Search, so a target that
                // ducks in and out of view cannot skip the warning.
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            } else if (agent.sentry.has_attention_point) {
                // Nothing in sight: face where its attention is -- the last
                // place it saw something, or where a hit came from.
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.sentry.attention_point,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
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
            } else {
                agent_steering::transition_to(&agent, AgentSentryState::kSearch);
            }
        }

        // Rooted, so searching is turning to face where the target was last
        // seen until it forgets.
        if (agent.sentry.state == AgentSentryState::kSearch) {
            if (agent.sentry.lost_target_ticks >= config.forget_ticks ||
                !agent.sentry.has_attention_point) {
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            } else {
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.sentry.attention_point,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
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
