#include "game_server/src/patrol_targeting.h"

#include <algorithm>
#include <numeric>
#include <unordered_set>

namespace network_example::game_server {

namespace {

float horizontal_distance_squared(const KernelVec3& from, const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    return delta_x * delta_x + delta_z * delta_z;
}

}  // namespace

std::vector<TargetPlayer> target_players_from(const ActorStateView& actors) {
    std::vector<TargetPlayer> players;
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        const KernelServerEntityState& state = actors.states[index];
        if (state.valid == 0u || state.actor_type != kActorTypePlayer ||
            state.hp == 0u || (state.visual_flags & KERNEL_VISUAL_FLAG_DEAD) != 0u) {
            continue;
        }
        players.push_back(TargetPlayer{state.net_id, state.position});
    }
    return players;
}

std::vector<PlayerGroup> group_players(
    const std::vector<TargetPlayer>& players,
    float radius) {
    // Sorted first, so the union below and everything after it is independent
    // of the order the caller listed players in.
    std::vector<TargetPlayer> sorted = players;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const TargetPlayer& lhs, const TargetPlayer& rhs) {
            return lhs.net_id < rhs.net_id;
        });

    std::vector<std::size_t> parent(sorted.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&parent](std::size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const float radius_squared = radius * radius;
    for (std::size_t lhs = 0; lhs < sorted.size(); ++lhs) {
        for (std::size_t rhs = lhs + 1; rhs < sorted.size(); ++rhs) {
            if (horizontal_distance_squared(sorted[lhs].position, sorted[rhs].position) <=
                radius_squared) {
                // The lower index wins, so a group's root is its lowest net_id.
                const std::size_t lhs_root = root(lhs);
                const std::size_t rhs_root = root(rhs);
                parent[std::max(lhs_root, rhs_root)] = std::min(lhs_root, rhs_root);
            }
        }
    }

    // Walked in net_id order, so members come out in net_id order too.
    std::vector<PlayerGroup> groups;
    std::unordered_map<std::size_t, std::size_t> group_of_root;
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        const std::size_t group_root = root(index);
        auto found = group_of_root.find(group_root);
        if (found == group_of_root.end()) {
            found = group_of_root.emplace(group_root, groups.size()).first;
            groups.emplace_back();
        }
        PlayerGroup& group = groups[found->second];
        group.member_net_ids.push_back(sorted[index].net_id);
        group.member_positions.push_back(sorted[index].position);
    }
    for (PlayerGroup& group : groups) {
        KernelVec3 sum{0.0f, 0.0f, 0.0f};
        for (const KernelVec3& position : group.member_positions) {
            sum.x += position.x;
            sum.y += position.y;
            sum.z += position.z;
        }
        const float count = static_cast<float>(group.member_positions.size());
        group.centroid = KernelVec3{sum.x / count, sum.y / count, sum.z / count};
    }
    // Stable, and groups were created in order of their lowest net_id, so ties
    // in size keep that order.
    std::stable_sort(
        groups.begin(),
        groups.end(),
        [](const PlayerGroup& lhs, const PlayerGroup& rhs) {
            return lhs.member_net_ids.size() > rhs.member_net_ids.size();
        });
    return groups;
}

std::uint32_t squads_near_group(
    const PlayerGroup& group,
    const std::vector<KernelVec3>& squad_cursors,
    float radius) {
    const float radius_squared = radius * radius;
    std::uint32_t near = 0;
    for (const KernelVec3& cursor : squad_cursors) {
        for (const KernelVec3& member : group.member_positions) {
            if (horizontal_distance_squared(member, cursor) <= radius_squared) {
                ++near;
                break;
            }
        }
    }
    return near;
}

void PlayerThreatTracker::observe(
    std::uint64_t tick,
    const std::vector<TargetPlayer>& players,
    const std::vector<KernelVec3>& squad_cursors,
    float radius) {
    const float radius_squared = radius * radius;
    std::unordered_set<std::uint32_t> present;
    for (const TargetPlayer& player : players) {
        present.insert(player.net_id);
        bool threatened = false;
        for (const KernelVec3& cursor : squad_cursors) {
            if (horizontal_distance_squared(player.position, cursor) <= radius_squared) {
                threatened = true;
                break;
            }
        }
        const auto [entry, inserted] = last_threatened_.try_emplace(player.net_id, tick);
        if (!inserted && threatened) {
            entry->second = tick;
        }
    }
    for (auto entry = last_threatened_.begin(); entry != last_threatened_.end();) {
        entry = present.contains(entry->first) ? std::next(entry)
                                               : last_threatened_.erase(entry);
    }
}

std::uint64_t PlayerThreatTracker::group_starved_ticks(
    const PlayerGroup& group,
    std::uint64_t tick) const {
    std::uint64_t latest = 0;
    bool any = false;
    for (const std::uint32_t net_id : group.member_net_ids) {
        const auto found = last_threatened_.find(net_id);
        if (found == last_threatened_.end()) {
            continue;
        }
        latest = any ? std::max(latest, found->second) : found->second;
        any = true;
    }
    // A group nobody has observed yet has not waited for anything.
    if (!any || latest >= tick) {
        return 0;
    }
    return tick - latest;
}

std::uint64_t PlayerThreatTracker::last_threatened_tick(std::uint32_t net_id) const {
    const auto found = last_threatened_.find(net_id);
    return found == last_threatened_.end() ? 0u : found->second;
}

}  // namespace network_example::game_server
