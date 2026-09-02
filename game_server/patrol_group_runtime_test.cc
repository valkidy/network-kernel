#include "game_server/patrol_group_runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kFixedDelta = 1.0f / 30.0f;

bool almost_equal(float lhs, float rhs, float tolerance = 0.01f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

float distance(const KernelVec3& lhs, const KernelVec3& rhs) {
    const float delta_x = lhs.x - rhs.x;
    const float delta_z = lhs.z - rhs.z;
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

network_example::game_server::AgentRuntimeState agent(std::uint32_t net_id) {
    network_example::game_server::AgentRuntimeState state;
    state.net_id = net_id;
    return state;
}

// The wedge the cases below use: a leader on the cursor and two behind it, one
// to each side. Asymmetric across Z on purpose, so a rotation that is applied
// backwards is visible rather than symmetric-looking.
std::vector<KernelVec3> wedge_offsets() {
    return {
        KernelVec3{0.0f, 0.0f, 0.0f},
        KernelVec3{-1.0f, 0.0f, -1.5f},
        KernelVec3{-1.0f, 0.0f, 1.5f},
    };
}

}  // namespace

int main() {
    using network_example::game_server::AgentRuntimeState;
    using network_example::game_server::AgentSentryState;
    using network_example::game_server::PatrolGroup;
    using network_example::game_server::PatrolGroupRuntime;
    using network_example::game_server::PatrolGroupTuning;

    PatrolGroupTuning tuning;
    tuning.advance_speed_meters_per_second = 3.0f;
    tuning.waypoint_radius_meters = 0.5f;
    PatrolGroupRuntime runtime(tuning);

    std::vector<AgentRuntimeState> agents{agent(1), agent(2), agent(3)};

    // An L-shaped route, because a straight one cannot show whether the
    // formation turns with it.
    const std::uint32_t group_id = runtime.create_group(
        {KernelVec3{10.0f, 0.0f, 0.0f}, KernelVec3{10.0f, 0.0f, 10.0f}},
        KernelVec3{0.0f, 0.0f, 0.0f},
        {1u, 2u, 3u},
        wedge_offsets());
    require(group_id != 0);

    // A route with no waypoints, or a member list that does not line up with
    // the offsets, is not a squad.
    require(runtime.create_group({}, {}, {1u}, {KernelVec3{}}) == 0);
    require(
        runtime.create_group(
            {KernelVec3{1.0f, 0.0f, 0.0f}}, {}, {1u, 2u}, {KernelVec3{}}) == 0);

    // One second of walking, along +X.
    for (int tick = 0; tick < 30; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    const PatrolGroup* group = runtime.find_group(group_id);
    require(group != nullptr);
    require(almost_equal(group->cursor.x, 3.0f, 0.05f));
    require(almost_equal(group->cursor.z, 0.0f));
    require(!group->holding);
    require(!group->route_complete);

    // Every member is told where to stand, and the leader stands on the cursor.
    for (const AgentRuntimeState& member : agents) {
        require(member.patrol.has_slot);
        require(member.patrol.group_id == group_id);
    }
    require(almost_equal(agents[0].patrol.slot.x, group->cursor.x));
    require(almost_equal(agents[0].patrol.slot.z, group->cursor.z));
    // Heading is +X here, so the offsets are unrotated.
    require(almost_equal(agents[1].patrol.slot.x, group->cursor.x - 1.0f));
    require(almost_equal(agents[1].patrol.slot.z, group->cursor.z - 1.5f));
    require(almost_equal(agents[2].patrol.slot.z, group->cursor.z + 1.5f));

    // One member in a fight stops the squad where it stands.
    agents[1].sentry.state = AgentSentryState::kAlert;
    const KernelVec3 held_at = group->cursor;
    for (int tick = 0; tick < 30; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    require(group->holding);
    require(almost_equal(group->cursor.x, held_at.x));
    require(almost_equal(group->cursor.z, held_at.z));

    // A member walking back is not a member fighting, so the squad moves on
    // while it returns -- which is the whole reason a slot has to be a moving
    // target rather than the spot the member left.
    agents[1].sentry.state = AgentSentryState::kReturn;
    for (int tick = 0; tick < 15; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    require(!group->holding);
    require(group->cursor.x > held_at.x + 1.0f);
    agents[1].sentry.state = AgentSentryState::kIdle;

    // Round the corner. The formation has to turn with the route: after the
    // heading swings to +Z the member authored behind-and-left must be behind
    // and left of the new heading, not still sitting at a world-axis offset.
    for (int tick = 0; tick < 120; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    require(group->next_waypoint == 1);
    require(almost_equal(group->cursor.x, 10.0f, 0.6f));
    require(group->cursor.z > 1.0f);
    // Stated as the shape rather than as coordinates. The cursor turns the
    // corner up to waypoint_radius short of the waypoint, so the new heading is
    // a couple of degrees off +Z and every exact figure here would be a figure
    // about that shortfall rather than about the rotation.
    //
    // Behind-and-left of a +Z heading is +X and behind is -Z, so the member
    // authored behind-left has swung to the far side of the cursor, its partner
    // to the near side, and both are still behind.
    require(agents[1].patrol.slot.x > group->cursor.x + 1.0f);
    require(agents[1].patrol.slot.z < group->cursor.z);
    require(agents[2].patrol.slot.x < group->cursor.x - 1.0f);
    require(agents[2].patrol.slot.z < group->cursor.z);
    // Rotation is rigid: whatever the heading, a member stands exactly as far
    // from the cursor as its offset is long.
    require(almost_equal(
        distance(agents[1].patrol.slot, group->cursor), 1.8028f, 0.02f));
    require(almost_equal(
        distance(agents[2].patrol.slot, group->cursor), 1.8028f, 0.02f));

    // Walking the route out finishes it, once, and stops the cursor.
    for (int tick = 0; tick < 200; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    require(group->route_complete);
    const KernelVec3 finished_at = group->cursor;
    for (int tick = 0; tick < 30; ++tick) {
        runtime.tick(&agents, kFixedDelta);
    }
    require(almost_equal(group->cursor.x, finished_at.x));
    require(almost_equal(group->cursor.z, finished_at.z));

    // A casualty leaves the squad. Without this a dead member's slot would be
    // kept, and worse, a dead member stuck in kAlert would hold the squad in
    // place for good.
    agents.erase(agents.begin() + 1);
    runtime.tick(&agents, kFixedDelta);
    require(group->member_net_ids.size() == 2u);
    require(group->member_net_ids[0] == 1u);
    require(group->member_net_ids[1] == 3u);
    // The survivors keep the offsets they were authored with, rather than
    // sliding up the list into someone else's place in the formation.
    require(group->member_offsets[1].z > 0.0f);

    runtime.remove_group(group_id);
    require(runtime.find_group(group_id) == nullptr);
    require(runtime.groups().empty());
    return 0;
}
