#ifndef GAME_SERVER_AGENT_RUNTIME_H_
#define GAME_SERVER_AGENT_RUNTIME_H_

#include <cstdint>
#include <vector>

#include "game_server/ballistic_aim.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

inline constexpr std::uint16_t kEntityTypeActor = 1;
inline constexpr std::uint16_t kActorTypePlayer = KernelActorType_Player;
inline constexpr std::uint16_t kActorTypeAgent = KernelActorType_Agent;

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
};

// A patrol route as a polyline the agent walks once, first waypoint to last,
// and then reports finished. It does not loop: the squad model is a one-shot
// route across the world, so "finished" is what hands the agent to the despawn
// path rather than what starts a second lap.
//
// Empty is the normal state for every agent that is not patrolling, and an
// empty route leaves the chaser behaving exactly as it did before routes
// existed.
//
// The waypoint list lives on the agent only while a patrol is one agent. It
// belongs to the squad, and moves there once squads exist; until then this
// costs an allocation on each of the two copies AgentRuntimeManager makes per
// tick.
struct AgentPatrolRuntimeState {
    std::vector<KernelVec3> waypoints;
    // The waypoint being walked toward. Equal to waypoints.size() once the
    // route has been walked out.
    std::size_t next_waypoint = 0;
    bool route_complete = false;
    // Where the agent stood when it broke off the route to give chase. The
    // leash is measured from here rather than from the route, so "how far a
    // pursuit may drag a patrol" is a property of the pursuit and does not
    // change as the route runs on ahead.
    KernelVec3 leash_anchor{0.0f, 0.0f, 0.0f};
};

struct AgentSentryRuntimeState {
    std::uint32_t self_id = 0;
    AgentSentryState state = AgentSentryState::kIdle;
    std::uint32_t state_ticks = 0;
    std::uint32_t lost_target_ticks = 0;
    std::uint32_t patrol_rotation_tick = 0;
    std::uint32_t patrol_rotation_step = 0;
    std::uint32_t ballistic_retry_ticks = 0;
    std::uint32_t target = 0;
};

// The sentry knobs an agent runs on. These are authored per actor template
// (`ai.sentry` in an entity template), so they live on the agent rather than on
// the controller: one server ticks agents from several templates at once -- a
// patrolling walker and a stationary artillery sentry in the same wave -- and a
// single controller-wide config would silently give all of them whichever
// template the catalog's `enemy:` entry happens to name.
struct AgentSentryConfig {
    std::uint32_t alert_ticks = 90;
    std::uint32_t forget_ticks = 150;
    std::uint32_t ballistic_retry_cooldown_ticks = 30;
    std::uint32_t patrol_rotation_interval_ticks = 30;
    float patrol_rotation_min_degrees = 15.0f;
    float patrol_rotation_max_degrees = 30.0f;
    bool passive_patrol = false;
    float patrol_extent_x_meters = 0.0f;
    float patrol_input_magnitude = 0.0f;
    float move_speed_meters_per_second = 0.0f;
    std::uint16_t weapon_id = UINT16_MAX;
    BallisticAimProfile ballistic_aim;
    std::uint16_t animation_idle = 0;
    std::uint16_t animation_attack = 0;
};

struct AgentRuntimeState {
    std::uint32_t net_id = 0;
    // Selects which controller and which tuning this agent runs under; agents
    // spawned from different templates share one runtime list.
    std::uint32_t actor_template_id = 0;
    KernelVec3 position{0.0f, 0.0f, 0.0f};
    KernelVec3 patrol_anchor{0.0f, 0.0f, 0.0f};
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t hp = kAgentInitialHp;
    std::uint16_t max_hp = kAgentInitialHp;
    // Legacy ABI mirror only; AI intent and presentation do not depend on it.
    std::uint16_t animation_state = 0;
    std::uint32_t target_player_net_id = 0;
    std::uint32_t next_input_seq = 1;
    std::uint32_t next_action_instance_id = 1;
    AgentSentryRuntimeState sentry{};
    // Resolved once from this agent's own actor template when the runtime first
    // discovers it; see AgentRuntimeManager::sync_agents_from_kernel.
    AgentSentryConfig sentry_config{};
    int patrol_direction = 1;
    // Chaser only: true once the agent has closed inside its stop distance and
    // is holding position, until the target opens the gap back up.
    bool chase_holding = false;
    AgentPatrolRuntimeState patrol{};
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_AGENT_RUNTIME_H_
