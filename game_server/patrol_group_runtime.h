#ifndef GAME_SERVER_PATROL_GROUP_RUNTIME_H_
#define GAME_SERVER_PATROL_GROUP_RUNTIME_H_

#include <cstdint>
#include <vector>

#include "game_server/agent_runtime.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// Per squad, not per runtime: two definitions can want different speeds, and a
// runtime-wide setting would silently give both whichever one was constructed
// with.
struct PatrolGroupTuning {
    // How fast the squad's cursor walks the route. Below the members' move
    // speed on purpose: a cursor that moves as fast as they do leaves them
    // permanently chasing it, and every slot permanently unreached.
    float advance_speed_meters_per_second = 1.25f;
    // How close the cursor has to get to a waypoint to move on to the next.
    float waypoint_radius_meters = 0.5f;
};

// A squad walking one route together.
//
// The route lives here rather than on its members because it is one route, not
// N copies of one: a squad that stores a route per agent has to keep those
// copies agreeing, and there is no reason for them to disagree. What each
// member gets instead is a slot -- a point recomputed every tick from where the
// squad is and where in the formation that member stands -- and the controller
// walks to it without knowing a route exists.
//
// A group is server-side gameplay state. It is a plain struct held by
// game_server, not a component on an entity, because nothing in the kernel
// needs to know that squads are a thing.
struct PatrolGroup {
    std::uint32_t group_id = 0;
    // Which authored patrol produced this squad, so a definition can be held to
    // its own live-squad ceiling without counting anyone else's.
    std::uint32_t definition_id = 0;
    // Walked once, first to last. The squad model is a one-shot route across
    // the world, so reaching the end is what will hand the group to a despawn
    // rather than what starts another lap.
    std::vector<KernelVec3> waypoints;
    std::size_t next_waypoint = 0;
    // Where the squad is along its route. Not the average of its members: a
    // member dragged away by a fight would otherwise pull the whole formation
    // sideways.
    KernelVec3 cursor{0.0f, 0.0f, 0.0f};
    bool route_complete = false;
    // Ticks since the route was walked out, which is what a retirement linger
    // counts. Frozen while the squad is fighting, so a squad that finishes its
    // route mid-engagement does not have its linger run out during the fight.
    std::uint32_t ticks_since_route_complete = 0;
    // Whether the squad held station last tick because one of its own was
    // fighting. Kept for the caller to read; the tick recomputes it.
    bool holding = false;
    // Members, and where each one stands relative to the cursor. Parallel
    // vectors, indexed together. The offset is in the squad's frame, +X along
    // the direction of travel, so a formation keeps its shape when the route
    // turns instead of shearing into world axes.
    std::vector<std::uint32_t> member_net_ids;
    std::vector<KernelVec3> member_offsets;
    PatrolGroupTuning tuning{};
};


// Owns the live squads and, once per tick, tells every member where to stand.
class PatrolGroupRuntime {
public:
    // `origin` is where the squad starts walking from -- normally where it
    // spawned, which is not the first waypoint, so the first leg is walked
    // rather than skipped. Returns the group id, or 0 if the request was not
    // usable.
    std::uint32_t create_group(
        std::uint32_t definition_id,
        std::vector<KernelVec3> waypoints,
        const KernelVec3& origin,
        const std::vector<std::uint32_t>& member_net_ids,
        const std::vector<KernelVec3>& member_offsets,
        PatrolGroupTuning tuning = {});

    // Advances each squad and writes its members' slots. Members that no longer
    // appear in `agents` are dropped, which is how a squad shrinks as it takes
    // casualties, and a squad that loses its last member is dropped with them.
    void tick(std::vector<AgentRuntimeState>* agents, float delta_seconds);

    const std::vector<PatrolGroup>& groups() const;
    const PatrolGroup* find_group(std::uint32_t group_id) const;
    void remove_group(std::uint32_t group_id);

private:
    std::vector<PatrolGroup> groups_;
    std::uint32_t next_group_id_ = 1;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_GROUP_RUNTIME_H_
