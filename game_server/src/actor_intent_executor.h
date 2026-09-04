#ifndef GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
#define GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_

#include <cstdint>

#include "ai_intent.h"
#include "capability_registry.h"
#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/agent_runtime.h"
#include "game_server/src/ballistic_aim.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {

struct ActorIntentExecutorConfig {
    std::uint16_t weapon_id = UINT16_MAX;
    BallisticAimProfile ballistic_aim;
};

struct ActorIntentExecutionResult {
    ai::IntentStatus status = ai::IntentStatus::kFailed;
    ai::CapabilityReport report;
    bool submitted_input = false;
    bool ballistic_solution_unavailable = false;
};

class ActorIntentExecutor {
public:
    explicit ActorIntentExecutor(ActorIntentExecutorConfig config = {});

    // `move` rides along on the same input the action is submitted with. A
    // second movement-only input in the same tick would overwrite the velocity
    // this one asks for, so callers that both move and act must pass it here.
    ActorIntentExecutionResult execute(
        KernelHandle* kernel,
        AgentRuntimeState* actor,
        const ai::ScopedIntent& intent,
        const SentryPerceptionSnapshot& perception,
        KernelVec2 move = KernelVec2{0.0f, 0.0f}) const;

private:
    ActorIntentExecutorConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
