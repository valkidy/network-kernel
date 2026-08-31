#ifndef GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
#define GAME_SERVER_AGENT_CHASER_CONTROLLER_H_

#include <vector>

#include "game_server/agent_runtime.h"
#include "game_server/agent_sentry_controller.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// Movement tuning layered on top of the sentry's perception and attack tuning.
struct AgentChaseTuning {
    // Closer than this and the agent holds still instead of walking into the
    // target. Reopening the gap past `resume_distance_meters` starts it moving
    // again; the two thresholds must differ or the agent oscillates on the
    // boundary.
    float stop_distance_meters = 2.0f;
    float resume_distance_meters = 3.0f;
    // Fraction of the template's move speed to chase at, 0..1.
    float input_magnitude = 1.0f;
    // How close the target must be before the agent will actually attack.
    // Visibility is the sentry's gate and it reaches as far as the vision cone,
    // which is much further than a melee weapon does -- without a gate of its
    // own a chaser carrying one swings through empty air the moment it sees
    // anything. Zero means no gate, which is what a chaser carrying a ranged
    // weapon wants and what every chaser did before this field existed.
    //
    // Measured between origins, while the weapon reaches to a target's near
    // edge, so a value equal to the weapon's reach is deliberately on the
    // conservative side of it.
    float attack_range_meters = 0.0f;
};

struct AgentChaserConfig {
    // Detection, alert timing, forgetting, and firing are the sentry's.
    AgentSentryConfig sentry;
    AgentChaseTuning chase;
};

// A sentry that closes on what it sees. It pursues only while the target is
// actually visible: the tick vision breaks the agent stops where it stands and
// falls back through the sentry's forget timers. It never walks to a remembered
// position.
class AgentChaserController {
public:
    explicit AgentChaserController(AgentChaserConfig config = {});

    void tick(
        KernelHandle* kernel,
        std::vector<AgentRuntimeState>* agents,
        float delta_seconds) const;

private:
    AgentChaserConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
