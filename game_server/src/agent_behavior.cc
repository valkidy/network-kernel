#include "game_server/src/agent_behavior.h"

#include <algorithm>
#include <cmath>

#include "game_server/src/agent_steering.h"

namespace network_example::game_server {
namespace {

// How close a returning guard or patrol comes to where it broke off. The same
// allowance a route corner gets.
constexpr float kReturnArrivalMeters = 1.0f;

void face_toward(
    const AgentRuntimeState& agent,
    const KernelVec3& point,
    AgentBehaviorStep& step) {
    step.should_update_rotation =
        agent_steering::facing_rotation_from_vision_toward(
            agent.position,
            point,
            step.perception.vision_forward,
            step.self.rotation,
            &step.desired_rotation) ||
        step.should_update_rotation;
}

// Faces where `move` is taking it.
void face_move(const AgentRuntimeState& agent, AgentBehaviorStep& step) {
    face_toward(
        agent,
        KernelVec3{agent.position.x + step.move.x, agent.position.y,
                   agent.position.z + step.move.y},
        step);
}

void look_around(AgentRuntimeState& agent, AgentBehaviorStep& step) {
    const AgentSentryConfig& sentry = step.config.sentry;
    step.should_update_rotation = agent_steering::update_patrol_facing(
        sentry.patrol_rotation_interval_ticks,
        sentry.patrol_rotation_min_degrees,
        sentry.patrol_rotation_max_degrees,
        &agent,
        step.self.rotation,
        &step.desired_rotation);
}

bool inside_leash_resume(
    const AgentSquadTuning& squad,
    const AgentRuntimeState& agent) {
    return agent_steering::horizontal_distance(
               agent.position, agent.patrol.leash_anchor) <=
        squad.leash_resume_meters;
}

bool standing_in_slot(
    const AgentSquadTuning& squad,
    const AgentRuntimeState& agent) {
    return agent_steering::horizontal_distance(
               agent.position, agent.patrol.slot) <= squad.slot_radius_meters;
}

KernelVec2 patrol_move(
    const AgentSquadTuning& squad,
    const AgentRuntimeState& agent,
    float max_speed) {
    const KernelVec3& travel = agent.patrol.travel_velocity;
    const float speed = std::hypot(travel.x, travel.z);
    if (speed <= 0.0001f) {
        return standing_in_slot(squad, agent)
            ? KernelVec2{0.0f, 0.0f}
            : agent_steering::horizontal_move_toward(
                  agent.position, agent.patrol.slot, squad.input_magnitude);
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

// A squad member: holds its slot in the formation while nothing is in sight,
// and goes back to it after a fight. Only the squad makes one, and a member
// stays one.
class SquadBehavior final : public AgentBehavior {
public:
    BehaviorSwitchResult validate(const AgentControllerSpec&) const override {
        return BehaviorSwitchResult::kNotAllowed;
    }

    float leash_meters(const AgentChaserConfig& config) const override {
        return config.squad.leash_meters;
    }

    void on_idle(AgentRuntimeState& agent, AgentBehaviorStep& step) const override {
        const AgentSquadTuning& squad = step.config.squad;
        if (!standing_in_slot(squad, agent) ||
            std::hypot(agent.patrol.travel_velocity.x,
                       agent.patrol.travel_velocity.z) > 0.0001f) {
            step.move = patrol_move(
                squad, agent, step.config.sentry.move_speed_meters_per_second);
            face_move(agent, step);
        } else {
            look_around(agent, step);
        }
    }

    void on_return(AgentRuntimeState& agent, AgentBehaviorStep& step) const override {
        const AgentSquadTuning& squad = step.config.squad;
        if (standing_in_slot(squad, agent) || inside_leash_resume(squad, agent)) {
            // Two ways to be back: standing in the slot, or having returned
            // to where it broke formation. The second is what ends a short
            // chase, where the squad may have moved on and waiting to reach
            // the slot would leave the agent refusing to engage for the
            // rest of the leg.
            //
            // Any re-engagement happens in the kIdle branch on this same
            // tick, which is also what re-anchors the leash to where the next
            // chase starts.
            agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            return;
        }
        step.move = patrol_move(
            squad, agent, step.config.sentry.move_speed_meters_per_second);
        face_move(agent, step);
    }

    AgentSentryState on_target_forgotten(AgentRuntimeState&) const override {
        // Back to its formation rather than idling wherever the chase ended.
        // kIdle would let it re-acquire from there, which is the runaway the
        // leash exists to prevent.
        return AgentSentryState::kReturn;
    }
};

// Leashed, and walks back to where it broke off: a guard, a patrol, a hunter.
class LeashedBehavior : public AgentBehavior {
public:
    float leash_meters(const AgentChaserConfig& config) const override {
        return leash(config).leash_meters;
    }

    void on_return(AgentRuntimeState& agent, AgentBehaviorStep& step) const override {
        // Walks back to where it broke off, then goes idle there -- which puts
        // a patrol back onto its route.
        const float back = agent_steering::horizontal_distance(
            agent.position, agent.patrol.leash_anchor);
        if (back <= std::max(leash(step.config).leash_resume_meters,
                             kReturnArrivalMeters)) {
            agent_steering::transition_to(&agent, AgentSentryState::kIdle);
            return;
        }
        step.move = agent_steering::horizontal_move_toward(
            agent.position, agent.patrol.leash_anchor, step.config.chase.input_magnitude);
        face_toward(agent, agent.patrol.leash_anchor, step);
    }

    AgentSentryState on_target_forgotten(AgentRuntimeState&) const override {
        return AgentSentryState::kIdle;
    }

protected:
    virtual const AgentLeashTuning& leash(const AgentChaserConfig& config) const = 0;
};

// Stands its ground and looks around.
class GuardBehavior : public LeashedBehavior {
public:
    BehaviorSwitchResult validate(const AgentControllerSpec& spec) const override {
        return spec.route.empty() && !spec.has_hunter
            ? BehaviorSwitchResult::kAccepted
            : BehaviorSwitchResult::kInvalidData;
    }

    void on_idle(AgentRuntimeState& agent, AgentBehaviorStep& step) const override {
        look_around(agent, step);
    }

protected:
    const AgentLeashTuning& leash(const AgentChaserConfig& config) const override {
        return config.guard;
    }
};

// Walks the route it was given whenever it has nothing to fight.
class PatrolBehavior final : public LeashedBehavior {
public:
    BehaviorSwitchResult validate(const AgentControllerSpec& spec) const override {
        if (spec.route.size() < 2u || spec.has_hunter) {
            return BehaviorSwitchResult::kInvalidData;
        }
        for (const KernelVec3& point : spec.route) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                return BehaviorSwitchResult::kInvalidData;
            }
        }
        return BehaviorSwitchResult::kAccepted;
    }

    void exit(AgentRuntimeState& agent) const override {
        agent.route_patrol = AgentRoutePatrolState{};
    }

    void on_idle(AgentRuntimeState& agent, AgentBehaviorStep& step) const override {
        if (agent.route_patrol.path.empty()) {
            look_around(agent, step);
            return;
        }
        // Walks it whenever it has nothing to fight, and after a fight carries
        // on to the corner it was heading for from wherever the fight left it.
        step.move = agent_steering::route_patrol_move(
            &agent.route_patrol, agent.position, step.config.chase.input_magnitude);
        face_move(agent, step);
    }

protected:
    const AgentLeashTuning& leash(const AgentChaserConfig& config) const override {
        return config.patrol;
    }
};

// Goes looking for a target it has not seen (AI plan D10). Leashed and
// walked back as a guard is.
class HunterBehavior final : public GuardBehavior {
public:
    BehaviorSwitchResult validate(const AgentControllerSpec& spec) const override {
        return spec.route.empty() ? BehaviorSwitchResult::kAccepted
                                  : BehaviorSwitchResult::kInvalidData;
    }

