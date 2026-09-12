#ifndef GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
#define GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_

#include <cstdint>
#include <optional>

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

    // The direction a shot at `target_position` leaves along: from the muzzle,
    // and lofted when this actor's weapon is ballistic. Empty only when a
    // ballistic weapon has no arc that reaches the target.
    //
    // `shooter_position` stands in for the muzzle if the kernel cannot place
    // one, which is how the aim was derived before there was a muzzle to ask
    // for.
    std::optional<KernelVec3> solve_aim_direction(
        KernelHandle* kernel,
        std::uint32_t shooter_net_id,
        const KernelVec3& shooter_position,
        const KernelVec3& target_position) const;

    // The aim to stamp on any input this actor submits, firing or not.
    //
    // Not optional, and not zero: the kernel reads an input whose aim_dir is
    // zero as aiming down world +X, and that aim is replicated -- the client
    // turns the body with it and drives the aim blend from it. An agent that
    // submits one aimless input while reloading, waiting out a ballistic retry
    // or walking back into range therefore snaps round to face east for as long
    // as it is not shooting, which is exactly the ticks where nothing else is
    // pointing it at anything. Every input carries the direction the agent is
    // looking along instead: the shot it would fire at a visible target, or the
    // way its vision cone already points when it has none.
    KernelVec3 input_aim_direction(
        KernelHandle* kernel,
        std::uint32_t shooter_net_id,
        const SentryPerceptionSnapshot& perception) const;

private:
    ActorIntentExecutorConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_ACTOR_INTENT_EXECUTOR_H_
