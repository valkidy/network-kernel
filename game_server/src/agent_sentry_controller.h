#ifndef GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_
#define GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_

#include <cstdint>
#include <vector>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/ballistic_aim.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// Stateless: every knob it reads comes from the agent it is ticking
// (AgentRuntimeState::sentry_config), so one instance drives a mixed population
// of actor templates. AgentSentryConfig itself lives in agent_runtime.h.
class AgentSentryController {
public:
    // `frame` is the tick's perception snapshot, taken once for the whole
    // population by the caller.
    void tick(
        KernelHandle* kernel,
        const PerceptionFrame& frame,
        std::vector<AgentRuntimeState>* agents,
        float delta_seconds) const;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_SENTRY_CONTROLLER_H_
