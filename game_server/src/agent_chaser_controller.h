#ifndef GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
#define GAME_SERVER_AGENT_CHASER_CONTROLLER_H_

#include <vector>

#include "game_server/src/actor_intent_executor.h"
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
// One weapon skill as the chaser runs it (AI plan D13, D14): how to fire the
// weapon -- its own aim and charge, not the template's first weapon's -- and
// when it may.
struct AgentWeaponSkill {
    ActorIntentExecutorConfig executor;
    // Origin to origin. A max of zero is no limit.
    float range_min_meters = 0.0f;
    float range_max_meters = 0.0f;
    std::uint32_t cooldown_ticks = 0;
    std::uint32_t weight = 1;
};

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
    // The weapon swung on the way down (its sp_attack's on_landing.weapon):
    // one of the agent's weapon skills, chosen for the landing.
    std::uint32_t on_landing_weapon_id = 0;
};

// A charge at the target: when it stands inside the band, the agent stops and
// winds up for windup_ticks, still turning to face it, then runs straight at
// where it stood at release (Kernel_ServerEnqueueEntityCharge), overshooting
// by overshoot_meters. The run carries the melee weapon `weapon_id`'s contact;
// it stops on the first hit, on a wall (staggering the agent), on a body, on a
// stagger, or when the distance is covered. Inert while max_distance_meters is
// zero, which is every chaser authored before charges existed.
struct AgentChargeTuning {
    // The band, origin to origin like attack_range_meters.
    float min_distance_meters = 0.0f;
    float max_distance_meters = 0.0f;
    // Standing still and facing the target before the run: the telegraph.
    std::uint32_t windup_ticks = 0;
    float speed_meters_per_second = 0.0f;
    // Run this far past where the target stood, so a target that holds still
    // is reached rather than stopped short of.
    float overshoot_meters = 1.0f;
    // Ticks after one charge ends before the next may wind up.
    std::uint32_t cooldown_ticks = 90;
    // A melee weapon in the loadout. Not the one the chaser swings.
    std::uint8_t weapon_id = 0;
    // Shown while winding up. Defaults to the sentry's idle.
    std::uint16_t windup_animation = 0;
    bool has_windup_animation = false;
};

// Backing off from a target that has come too close
// (`ai.locomotion.retreat`, docs/AI_RANGED_RETREAT_PLAN.md). Opt-in:
// trigger_distance_meters zero leaves the rest unread, which is every chaser
// authored before this existed. Only while pursuing, never in a squad, never
// on a legged rig (its body turns to the way it walks, and with it its eyes).
struct AgentRetreatTuning {
    // Closer than this, origin to origin, and it starts backing off.
    float trigger_distance_meters = 0.0f;
    // Backed off this far, it stops. Inside the hold band, so it does not
    // turn round and close again.
    float target_distance_meters = 0.0f;
    // Fraction of the template's move speed, 0..1.
    float input_magnitude = 1.0f;
    // A target that keeps coming is faster than anything that backs off, so
    // a retreat that has not opened the gap by now gives up and fights where
    // it stands.
    std::uint32_t max_ticks = 90;
    // After a retreat ends, however it ended, how long before another.
    std::uint32_t cooldown_ticks = 150;
    // Whether it keeps shooting while it backs off.
    bool fire_while_retreating = true;
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
    // How far apart, origin to origin, agents of this template keep while
    // they pursue. Their movement capsules do not see one another (a giant's
    // mask is terrain only, and a capsule wide enough for its body cannot
    // walk), so nothing else keeps two that close on one target from standing
    // in each other. While pursuing, each one is pushed away from any other
    // inside this distance; when several pursue the same target the nearest
    // spread round it on a ring between the stop and resume distances, and
    // those that do not fit wait on rings further out. Zero turns both off,
    // which is every chaser authored before this existed. The cost is
    // quadratic in the template's population, so only a template that is
    // never many uses it.
    float spacing_meters = 0.0f;
    AgentLeapTuning leap;
    AgentChargeTuning charge;
    AgentRetreatTuning retreat;
};

// What walking to a squad slot costs and what it tolerates (`behaviors.squad`).
// Inert unless the agent has a slot, so a chaser that is not part of a squad
// reads the same as one authored before any of this existed.
struct AgentSquadTuning {
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

// A guard's or a patrol's leash (AI plan D5; `behaviors.guard`,
// `behaviors.patrol`): how far a pursuit may drag it from where it broke off
// before it gives up and walks back. Measured from that point, like a squad
// member's, and zero turns it off -- it then chases until it loses sight, as
// a garrison did before P2b.
struct AgentLeashTuning {
    float leash_meters = 0.0f;
    // Back within this of where it broke off, it may engage again. Must be
    // below leash_meters; at zero it walks all the way back.
    float leash_resume_meters = 0.0f;
};

struct AgentChaserConfig {
    // Detection, alert timing, forgetting, and firing are the sentry's.
    AgentSentryConfig sentry;
    AgentChaseTuning chase;
    AgentSquadTuning squad;
    // Separate, so a switch at runtime takes the leash of the behavior it
    // switches to (docs/AI_BEHAVIOR_PLAN.md). A hunter is leashed as a guard.
    AgentLeashTuning guard;
    AgentLeashTuning patrol;
    // Its weapon skills, in authored order. Left empty, the controller makes
    // one from sentry.weapon_id reaching chase.attack_range_meters -- what a
    // config built in code (the tests) means.
    std::vector<AgentWeaponSkill> weapon_skills;
    // The template's movement gravity, which a leap's airtime is worked out
    // against. Not authored here.
    float gravity_y = -9.81f;
};

// A sentry that closes on what it sees. It pursues while the target is in
// sight; the tick vision breaks it searches -- walks to where it last saw it --
// until the sentry's forget timer gives up (AI plan P2a).
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
    // The weapon skill a leap swings on landing (chase.leap.on_landing_weapon_id).
    std::uint8_t leap_weapon_skill_ = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_CHASER_CONTROLLER_H_
