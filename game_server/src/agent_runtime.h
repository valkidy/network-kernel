#ifndef GAME_SERVER_AGENT_RUNTIME_H_
#define GAME_SERVER_AGENT_RUNTIME_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_server/src/ballistic_aim.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypeActor = 1;
inline constexpr std::uint16_t kActorTypePlayer = KernelActorType_Player;
inline constexpr std::uint16_t kActorTypeAgent = KernelActorType_Agent;

// One tick's worth of actor states, taken once and handed to every phase that
// needs it. The phases used to each run their own Kernel_ServerQueryEntities --
// four of them per tick, one of which ran once per patrol squad -- and every one
// of those walks the whole world and builds a full state per entity.
//
// It is a view, not a copy: the buffer belongs to whoever took the snapshot, and
// it is only valid until the next thing creates or destroys an entity.
struct ActorStateView {
    const KernelServerEntityState* states = nullptr;
    std::uint32_t count = 0;
};

inline constexpr std::uint16_t kAgentInitialHp = 240;
inline constexpr std::uint8_t kAgentSpammerWeaponId = 2;

enum class AgentSentryState : std::uint8_t {
    kIdle = 0,
    kAlert = 1,
    kAttack = 2,
    // Walking back to the route after a pursuit, and refusing to start another
    // one on the way. Without a state of its own the agent re-acquires the
    // target it just broke off from on the very next tick and never returns.
    kReturn = 3,
    // The target is in sight but no attack can reach it yet: a chaser closes,
    // a sentry -- which has no reach to fall short of -- never is. (AI plan
    // D4: Attack is the same pursuit once something can hit.)
    kChasing = 4,
    // Sight lost mid-pursuit, still inside forget_ticks: a chaser walks to
    // where it last saw the target, a sentry turns to face it. Seeing it again
    // resumes the pursuit; forgetting it ends the engagement.
    kSearch = 5,
    // A hunter walking to where the shared table says its target is, which it
    // cannot see yet (D10). Seeing anything hostile alerts it as from Idle.
    kSeek = 6,
};

// The pursuit proper: something to close on or to hit.
inline bool is_pursuing(AgentSentryState state) {
    return state == AgentSentryState::kChasing || state == AgentSentryState::kAttack;
}

// The agent has a target, or had one recently enough that it has not given up
// on it yet. A squad holds while a member is engaged, and an on_alert spawner
// only calls while its carrier is.
inline bool is_engaged(AgentSentryState state) {
    return state == AgentSentryState::kAlert || is_pursuing(state) ||
           state == AgentSentryState::kSearch;
}

// What a squad member needs to know about the squad, which is very little: the
// point it should be standing on and the squad's travel velocity. The route,
// formation and progress all live on the PatrolGroup, so the controller can
// walk a patrol without knowing that routes exist.
//
// has_slot false is every agent that is not in a squad, and it leaves the
// chaser behaving exactly as it did before squads existed.
struct AgentPatrolRuntimeState {
    std::uint32_t group_id = 0;
    KernelVec3 slot{0.0f, 0.0f, 0.0f};
    // Zero while holding or finished; otherwise members keep walking even
    // inside the slot radius, and use their full move speed to catch up.
    KernelVec3 travel_velocity{0.0f, 0.0f, 0.0f};
    bool has_slot = false;
    // Where the agent stood when it broke off to give chase. The leash is
    // measured from here rather than from the slot, so "how far a pursuit may
    // drag a patrol" is a property of the pursuit and does not change as the
    // squad moves on.
    KernelVec3 leash_anchor{0.0f, 0.0f, 0.0f};
};

// What an agent is there to do, which picks the controller that runs it
// (docs/AI_CONTROLLER_PLAN.md D1, D2). A template names a default; P1b lets
// whatever spawned the agent choose instead. Sentry runs the sentry
// controller; garrison and patrol both run the chaser, which tells them apart
// by whether the squad gave the agent a slot.
enum class AgentControllerRole : std::uint8_t {
    kNone = 0,
    kSentry,
    kGarrison,
    kPatrol,
    // Goes looking for a target it has not seen (AI plan D2, D10): the chaser,
    // with a shared table of every hostile's position to walk towards.
    kHunter,
};

