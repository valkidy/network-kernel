#ifndef GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
#define GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_

#include <cstdint>

#include "ai_intent.h"
#include "capability_registry.h"
#include "game_server/ai_perception_adapter.h"
#include "game_server/agent_runtime.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {

struct ActorIntentExecutorConfig {
    std::uint8_t weapon_id = kAgentSpammerWeaponId;
};

struct ActorIntentExecutionResult {
    ai::IntentStatus status = ai::IntentStatus::kFailed;
    ai::CapabilityReport report;
    bool submitted_input = false;
};

class ActorIntentExecutor {
public:
    explicit ActorIntentExecutor(ActorIntentExecutorConfig config = {});

    ActorIntentExecutionResult execute(
        KernelHandle* kernel,
        AgentRuntimeState* actor,
        const ai::ScopedIntent& intent,
        const SentryPerceptionSnapshot& perception) const;

private:
    ActorIntentExecutorConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
