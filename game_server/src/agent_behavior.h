#ifndef GAME_SERVER_AGENT_BEHAVIOR_H_
#define GAME_SERVER_AGENT_BEHAVIOR_H_

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

// The part of a chaser that differs between what the agent is for
// (docs/AI_BEHAVIOR_PLAN.md B1): where it goes with nothing in sight, how far
// it may be drawn off, how it walks back, and what it does once it has given
// a target up. Alert, the pursuit and Search are the chaser's own and shared.
//
// Behaviors hold no state: everything they act on lives on the agent.
class AgentBehavior {
public:
    virtual ~AgentBehavior() = default;

    // How far a pursuit may drag it from where it broke off. Zero is no leash.
    virtual float leash_meters(
        const AgentChaserConfig& config,
        const AgentRuntimeState& agent) const = 0;
    // Idle with nothing in sight.
    virtual void on_idle(AgentRuntimeState& agent, AgentBehaviorStep& step) const = 0;
    // In Return: walks back, and goes Idle once it is back.
    virtual void on_return(AgentRuntimeState& agent, AgentBehaviorStep& step) const = 0;
    // The target is forgotten, from Alert or from Search. Returns the state to
    // go to; the chaser has already dropped the target.
    virtual AgentSentryState on_target_forgotten(AgentRuntimeState& agent) const = 0;
};

// Which behavior an agent runs this tick. A squad slot decides before the
// role does: whatever its role, an agent the squad placed answers to the squad.
const AgentBehavior& agent_behavior_for(const AgentRuntimeState& agent);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_BEHAVIOR_H_