// Who a hunter goes after (D10).
enum class HunterTargetSource : std::uint8_t {
    // The nearest hostile, re-chosen as they move.
    kNearestHostile = 0,
    // Whatever the agent that spawned it was fighting: reinforcements.
    kInherited = 1,
};

// What a hunter does once its target is gone for good -- dead, gone from the
// world -- rather than merely out of sight.
enum class HunterOnTargetLost : std::uint8_t {
    kNearestHostile = 0,
    // Stays where it is as a garrison.
    kGarrison = 1,
};

struct AgentHunterTuning {
    HunterTargetSource target_source = HunterTargetSource::kNearestHostile;
    HunterOnTargetLost on_target_lost = HunterOnTargetLost::kNearestHostile;
    // How often it re-reads where its target is and re-paths there. Each
    // hunter's first is staggered by its net id, so a crowd spreads the work.
    std::uint32_t repath_ticks = 60;
};

// What a spawn source asks of the agents it makes (plan D1): authored as a
// `controller:` block on a spawn_group, a spawner or a world rule, and set to
// patrol by a squad for its own members. Empty means the template's default
// controller and no route.
struct AgentControllerSpec {
    // kNone keeps the template's default_controller_role.
    AgentControllerRole role = AgentControllerRole::kNone;
    // World space. Only a garrison follows one.
    std::vector<KernelVec3> route;
    // kHunter only: overrides the template's controllers.hunter when set.
    bool has_hunter = false;
    AgentHunterTuning hunter;

    bool empty() const {
        return role == AgentControllerRole::kNone && route.empty() && !has_hunter;
    }

};

// One agent a director just made, and what it asked of it. Directors only
// report these: the runtime applies them once it has discovered the agent.
struct SpawnedControllerSpec {
    std::uint32_t net_id = 0;
    AgentControllerSpec spec;
    // The agent that spawned it, when one did (a spawner's carrier): whose
    // target an inherited hunter takes.
    std::uint32_t source_net_id = 0;
};

// The most skills one agent may carry (AI plan D13).
inline constexpr std::size_t kMaxAgentSkills = 8;

struct AgentSentryRuntimeState {
    std::uint32_t self_id = 0;
    AgentSentryState state = AgentSentryState::kIdle;
    std::uint32_t state_ticks = 0;
    std::uint32_t lost_target_ticks = 0;
    std::uint32_t patrol_rotation_tick = 0;
    std::uint32_t patrol_rotation_step = 0;
    std::uint32_t ballistic_retry_ticks = 0;
    std::uint32_t target = 0;
    // Where the agent's attention is (AI plan D6): the target's feet while it
    // is in sight, and where it was last seen after. Search walks or turns to
    // it. Cleared when the engagement ends.
    bool has_attention_point = false;
    KernelVec3 attention_point{0.0f, 0.0f, 0.0f};
    // This alert began with a hit, so it runs alert_on_damage_ticks.
    bool alerted_by_damage = false;
};

// The sentry knobs an agent runs on. These are authored per actor template
// (`ai.sentry` in an entity template), so they live on the agent rather than on
// the controller: one server ticks agents from several templates at once -- a
// patrolling walker and a stationary artillery sentry in the same wave -- and a
// single controller-wide config would silently give all of them whichever
// template the catalog's `enemy:` entry happens to name.
struct AgentSentryConfig {
    std::uint32_t alert_ticks = 90;
    // The alert after being hit (AI plan D6): long enough to turn round, not
    // the warning alert_ticks gives a target that has not fired yet.
    std::uint32_t alert_on_damage_ticks = 15;
    std::uint32_t forget_ticks = 150;
    std::uint32_t ballistic_retry_cooldown_ticks = 30;
    std::uint32_t patrol_rotation_interval_ticks = 30;
    float patrol_rotation_min_degrees = 15.0f;
    float patrol_rotation_max_degrees = 30.0f;
    float move_speed_meters_per_second = 0.0f;
    std::uint16_t weapon_id = UINT16_MAX;
    BallisticAimProfile ballistic_aim;
    // The weapon's charge time when its fire action charges (D21), else 0.
    std::uint32_t charge_ticks = 0;
    std::uint16_t animation_idle = 0;
    std::uint16_t animation_attack = 0;
};

