#include "game_server/src/agent_chaser_controller.h"

#include <algorithm>
#include <cmath>

#include "ai_intent.h"
#include "game_server/src/actor_intent_executor.h"
#include "game_server/src/agent_steering.h"
#include "game_server/src/ai_perception_adapter.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

// How close Search comes to where the target was last seen before it stops
// and looks around, and a returning garrison to where it broke off. The same
// allowance a route corner gets.
constexpr float kSearchArrivalMeters = 1.0f;
constexpr float kReturnArrivalMeters = 1.0f;

bool is_zero_move(const KernelVec2& move) {
    return move.x == 0.0f && move.y == 0.0f;
}

// A slot to stand in, which is what being in a squad amounts to from here.
bool has_slot(const AgentRuntimeState& agent) {
    return agent.patrol.has_slot;
}

// Distance from where the agent broke off, which is what the leash bounds. A
// leash of zero means no leash.
bool leash_exceeded(float leash_meters, const AgentRuntimeState& agent) {
    return leash_meters > 0.0f &&
        agent_steering::horizontal_distance(
            agent.position, agent.patrol.leash_anchor) > leash_meters;
}

// A squad member answers to its squad's leash, anyone else to its garrison's.
float leash_meters(const AgentChaserConfig& config, const AgentRuntimeState& agent) {
    return has_slot(agent) ? config.patrol.leash_meters : config.garrison.leash_meters;
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

KernelVec2 patrol_move(
    const AgentPatrolTuning& patrol,
    const AgentRuntimeState& agent,
    float max_speed) {
    const KernelVec3& travel = agent.patrol.travel_velocity;
    const float speed = std::hypot(travel.x, travel.z);
    if (speed <= 0.0001f) {
        return standing_in_slot(patrol, agent)
            ? KernelVec2{0.0f, 0.0f}
            : agent_steering::horizontal_move_toward(
                  agent.position, agent.patrol.slot, patrol.input_magnitude);
    }
    if (max_speed <= 0.0f) {
        return KernelVec2{0.0f, 0.0f};
    }

    const float forward_x = travel.x / speed;
    const float forward_z = travel.z / speed;
    // Correct position error over roughly one second, while feeding through
    // the squad velocity. Ahead members slow down without stopping or backing
    // up; lagging members may use the template's full movement speed.
    constexpr float kCorrectionPerSecond = 1.0f;
    const float error_x = agent.patrol.slot.x - agent.position.x;
    const float error_z = agent.patrol.slot.z - agent.position.z;
    const float along = error_x * forward_x + error_z * forward_z;
    const float forward_speed = std::max(
        speed * 0.25f, speed + along * kCorrectionPerSecond);
    const float velocity_x = forward_x * forward_speed +
        (error_x - along * forward_x) * kCorrectionPerSecond;
    const float velocity_z = forward_z * forward_speed +
        (error_z - along * forward_z) * kCorrectionPerSecond;
    const float scale = std::max(max_speed, std::hypot(velocity_x, velocity_z));
    return KernelVec2{velocity_x / scale, velocity_z / scale};
}

// Mirrors leap_airtime_ticks in simulation/public/simulation.h, which
// game_server does not link: the airtime of a leap that peaks apex_meters over
// the higher of its two ends, stretched to keep under the horizontal speed
// cap. The kernel takes the result as given, so the two only have to agree to
// keep tuning meaningful.
std::uint32_t leap_airtime_ticks(
    const AgentLeapTuning& leap,
    float horizontal_distance,
    float height_difference,
    float gravity_y,
    float fixed_delta_seconds) {
    float seconds = 0.0f;
    if (leap.apex_meters > 0.0f && gravity_y < 0.0f) {
        const float rise =
            leap.apex_meters + std::max(0.0f, height_difference);
        const float fall =
            leap.apex_meters + std::max(0.0f, -height_difference);
        seconds = std::sqrt(2.0f * rise / -gravity_y) +
            std::sqrt(2.0f * fall / -gravity_y);
    }
    if (leap.max_horizontal_speed_meters_per_second > 0.0f) {
        seconds = std::max(
            seconds,
            horizontal_distance / leap.max_horizontal_speed_meters_per_second);
    }
    const float ticks = std::ceil(seconds / fixed_delta_seconds);
    // The kernel refuses a one-step leap.
    return ticks < 2.0f ? 2u : static_cast<std::uint32_t>(ticks);
}

// Starts a leap at the target if it is in the band and the agent is free to.
// The landing spot is fixed here, at lift-off: a target that moves after the
// agent commits is where the leap misses. Returns whether one was enqueued;
// the kernel may still refuse it, which the next tick's flags show.
//
// target_feet is the target's own position, not perception's target_position:
// that is an aim point at the body's centre, and read as the ground it put a
// level leap 0.9 m uphill and stretched its airtime to match.
bool try_start_leap(
    KernelHandle* kernel,
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelServerEntityState& self,
    const KernelVec3& target_feet) {
    const AgentLeapTuning& leap = config.chase.leap;
    if (leap.max_distance_meters <= 0.0f || agent->leap_ticks_to_landing > 0u ||
        agent->leap_cooldown_ticks > 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) == 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_KNOCKED_BACK) != 0u) {
        return false;
    }
    const float dx = target_feet.x - agent->position.x;
    const float dz = target_feet.z - agent->position.z;
    const float distance = std::hypot(dx, dz);
    if (distance < leap.min_distance_meters ||
        distance > leap.max_distance_meters || distance <= 0.0001f) {
        return false;
    }
    const float travel = distance - leap.landing_distance_meters;
    const KernelVec3 destination{
        agent->position.x + dx * (travel / distance),
        target_feet.y,
        agent->position.z + dz * (travel / distance)};
    // The target's feet stand in for the ground under the landing spot,
    // which only the kernel probes.
    const std::uint32_t airtime = leap_airtime_ticks(
        leap,
        travel,
        target_feet.y - agent->position.y,
        config.gravity_y,
        Kernel_GetFixedDeltaSeconds(kernel));
    if (!Kernel_ServerEnqueueEntityLeap(
            kernel, KernelCommandSource_AI, agent->net_id, &destination, airtime)) {
        return false;
    }
    agent->leap_ticks_to_landing = airtime;
    agent->leap_cooldown_ticks = airtime + leap.cooldown_ticks;
    return true;
}

