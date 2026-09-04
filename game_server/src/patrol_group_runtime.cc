#include "game_server/src/patrol_group_runtime.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game_server/src/agent_steering.h"

namespace network_example::game_server {
namespace {

constexpr float kEpsilon = 0.0001f;

// The squad's frame: +X along the direction of travel, +Z to its right. Applied
// to an authored offset so a formation keeps its shape through a turn.
KernelVec3 rotate_into_heading(
    const KernelVec3& offset,
    float heading_x,
    float heading_z) {
    const float length =
        std::sqrt(heading_x * heading_x + heading_z * heading_z);
    if (length <= kEpsilon) {
        return offset;
    }
    const float forward_x = heading_x / length;
    const float forward_z = heading_z / length;
    return KernelVec3{
        offset.x * forward_x - offset.z * forward_z,
        offset.y,
        offset.x * forward_z + offset.z * forward_x,
    };
}

bool is_engaged(AgentSentryState state) {
    return state == AgentSentryState::kAlert || state == AgentSentryState::kAttack;
}

AgentRuntimeState* find_agent(
    std::vector<AgentRuntimeState>* agents,
    const AgentIndex& index,
    std::uint32_t net_id) {
    const std::size_t position = index.find(net_id);
    return position == AgentIndex::kNotFound ? nullptr : &(*agents)[position];
}

}  // namespace

std::uint32_t PatrolGroupRuntime::create_group(
    std::uint32_t definition_id,
    std::vector<KernelVec3> waypoints,
    const KernelVec3& origin,
    const std::vector<std::uint32_t>& member_net_ids,
    const std::vector<KernelVec3>& member_offsets,
    PatrolGroupTuning tuning) {
    if (waypoints.empty() || member_net_ids.empty() ||
        member_net_ids.size() != member_offsets.size()) {
        return 0;
    }
    PatrolGroup group;
    group.group_id = next_group_id_++;
    group.definition_id = definition_id;
    group.waypoints = std::move(waypoints);
    group.cursor = origin;
    group.member_net_ids = member_net_ids;
    group.member_offsets = member_offsets;
    group.tuning = tuning;
    groups_.push_back(std::move(group));
    return groups_.back().group_id;
}

void PatrolGroupRuntime::tick(
    std::vector<AgentRuntimeState>* agents,
    const AgentIndex& index,
    float delta_seconds) {
    if (agents == nullptr || delta_seconds <= 0.0f) {
        return;
    }

    // A squad that has lost everyone is not a squad, and leaving it in the list
    // would hold a slot against its definition's live ceiling for good.
    groups_.erase(
        std::remove_if(
            groups_.begin(),
            groups_.end(),
            [&agents, &index](const PatrolGroup& group) {
                return std::none_of(
                    group.member_net_ids.begin(),
                    group.member_net_ids.end(),
                    [&agents, &index](std::uint32_t net_id) {
                        return find_agent(agents, index, net_id) != nullptr;
                    });
            }),
        groups_.end());

    for (PatrolGroup& group : groups_) {
        // Casualties first, so a dead member neither holds the squad in place
        // nor keeps a slot nobody is standing in.
        for (std::size_t remaining = group.member_net_ids.size(); remaining > 0;
             --remaining) {
            const std::size_t member = remaining - 1;
            if (find_agent(agents, index, group.member_net_ids[member]) !=
                nullptr) {
                continue;
            }
            group.member_net_ids.erase(group.member_net_ids.begin() + member);
            group.member_offsets.erase(group.member_offsets.begin() + member);
        }

        // One member in a fight stops the squad. The alternative -- walking on
        // and leaving them to catch up -- reads as a squad abandoning its own,
        // and it makes the leash useless, because the slot being chased would
        // run away as fast as the pursuit dragged the member off it.
        group.holding = false;
        for (const std::uint32_t net_id : group.member_net_ids) {
            const AgentRuntimeState* member = find_agent(agents, index, net_id);
            if (member != nullptr && is_engaged(member->sentry.state)) {
                group.holding = true;
                break;
            }
        }

        float heading_x = 1.0f;
        float heading_z = 0.0f;
        if (group.next_waypoint < group.waypoints.size()) {
            const KernelVec3& waypoint = group.waypoints[group.next_waypoint];
            heading_x = waypoint.x - group.cursor.x;
            heading_z = waypoint.z - group.cursor.z;
        }

        if (!group.holding && !group.route_complete) {
            float remaining = group.tuning.advance_speed_meters_per_second * delta_seconds;
            // A loop rather than one step, so a tick long enough to cross a
            // whole leg does not leave the cursor stalled on a waypoint it has
            // already passed.
            while (remaining > 0.0f &&
                   group.next_waypoint < group.waypoints.size()) {
                const KernelVec3& waypoint = group.waypoints[group.next_waypoint];
                const float distance =
                    agent_steering::horizontal_distance(group.cursor, waypoint);
                if (distance <= group.tuning.waypoint_radius_meters) {
                    ++group.next_waypoint;
                    continue;
                }
                const float step = std::min(remaining, distance);
                group.cursor.x += (waypoint.x - group.cursor.x) / distance * step;
                group.cursor.z += (waypoint.z - group.cursor.z) / distance * step;
                group.cursor.y = waypoint.y;
                remaining -= step;
                heading_x = waypoint.x - group.cursor.x;
                heading_z = waypoint.z - group.cursor.z;
            }
            if (group.next_waypoint >= group.waypoints.size()) {
                group.route_complete = true;
            }
        }
        if (group.route_complete && !group.holding) {
            ++group.ticks_since_route_complete;
        }

        for (std::size_t member = 0; member < group.member_net_ids.size();
             ++member) {
            AgentRuntimeState* agent =
                find_agent(agents, index, group.member_net_ids[member]);
            if (agent == nullptr) {
                continue;
            }
            const KernelVec3 offset = rotate_into_heading(
                group.member_offsets[member], heading_x, heading_z);
            agent->patrol.group_id = group.group_id;
            agent->patrol.slot = KernelVec3{
                group.cursor.x + offset.x,
                group.cursor.y + offset.y,
                group.cursor.z + offset.z,
            };
            agent->patrol.has_slot = true;
        }
    }
}

const std::vector<PatrolGroup>& PatrolGroupRuntime::groups() const {
    return groups_;
}

const PatrolGroup* PatrolGroupRuntime::find_group(std::uint32_t group_id) const {
    const auto found = std::find_if(
        groups_.begin(),
        groups_.end(),
        [group_id](const PatrolGroup& group) {
            return group.group_id == group_id;
        });
    return found == groups_.end() ? nullptr : &*found;
}

void PatrolGroupRuntime::remove_group(std::uint32_t group_id) {
    groups_.erase(
        std::remove_if(
            groups_.begin(),
            groups_.end(),
            [group_id](const PatrolGroup& group) {
                return group.group_id == group_id;
            }),
        groups_.end());
}

}  // namespace network_example::game_server