// An agent that is still walking out of whatever spawned it.
//
// While this is active the agent is not the AI's: the controllers skip it, the
// entry pass drives it, and its movement mask is the spawner's rather than its
// template's. Everything here is resolved when the request is attached, so the
// pass itself needs nothing but the agent.
struct AgentEntryRuntimeState {
    bool active = false;
    // Where it is walking to, in world space.
    KernelVec3 exit{0.0f, 0.0f, 0.0f};
    // Ticks to wait before setting off, which is how a wave leaves one at a
    // time. It stands still inside the carrier until then.
    std::uint32_t hold_ticks = 0;
    // The budget for the walk itself. At zero the agent is released wherever it
    // stands: a door blocked by something the walk cannot push aside must not
    // strand it.
    std::uint32_t remaining_ticks = 0;
    // The template's own mask, put back the moment the walk ends. Zero is a
    // legal value -- it means the engine default -- so `active` is what says
    // whether a restore is owed, not this.
    std::uint32_t restore_movement_collision_mask = 0;
};

// An authored route a garrison walks while idle: the waypoints joined by
// navmesh corners, walked to the end and back.
struct AgentRoutePatrolState {
    std::vector<KernelVec3> path;
    std::size_t next = 0;
    // +1 walking towards the last point, -1 back towards the first.
    int step = 1;
};

struct AgentRuntimeState {
    std::uint32_t net_id = 0;
    // Selects which controller and which tuning this agent runs under; agents
    // spawned from different templates share one runtime list.
    std::uint32_t actor_template_id = 0;
    // Resolved once, when the runtime first discovers the agent.
    AgentControllerRole controller_role = AgentControllerRole::kNone;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t hp = kAgentInitialHp;
    std::uint16_t max_hp = kAgentInitialHp;
    // Legacy ABI mirror only; AI intent and presentation do not depend on it.
    std::uint16_t animation_state = 0;
    std::uint32_t target_player_net_id = 0;
    std::uint32_t next_input_seq = 1;
    std::uint32_t next_action_instance_id = 1;
    // A charging attack (D21): the action instance being charged and how many
    // held inputs it has had since the press.
    std::uint32_t charging_action_instance_id = 0;
    std::uint32_t charge_inputs = 0;
    AgentSentryRuntimeState sentry{};
    // Resolved once from this agent's own actor template when the runtime first
    // discovers it; see AgentRuntimeManager::sync_agents_from_kernel.
    AgentSentryConfig sentry_config{};
    // Garrison only: the route its spawn source gave it, walked whenever it
    // has nothing to fight. Empty for a garrison that stands its ground.
    AgentRoutePatrolState route_patrol{};
    // Chaser only: true once the agent has closed inside its stop distance and
    // is holding position, until the target opens the gap back up.
    bool chase_holding = false;
    // Chaser only. Ticks until a leap it started lands, counted down once per
    // controller tick; zero when it is not leaping. And ticks until it may
    // start another.
    std::uint32_t leap_ticks_to_landing = 0;
    std::uint32_t leap_cooldown_ticks = 0;
    // Chaser only. Ticks of wind-up left before a charge is released; ticks
    // the run it released may still take (zero once the kernel's flag shows
    // it ended); the line it runs along; and ticks until another may start.
    std::uint32_t charge_windup_ticks = 0;
    std::uint32_t charge_ticks_left = 0;
    KernelVec3 charge_direction{0.0f, 0.0f, 0.0f};
    std::uint32_t charge_cooldown_ticks = 0;
    // Chaser only (AI plan P4b): the weapon skill it is using, each skill's
    // cooldown, whether to choose again (after a shot, or once the one in hand
    // stops being usable), and its own random stream for the weighted choice,
    // seeded from its net id so a run replays.
    std::uint8_t weapon_skill = 0;
    std::array<std::uint32_t, kMaxAgentSkills> skill_cooldown_ticks{};
    bool choose_weapon = true;
    std::uint32_t skill_random = 0;
    // The last action instance it saw start: a new one that is not a reload is
    // the weapon in hand going off, which is when its cooldown starts and the
    // next draw is due -- not when the input asking for it was sent, which an
    // action still recovering ignores.
    std::uint32_t seen_action_instance_id = 0;
    // Item skills (AI plan P5): whether its template has any, the inventory
    // they live in (made the first time they are needed), each skill's item
    // instance, cooldown and used-once bit, and what on_spawn / on_state_enter
    // compare against.
    bool has_item_skills = false;
    KernelInventoryContainerId item_container = 0;
    std::array<KernelItemInstanceId, kMaxAgentSkills> item_instances{};
    std::array<std::uint32_t, kMaxAgentSkills> item_cooldown_ticks{};
    std::uint32_t items_used_once = 0;
    bool items_seen_spawn = false;
    AgentSentryState items_last_state = AgentSentryState::kIdle;
    std::uint32_t next_item_request = 1;
    // Hunter only (AI plan P6a): its tuning, its target, and the path the
    // manager last found towards it, with the ticks until it looks again.
    AgentHunterTuning hunter{};
    std::uint32_t hunt_target = 0;
    std::vector<KernelVec3> seek_path;
    std::size_t seek_next = 0;
    std::uint32_t seek_repath_in = 0;
    // Its vision cone is switched off while nothing hostile is near enough to
    // see (D11). The cone's range, read once when the agent is discovered; 0
    // when it has none, which leaves vision alone.
    bool vision_off = false;
    float vision_reach_meters = 0.0f;
    AgentPatrolRuntimeState patrol{};
    AgentEntryRuntimeState entry{};
    // Whether the agent was engaged when its controller last ran, so that an
    // engagement starting can be told from one carrying on. Only kept for
    // agents that carry an on_alert spawner; nothing else reads it.
    bool engaged_last_tick = false;
};

