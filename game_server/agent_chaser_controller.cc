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

// A slot to stand in, which is what being in a squad amounts to from here.
bool has_slot(const AgentRuntimeState& agent) {
    return agent.patrol.has_slot;
}

// Distance from where the agent broke formation, which is what the leash
// bounds. A leash of zero means no leash: a chaser without a squad is never
// dragged away from anything.
bool leash_exceeded(
    const AgentPatrolTuning& patrol,
    const AgentRuntimeState& agent) {
    return patrol.leash_meters > 0.0f &&
        agent_steering::horizontal_distance(
            agent.position, agent.patrol.leash_anchor) > patrol.leash_meters;
}

bool inside_leash_resume(
    const AgentPatrolTuning& patrol,
    const AgentRuntimeState& agent) {
    return agent_steering::horizontal_distance(
               agent.position, agent.patrol.leash_anchor) <=
        patrol.leash_resume_meters;
}

bool standing_in_slot(
    const AgentPatrolTuning& patrol,
    const AgentRuntimeState& agent) {
    return agent_steering::horizontal_distance(
               agent.position, agent.patrol.slot) <= patrol.slot_radius_meters;
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

        // Breaking off is a decision about the squad, not about the target, so
        // it is made before the pursuit states get to look at what they can
        // see. Without it the leash could only be enforced on a tick where
        // vision happened to break.
        if (has_slot(agent) && leash_exceeded(config_.patrol, agent) &&
            (agent.sentry.state == AgentSentryState::kAlert ||
             agent.sentry.state == AgentSentryState::kAttack)) {
            agent.sentry.target = 0;
            agent.chase_holding = false;
            agent_steering::transition_to(&agent, AgentSentryState::kReturn);
        }

        // Ordered ahead of kIdle so that an agent which arrives back on its
        // route resumes walking it on the same tick, the way kIdle -> kAlert
        // already reaches the alert branch in the tick it transitions.
        if (agent.sentry.state == AgentSentryState::kReturn) {
            if (!has_slot(agent)) {
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            } else if (
                standing_in_slot(config_.patrol, agent) ||
                inside_leash_resume(config_.patrol, agent)) {
                // Two ways to be back: standing in the slot, or having returned
                // to where it broke formation. The second is what ends a short
                // chase, where the squad may have moved on and waiting to reach
                // the slot would leave the agent refusing to engage for the
                // rest of the leg.
                //
                // Any re-engagement happens in the kIdle branch below on this
                // same tick, which is also what re-anchors the leash to where
                // the next chase starts.
                agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            } else {
                // Walked back at patrol pace: hurrying back is a tuning
                // opinion, and this way a returning agent reads the same as a
                // patrolling one to anything watching velocity.
                move = agent_steering::horizontal_move_toward(
                    agent.position,
                    agent.patrol.slot,
                    config_.patrol.input_magnitude);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.patrol.slot,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
            }
        }

        if (agent.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                // Recorded before the chase starts, so the leash measures the
                // pursuit rather than the ground the squad has since covered.
                agent.patrol.leash_anchor = agent.position;
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            } else if (has_slot(agent) && !standing_in_slot(config_.patrol, agent)) {
                move = agent_steering::horizontal_move_toward(
                    agent.position,
                    agent.patrol.slot,
                    config_.patrol.input_magnitude);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.patrol.slot,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
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
                // A squad member goes back to its formation rather than idling
                // wherever the chase ended. kIdle would let it re-acquire from
                // there, which is the runaway the leash exists to prevent.
                agent_steering::transition_to(
                    &agent,
                    has_slot(agent) ? AgentSentryState::kReturn
                                    : AgentSentryState::kIdle);
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
                // Seeing a target is not the same as being able to hit one.
                // Not attacking leaves submitted_input false, so the tail of
                // the loop still sends exactly one input carrying `move` --
                // the agent keeps closing, it just does not swing yet.
                const bool target_within_attack_range =
                    config_.chase.attack_range_meters <= 0.0f ||
                    agent_steering::horizontal_distance(
                        agent.position, perception.target_position) <=
                        config_.chase.attack_range_meters;
                if (agent.sentry.ballistic_retry_ticks > 0) {
                    --agent.sentry.ballistic_retry_ticks;
                } else if (target_within_attack_range) {
                    ai::ScopedIntent intent;
                    intent.scope = ai::IntentScope::kActor;
                    intent.type = "AttackTarget";
                    intent.subject = agent.net_id;
                    intent.params["target_id"] = agent.sentry.target;
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