    void enter(AgentRuntimeState& agent) const override {
        // Looks for a target on its first tick rather than its staggered one.
        agent.seek_repath_in = 0;
    }

    void exit(AgentRuntimeState& agent) const override {
        agent.hunt_target = 0u;
        agent.seek_path.clear();
        agent.seek_next = 0;
    }

    void on_idle(AgentRuntimeState& agent, AgentBehaviorStep&) const override {
        // The chaser's Seek starts walking on this same tick.
        agent_steering::transition_to(&agent, AgentSentryState::kSeek);
    }

    AgentSentryState on_target_forgotten(AgentRuntimeState& agent) const override {
        // Straight back to looking, re-reading where its target is now rather
        // than when the chase began.
        agent.seek_repath_in = 0;
        return AgentSentryState::kSeek;
    }
};

const SquadBehavior kSquad;
const GuardBehavior kGuard;
const PatrolBehavior kPatrol;
const HunterBehavior kHunter;

}  // namespace

const AgentBehavior* agent_behavior_for_role(AgentControllerRole role) {
    switch (role) {
        case AgentControllerRole::kGuard:
            return &kGuard;
        case AgentControllerRole::kPatrol:
            return &kPatrol;
        case AgentControllerRole::kSquad:
            return &kSquad;
        case AgentControllerRole::kHunter:
            return &kHunter;
        case AgentControllerRole::kNone:
        case AgentControllerRole::kSentry:
            break;
    }
    return nullptr;
}

const AgentBehavior& agent_behavior_for(const AgentRuntimeState& agent) {
    if (agent.patrol.has_slot) {
        return kSquad;
    }
    const AgentBehavior* behavior = agent_behavior_for_role(agent.controller_role);
    return behavior != nullptr ? *behavior : kGuard;
}

}  // namespace network_example::game_server