// Where each agent sits in the manager's list.
//
// Every phase after the resync used to find an agent by walking that list:
// the squad runtime does it three or four times per member, and the resync
// itself did it once per actor the kernel reported. All of them were quadratic
// in the population for a lookup that is a hash.
//
// It describes exactly one list and goes stale the moment that list is
// reordered or shortened, so whoever owns the list rebuilds it at every
// mutation rather than at every read.
class AgentIndex {
public:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    void rebuild(const std::vector<AgentRuntimeState>& agents) {
        // Sorted pairs rather than a hash map, because this is rebuilt every
        // tick and thrown away: an unordered_map allocates a node per entry on
        // every rebuild, which measured slower than the linear scan it was
        // replacing at the population this server actually runs. Clearing a
        // vector keeps its buffer, so after the first tick nothing allocates.
        entries_.clear();
        entries_.reserve(agents.size());
        for (std::size_t position = 0; position < agents.size(); ++position) {
            entries_.push_back(Entry{
                agents[position].net_id,
                static_cast<std::uint32_t>(position)});
        }
        std::sort(
            entries_.begin(),
            entries_.end(),
            [](const Entry& lhs, const Entry& rhs) {
                return lhs.net_id < rhs.net_id;
            });
    }

    std::size_t find(std::uint32_t net_id) const {
        const auto found = std::lower_bound(
            entries_.begin(),
            entries_.end(),
            net_id,
            [](const Entry& entry, std::uint32_t value) {
                return entry.net_id < value;
            });
        if (found == entries_.end() || found->net_id != net_id) {
            return kNotFound;
        }
        return found->position;
    }

private:
    struct Entry {
        std::uint32_t net_id = 0;
        std::uint32_t position = 0;
    };

    std::vector<Entry> entries_;
};

// The agents one controller drives this tick.
//
// Pointers into the manager's own list rather than copies of it. A controller
// mutates the entry it was handed, so a batch of copies had to be walked
// afterwards and matched back by net_id -- another quadratic loop, on top of
// copying a 200-byte struct per agent in each direction.
using AgentBatch = std::vector<AgentRuntimeState*>;

// Every agent in `agents`, as one batch. The manager splits its list by actor
// template and never uses this; it is for callers driving a single controller
// over a single list, which is what the tests and benches do.
inline AgentBatch whole_batch(std::vector<AgentRuntimeState>* agents) {
    AgentBatch batch;
    if (agents == nullptr) {
        return batch;
    }
    batch.reserve(agents->size());
    for (AgentRuntimeState& agent : *agents) {
        batch.push_back(&agent);
    }
    return batch;
}

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_H_