// Starts winding up a charge if the target is in the band and the agent is
// free to: on the ground, not knocked back or staggered, not leaping, and off
// cooldown. The run itself is released by release_charge once the wind-up is
// spent.
bool try_start_charge(
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelServerEntityState& self,
    const KernelVec3& target_feet) {
    const AgentChargeTuning& charge = config.chase.charge;
    if (charge.max_distance_meters <= 0.0f || agent->charge_cooldown_ticks > 0u ||
        agent->charge_windup_ticks > 0u || agent->charge_ticks_left > 0u ||
        agent->leap_ticks_to_landing > 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) == 0u ||
        (self.visual_flags & (KERNEL_VISUAL_FLAG_KNOCKED_BACK |
                              KERNEL_VISUAL_FLAG_STAGGERED)) != 0u) {
        return false;
    }
    const float distance = std::hypot(
        target_feet.x - agent->position.x, target_feet.z - agent->position.z);
    if (distance < charge.min_distance_meters ||
        distance > charge.max_distance_meters) {
        return false;
    }
    // Plus one: release_charge spends a tick of it before it looks.
    agent->charge_windup_ticks = charge.windup_ticks + 1u;
    return true;
}

// Releases the run at where the target stands now: the line is fixed here, so
// a target that moves after this is where the charge misses. Long enough to
// pass overshoot_meters beyond that spot. Whatever the kernel says, the agent
// goes on cooldown, so a refused charge is not retried every tick.
void release_charge(
    KernelHandle* kernel,
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelVec3& target_feet) {
    const AgentChargeTuning& charge = config.chase.charge;
    agent->charge_cooldown_ticks = charge.cooldown_ticks;
    const float dx = target_feet.x - agent->position.x;
    const float dz = target_feet.z - agent->position.z;
    const float distance = std::hypot(dx, dz);
    if (distance <= 0.0001f) {
        return;
    }
    const float seconds =
        (distance + charge.overshoot_meters) / charge.speed_meters_per_second;
    const float ticks = std::ceil(seconds / Kernel_GetFixedDeltaSeconds(kernel));
    const std::uint32_t duration = static_cast<std::uint32_t>(std::clamp(
        ticks, 1.0f, static_cast<float>(KERNEL_MAX_IMPULSE_LOCKOUT_TICKS)));
    const KernelVec3 direction{dx / distance, 0.0f, dz / distance};
    if (!Kernel_ServerEnqueueEntityCharge(
            kernel,
            KernelCommandSource_AI,
            agent->net_id,
            &direction,
            charge.speed_meters_per_second,
            duration,
            charge.weapon_id)) {
        return;
    }
    agent->charge_ticks_left = duration;
    agent->charge_direction = direction;
    agent->charge_cooldown_ticks = duration + charge.cooldown_ticks;
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
    const PerceptionFrame& frame,
    const AgentBatch& agents,
    float) const {
    if (kernel == nullptr) {
        return;
    }

    const AgentSentryConfig& sentry_config = config_.sentry;

    ActorIntentExecutorConfig executor_config;
    executor_config.weapon_id = sentry_config.weapon_id;
    executor_config.ballistic_aim = sentry_config.ballistic_aim;
    executor_config.charge_ticks = sentry_config.charge_ticks;
    const ActorIntentExecutor actor_executor(executor_config);

    for (AgentRuntimeState* agent_pointer : agents) {
        AgentRuntimeState& agent = *agent_pointer;
        const SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(
                kernel, frame, agent.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const KernelServerEntityState& entity_state = perception.self_state;

        agent.position = entity_state.position;
        agent.hp = entity_state.hp;
        agent.velocity = agent_steering::zero_vec3();
        agent.animation_state = sentry_config.animation_idle;
        agent.sentry.self_id = agent.net_id;
        // A leap counts down to its landing. The kernel arms it on the tick
        // after it is asked, so from then on the flag says it is in the air;
        // without the flag it was refused or something ended it, and there is
        // no landing left to time a swing against.
        if (agent.leap_ticks_to_landing > 0u) {
            --agent.leap_ticks_to_landing;
            if ((entity_state.visual_flags & KERNEL_VISUAL_FLAG_LEAPING) == 0u) {
                agent.leap_ticks_to_landing = 0u;
            }
        }
        if (agent.leap_cooldown_ticks > 0u) {
            --agent.leap_cooldown_ticks;
        }
        // A charge counts down the same way: armed the tick after it is
        // asked, and over -- a hit, a wall, a stagger -- once the flag is gone.
        if (agent.charge_ticks_left > 0u) {
            --agent.charge_ticks_left;
            if ((entity_state.visual_flags & KERNEL_VISUAL_FLAG_CHARGING) == 0u) {
                agent.charge_ticks_left = 0u;
            }
        }
        if (agent.charge_cooldown_ticks > 0u) {
            --agent.charge_cooldown_ticks;
        }

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
            // Feet, not the aim point: Search walks to it.
            const KernelServerEntityState* seen =
                frame.actor_state(perception.target_id);
            agent.sentry.attention_point =
                seen != nullptr ? seen->position : perception.target_position;
            agent.sentry.has_attention_point = true;
        } else {
            ++agent.sentry.lost_target_ticks;
            // The hold is about a target in sight; Search walks to where it
            // was last seen instead.
            agent.chase_holding = false;
        }

        // Breaking off is a decision about the squad, not about the target, so
        // it is made before the pursuit states get to look at what they can
        // see. Without it the leash could only be enforced on a tick where
        // vision happened to break.
        if (leash_exceeded(leash_meters(config_, agent), agent) &&
            is_engaged(agent.sentry.state)) {
            agent.sentry.target = 0;
            agent.sentry.has_attention_point = false;
            agent.chase_holding = false;
            agent_steering::transition_to(&agent, AgentSentryState::kReturn);
        }

        // Ordered ahead of kIdle so that an agent which arrives back on its
        // route resumes walking it on the same tick, the way kIdle -> kAlert
        // already reaches the alert branch in the tick it transitions.
        if (agent.sentry.state == AgentSentryState::kReturn) {
            if (!has_slot(agent)) {
                // A garrison walks back to where it broke off, then goes idle
                // there -- which puts one on a route back onto it.
                const float back = agent_steering::horizontal_distance(
                    agent.position, agent.patrol.leash_anchor);
                if (back <= std::max(config_.garrison.leash_resume_meters,
                                     kReturnArrivalMeters)) {
                    agent_steering::transition_to(&agent, AgentSentryState::kIdle);
                } else {
                    move = agent_steering::horizontal_move_toward(
                        agent.position,
                        agent.patrol.leash_anchor,
                        config_.chase.input_magnitude);
                    should_update_rotation =
                        agent_steering::facing_rotation_from_vision_toward(
                            agent.position,
                            agent.patrol.leash_anchor,
                            perception.vision_forward,
                            entity_state.rotation,
                            &desired_rotation) ||
                        should_update_rotation;
                }
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
                move = patrol_move(
                    config_.patrol, agent, sentry_config.move_speed_meters_per_second);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        KernelVec3{agent.position.x + move.x, agent.position.y,
                                   agent.position.z + move.y},
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
            } else if (has_slot(agent) &&
                       (!standing_in_slot(config_.patrol, agent) ||
                        std::hypot(agent.patrol.travel_velocity.x,
                                   agent.patrol.travel_velocity.z) > 0.0001f)) {
                move = patrol_move(
                    config_.patrol, agent, sentry_config.move_speed_meters_per_second);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        KernelVec3{agent.position.x + move.x, agent.position.y,
                                   agent.position.z + move.y},
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
            } else if (!has_slot(agent) && !agent.route_patrol.path.empty()) {
                // A garrison on a route walks it whenever it has nothing to
                // fight, and after a fight carries on to the corner it was
                // heading for from wherever the fight left it. Pathing back
                // and a leash on the route are P2's.
                move = agent_steering::route_patrol_move(
                    &agent.route_patrol, agent.position, config_.chase.input_magnitude);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        KernelVec3{agent.position.x + move.x, agent.position.y,
                                   agent.position.z + move.y},
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
                    // The pursuit below sorts Chasing from Attack this tick.
                    agent_steering::transition_to(
                        &agent, AgentSentryState::kChasing);
                }
            } else if (agent.sentry.lost_target_ticks >=
                       sentry_config.forget_ticks) {
                // Lost before the alert ran out: no Search, so a target that
                // ducks in and out of view cannot skip the warning. It waits
                // here, as it always has, and gives up.
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                // A squad member goes back to its formation rather than idling
                // wherever the chase ended. kIdle would let it re-acquire from
                // there, which is the runaway the leash exists to prevent.
                agent_steering::transition_to(
                    &agent,
                    has_slot(agent) ? AgentSentryState::kReturn
                                    : AgentSentryState::kIdle);
            }
        }

        // A wind-up is for this target in this pursuit: losing either drops it.
        // So does being knocked back or staggered out of it.
        if (agent.charge_windup_ticks > 0u &&
            (!is_pursuing(agent.sentry.state) ||
             !has_visible_target ||
             (entity_state.visual_flags & (KERNEL_VISUAL_FLAG_KNOCKED_BACK |
                                           KERNEL_VISUAL_FLAG_STAGGERED)) != 0u)) {
            agent.charge_windup_ticks = 0u;
        }

        const KernelServerEntityState* target_state =
            frame.actor_state(agent.sentry.target);
        if (is_pursuing(agent.sentry.state) &&
            has_visible_target && agent.charge_ticks_left > 0u) {
            // The kernel runs the charge: no steering, no turning, no swing.
            // The tail still sends the one input, with no move in it.
            agent.sentry.lost_target_ticks = 0;
            agent.sentry.state = AgentSentryState::kAttack;
        } else if (is_pursuing(agent.sentry.state) &&
                   has_visible_target && agent.charge_windup_ticks > 0u) {
            // Winding up: rooted, turning to keep the target in front.
            agent.sentry.lost_target_ticks = 0;
            agent.sentry.state = AgentSentryState::kAttack;
            should_update_rotation =
                agent_steering::facing_rotation_from_vision_toward(
                    agent.position,
                    perception.target_position,
                    perception.vision_forward,
                    entity_state.rotation,
                    &desired_rotation) ||
                should_update_rotation;
            if (--agent.charge_windup_ticks == 0u && target_state != nullptr) {
                release_charge(kernel, config_, &agent, target_state->position);
            }
        } else if (is_pursuing(agent.sentry.state)) {
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
                // A charge is tried ahead of a leap: winding one up roots the
                // agent from this tick, and a leap must not carry it off.
                if (target_state != nullptr &&
                    try_start_charge(
                        config_, &agent, entity_state, target_state->position)) {
                    move = KernelVec2{0.0f, 0.0f};
                } else if (target_state != nullptr) {
                    (void)try_start_leap(
                        kernel, config_, &agent, entity_state,
                        target_state->position);
                }
                // Seeing a target is not the same as being able to hit one.
                // Not attacking leaves submitted_input false, so the tail of
                // the loop still sends exactly one input carrying `move` --
                // the agent keeps closing, it just does not swing yet.
                //
                // In a leap, range is the landing's business: it comes down
                // inside it, so the swing goes by how soon that is instead.
                const bool target_within_attack_range =
                    agent.leap_ticks_to_landing > 0u
                    ? agent.leap_ticks_to_landing <=
                        config_.chase.leap.attack_lead_ticks
                    : config_.chase.attack_range_meters <= 0.0f ||
                        agent_steering::horizontal_distance(
                            agent.position, perception.target_position) <=
                            config_.chase.attack_range_meters;
                // Set directly, not through transition_to: flipping between
                // the two must not reset the pursuit's counters. A wind-up
                // started this tick is an attack already.
                agent.sentry.state =
                    target_within_attack_range || agent.charge_windup_ticks > 0u
                    ? AgentSentryState::kAttack
                    : AgentSentryState::kChasing;
                if (agent.sentry.ballistic_retry_ticks > 0) {
                    --agent.sentry.ballistic_retry_ticks;
                } else if (agent.charge_windup_ticks > 0u) {
                    // Wound up this tick: no swing on top of it.
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
            } else {
                agent_steering::transition_to(&agent, AgentSentryState::kSearch);
            }
        }

        // After the pursuit, so an agent that loses sight starts walking to
        // where it last saw the target on that same tick. Seeing it again
        // resumes the pursuit from the next one.
        if (agent.sentry.state == AgentSentryState::kSearch) {
            if (has_visible_target) {
                agent_steering::transition_to(&agent, AgentSentryState::kChasing);
            } else if (agent.sentry.lost_target_ticks >=
                           sentry_config.forget_ticks ||
                       !agent.sentry.has_attention_point) {
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                // A squad member goes back to its formation, as from Alert.
                agent_steering::transition_to(
                    &agent,
                    has_slot(agent) ? AgentSentryState::kReturn
                                    : AgentSentryState::kIdle);
            } else if (agent_steering::horizontal_distance(
                           agent.position, agent.sentry.attention_point) >
                       kSearchArrivalMeters) {
                move = agent_steering::horizontal_move_toward(
                    agent.position,
                    agent.sentry.attention_point,
                    config_.chase.input_magnitude);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.sentry.attention_point,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
            } else {
                // Arrived and nothing there: look around until it forgets.
                should_update_rotation = agent_steering::update_patrol_facing(
                    sentry_config.patrol_rotation_interval_ticks,
                    sentry_config.patrol_rotation_min_degrees,
                    sentry_config.patrol_rotation_max_degrees,
                    &agent,
                    entity_state.rotation,
                    &desired_rotation);
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
        if (agent.charge_windup_ticks > 0u) {
            agent.animation_state = config_.chase.charge.has_windup_animation
                ? config_.chase.charge.windup_animation
                : sentry_config.animation_idle;
        } else if (agent.charge_ticks_left > 0u) {
            agent.animation_state = sentry_config.animation_attack;
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
            // And so must the aim. A zero aim_dir is not "no opinion": the
            // kernel reads it as aiming down world +X and replicates that, so
            // every tick the executor declined to fire on -- reloading, waiting
            // out a ballistic retry, closing the last metres into range -- used
            // to turn the agent away from the target it is shooting at.
            input.aim_dir = actor_executor.input_aim_direction(
                kernel, agent.net_id, perception);
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
