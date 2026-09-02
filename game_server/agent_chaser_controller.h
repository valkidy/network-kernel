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

// What walking a route costs and what it tolerates. Inert unless the agent
// actually carries a route, so a chaser that is not part of a patrol reads the
// same as one authored before any of this existed.
struct AgentPatrolTuning {
    // How close counts as arrived. A tick of movement covers move_speed/tick_rate
    // metres, so a radius smaller than that leaves the agent overshooting its
    // waypoint and turning back forever.
    float waypoint_radius_meters = 1.0f;
    // Fraction of move speed to walk the route at, 0..1. Patrolling is not
    // chasing; the same value at both would make a patrol indistinguishable
    // from a pursuit at a glance.
    float input_magnitude = 0.5f;
    // How far a pursuit may drag the agent from where it left the route before
    // it breaks off and walks back. Zero disables the leash entirely, which is
    // what a chaser with no route wants and what every chaser did before this
    // field existed.
    float leash_meters = 0.0f;
    // Re-engaging while returning needs a threshold of its own, or the agent
    // oscillates on the leash boundary the same way an un-hystereticised chase
    // oscillates on the stop distance. Must be below leash_meters to have any
    // effect; at or above it the agent re-acquires the moment it breaks off.
    float leash_resume_meters = 0.0f;
};

struct AgentChaserConfig {
    // Detection, alert timing, forgetting, and firing are the sentry's.
    AgentSentryConfig sentry;
    AgentChaseTuning chase;
    AgentPatrolTuning patrol;
};

// A sentry that closes on what it sees. It pursues only while the target is
// actually visible: the tick vision breaks the agent stops where it stands and
// falls back through the sentry's forget timers. It never walks to a remembered
// position.
//
// Given a route it also walks that route while nothing is in sight, breaks off
// a pursuit that drags it further than its leash, and walks back to where it
// was going. An agent with no route behaves exactly as it did before routes
// existed -- including standing still the moment it loses sight, which is the
// behaviour a route replaces rather than one a route is layered onto.
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
