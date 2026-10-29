#ifndef GAME_SERVER_AGENT_BEHAVIOR_H_
#define GAME_SERVER_AGENT_BEHAVIOR_H_

#include <cstdint>

#include "game_server/src/agent_chaser_controller.h"
#include "game_server/src/agent_runtime.h"
#include "game_server/src/ai_perception_adapter.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// What one agent's chaser tick has decided so far, which a behavior hook reads
// and adds to: the move it sends and the facing it asks for.
struct AgentBehaviorStep {
    const AgentChaserConfig& config;
    const SentryPerceptionSnapshot& perception;
    const KernelServerEntityState& self;
    KernelVec2 move{0.0f, 0.0f};
    KernelQuat desired_rotation{};
    bool should_update_rotation = false;
};

// Whether a behavior switch was taken (AgentRuntimeManager::set_behavior).
enum class BehaviorSwitchResult : std::uint8_t {
    kAccepted = 0,
    kAgentMissing,
    // Not a switch this agent may make: to or from a sentry, into a squad, or
    // out of one.
    kNotAllowed,
    // The behavior is allowed but the data it needs is missing or wrong.
    kInvalidData,
};

// The part of a chaser that differs between what the agent is for
// (docs/AI_BEHAVIOR_PLAN.md B1): where it goes with nothing in sight, how far
// it may be drawn off, how it walks back, and what it does once it has given
// a target up. Alert, the pursuit and Search are the chaser's own and shared.
//
// Behaviors hold no state: everything they act on lives on the agent.
class AgentBehavior {
public:
    virtual ~AgentBehavior() = default;

    // Whether `spec` carries what this behavior needs, and nothing it would
    // ignore (B3). The data itself is applied by the runtime.
    virtual BehaviorSwitchResult validate(const AgentControllerSpec& spec) const = 0;
    // Switching in and out: each clears what only it uses.
    virtual void enter(AgentRuntimeState&) const {}
    virtual void exit(AgentRuntimeState&) const {}

    // The one target it goes after, or 0 for whatever vision offers. With one,
    // the chaser sees only that target and pursues nothing else.
    virtual std::uint32_t locked_target(const AgentRuntimeState&) const { return 0; }
    // After the chaser has worked out whether it sees its target this tick.
    virtual void after_perception(AgentRuntimeState&, bool /*sees_target*/) const {}

    // How far a pursuit may drag it from where it broke off. Zero is no leash.
    virtual float leash_meters(const AgentChaserConfig& config) const = 0;
    // Idle with nothing in sight.
    virtual void on_idle(AgentRuntimeState& agent, AgentBehaviorStep& step) const = 0;
    // In Return: walks back, and goes Idle once it is back.
    virtual void on_return(AgentRuntimeState& agent, AgentBehaviorStep& step) const = 0;
    // The target is forgotten, from Alert or from Search. Returns the state to
    // go to; the chaser has already dropped the target.
    virtual AgentSentryState on_target_forgotten(AgentRuntimeState& agent) const = 0;
};

// The behavior a chaser role runs, or nullptr for one the chaser does not run
// (none, sentry).
const AgentBehavior* agent_behavior_for_role(AgentControllerRole role);

// How close a guard comes to its post before it counts as back on it.
inline constexpr float kGuardAnchorArrivalMeters = 1.0f;

// Which behavior an agent runs this tick. A squad slot decides before the
// role does: whatever its role, an agent the squad placed answers to the squad.
const AgentBehavior& agent_behavior_for(const AgentRuntimeState& agent);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_BEHAVIOR_H_
