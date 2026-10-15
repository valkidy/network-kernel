#ifndef GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
#define GAME_SERVER_AGENT_CHASER_CONTROLLER_H_

#include <vector>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/agent_sentry_controller.h"
#include "game_server/src/ai_perception_adapter.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// A leap at the target before the swing: when the target is within reach of a
// leap but not of the weapon, the agent jumps to land `landing_distance_meters`
// short of it, on a flight the kernel solves to land there on a known tick.
// Inert while max_distance_meters is zero, which is every chaser authored
// before leaps existed.
struct AgentLeapTuning {
    // The band, measured between origins like attack_range_meters. Below
    // min_distance_meters it just walks the rest.
    float min_distance_meters = 0.0f;
    float max_distance_meters = 0.0f;
    // Where it lands, short of the target, measured the same way. Inside the
    // attack range, or it lands and still has to walk.
    float landing_distance_meters = 0.0f;
    // How high a level leap peaks; this is what sets its airtime.
    float apex_meters = 0.6f;
    // A long leap is stretched to stay under this. Zero: no cap.
    float max_horizontal_speed_meters_per_second = 0.0f;
    // Ticks after one leap lands before the next may start.
    std::uint32_t cooldown_ticks = 90;
    // Start the swing this many ticks before the landing, so a wind-up of the
    // same length puts the hit on touchdown. Zero swings once it is down.
    std::uint32_t attack_lead_ticks = 0;
};

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
    AgentLeapTuning leap;
};

// What walking to a squad slot costs and what it tolerates. Inert unless the
// agent has a slot, so a chaser that is not part of a patrol reads the same as
// one authored before any of this existed.
struct AgentPatrolTuning {
    // Arrival radius while the squad is stopped, also used when rejoining
    // after pursuit. Moving squads do not stop members inside this radius.
    float slot_radius_meters = 1.0f;
    // Fraction of move speed for arrival at a stopped squad's slot, 0..1.
    // During travel, members match squad velocity and correct formation error
    // up to their template's full move speed.
    float input_magnitude = 0.5f;
    // How far a pursuit may drag the agent from where it broke formation before
    // it gives up and walks back. Zero disables the leash entirely, which is
    // what a chaser with no squad wants and what every chaser did before this
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
    // The template's movement gravity, which a leap's airtime is worked out
    // against. Not authored here.
    float gravity_y = -9.81f;
};

// A sentry that closes on what it sees. It pursues only while the target is
// actually visible: the tick vision breaks the agent stops where it stands and
// falls back through the sentry's forget timers. It never walks to a remembered
// position.
//
// Given a squad slot it also walks to that slot while nothing is in sight,
// breaks off a pursuit that drags it further than its leash, and walks back.
// The slot moves because the squad does; the controller never sees the route
// that moves it. An agent with no slot behaves exactly as it did before squads
// existed -- including standing still the moment it loses sight, which is the
// behaviour a squad replaces rather than one a squad is layered onto.
class AgentChaserController {
public:
    explicit AgentChaserController(AgentChaserConfig config = {});

    // `frame` is the tick's perception snapshot, taken once for the whole
    // population by the caller.
    void tick(
        KernelHandle* kernel,
        const PerceptionFrame& frame,
        const AgentBatch& agents,
        float delta_seconds) const;

private:
    AgentChaserConfig config_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
