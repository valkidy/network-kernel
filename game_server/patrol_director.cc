#include "game_server/patrol_director.h"

#include "game_server/spawn_sampling.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// The lifecycle command's `reason` is an opaque caller-chosen tag -- the kernel
// stores and reports it without attaching any meaning -- so this only has to be
// distinguishable from whatever else destroys an entity.
constexpr std::uint32_t kPatrolRetiredReason = 2;
// A route of two points is the whole of the degenerate navigation this ships
// with: a straight chord, walked with the character controller absorbing slopes
// and steps. //game_server:patrol_nav_bench measured what that costs -- on flat
// ground nothing, on rolling ground a straight chord is walkable 30% of the
// time, and on a built level 17%, falling to zero past 100 m. The shipping map
// is a flat plane, so this is exactly a pathed route today and badly wrong the
// day the map is not. The waypoint list is a list rather than a pair of
// endpoints so that swapping in Detour corners is a change to this function.

KernelVec3 rotate_into_heading(
    const KernelVec3& offset,
    const KernelVec3& from,
    const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    const float length = std::sqrt(delta_x * delta_x + delta_z * delta_z);
    if (length <= 0.0001f) {
        return offset;
    }
    const float forward_x = delta_x / length;
    const float forward_z = delta_z / length;
    return KernelVec3{
        offset.x * forward_x - offset.z * forward_z,
        offset.y,
        offset.x * forward_z + offset.z * forward_x,
    };
}

}  // namespace

std::string validate_patrol_definition(const PatrolDefinitionConfig& definition) {
    const std::string composition_error = validate_spawn_composition(
        definition.composition, definition.count_min, definition.count_max);
    if (!composition_error.empty()) {
        return "patrol " + composition_error;
    }
    const std::string area_error = validate_spawn_area(definition.area);
    if (!area_error.empty()) {
        return "patrol " + area_error;
    }
    return {};
}

std::vector<KernelVec3> formation_offsets(std::uint32_t count, float spacing) {
    std::vector<KernelVec3> offsets;
    offsets.reserve(count);
    // Ranks of three behind the squad's point, centred across it. Three is a
    // shape, not a rule: the offsets are data by the time anything reads them.
    constexpr std::uint32_t kRankWidth = 3;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto rank = static_cast<float>(index / kRankWidth);
        const auto file = static_cast<float>(index % kRankWidth) - 1.0f;
        offsets.push_back(KernelVec3{-rank * spacing, 0.0f, file * spacing});
    }
    return offsets;
}

PatrolDirector::PatrolDirector(
    std::vector<PatrolDefinitionConfig> definitions,
    PatrolBudgetConfig budget)
    : definitions_(std::move(definitions)),
      budget_(budget),
      runtimes_(definitions_.size()) {
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        // Staggered by one interval rather than firing on tick zero, so that a
        // server which has just started does not put every patrol in the world
        // in the same instant.
        runtimes_[index].ticks_until_spawn = definitions_[index].interval_ticks;
    }
}

void PatrolDirector::tick(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups,
    const PatrolNavigation* navigation) {
    if (kernel == nullptr || groups == nullptr) {
        return;
    }
    retire_finished_patrols(kernel, groups);

    std::uint32_t live_agents = 0;
    for (const PatrolGroup& group : groups->groups()) {
        live_agents += static_cast<std::uint32_t>(group.member_net_ids.size());
    }
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        const PatrolDefinitionConfig& definition = definitions_[index];
        DefinitionRuntime& runtime = runtimes_[index];
        if (definition.trigger != PatrolSpawnTrigger::kFixedInterval) {
            continue;
        }
        if (runtime.ticks_until_spawn > 0) {
            --runtime.ticks_until_spawn;
            continue;
        }
        std::uint32_t live = 0;
        for (const PatrolGroup& group : groups->groups()) {
            live += group.definition_id == definition.id ? 1u : 0u;
        }
        if (live >= definition.max_live_groups) {
            // Checked without resetting the countdown, so the next opening is
            // taken as soon as it appears rather than a whole interval later.
            continue;
        }
        // Against the largest squad this definition could draw, not the one it
        // is about to: a budget that admitted a spawn and then found out it had
        // overshot would have nothing useful to do about it.
        if (budget_.max_live_agents != 0 &&
            live_agents + definition.count_max > budget_.max_live_agents) {
            continue;
        }
        if (spawn_patrol(kernel, groups, navigation, definition, &runtime)) {
            runtime.ticks_until_spawn = definition.interval_ticks;
            live_agents += definition.count_max;
        }
    }
}


namespace {

// Squared, because this is only ever compared against another distance.
float horizontal_distance_squared(const KernelVec3& from, const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    return delta_x * delta_x + delta_z * delta_z;
}

// How far the nearest player is from a point, or a negative number when there
// are no players at all -- which is not "infinitely far" for retirement
// purposes: an empty server should not sweep its patrols away, because the
// distance rule is about a squad nobody can see rather than about a squad
// nobody owns.
float nearest_player_distance(
    KernelHandle* kernel,
    const KernelVec3& from,
    std::vector<KernelServerEntityState>* buffer) {
    if (buffer->size() < 64) {
        buffer->resize(64);
    }
    while (true) {
        for (KernelServerEntityState& state : *buffer) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel,
            kEntityTypeActor,
            buffer->data(),
            static_cast<std::uint32_t>(buffer->size()));
        // Same truncation trap AgentRuntimeManager::query_actor_states
        // documents: the query reports what it wrote, never what it had, so a
        // full buffer is indistinguishable from a truncated one.
        if (count >= buffer->size() && buffer->size() < 4096) {
            buffer->resize(buffer->size() * 2);
            continue;
        }
        float nearest = -1.0f;
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelServerEntityState& state = (*buffer)[index];
            if (state.valid == 0u || state.actor_type != kActorTypePlayer) {
                continue;
            }
            const float distance = std::sqrt(
                horizontal_distance_squared(from, state.position));
            if (nearest < 0.0f || distance < nearest) {
                nearest = distance;
            }
        }
        return nearest;
    }
}

}  // namespace

void PatrolDirector::retire_finished_patrols(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups) {
    std::vector<std::uint32_t> retiring;
    std::vector<KernelServerEntityState> actor_states;
    for (const PatrolGroup& group : groups->groups()) {
        const auto definition = std::find_if(
            definitions_.begin(),
            definitions_.end(),
            [&group](const PatrolDefinitionConfig& candidate) {
                return candidate.id == group.definition_id;
            });
        if (definition == definitions_.end()) {
            continue;
        }
        // A squad in a fight is never retired, whichever rule would have
        // retired it. Despawning enemies out from under the player who is
        // shooting at them is worse than any population it would have saved.
        if (group.holding) {
            continue;
        }
        bool retire = group.route_complete &&
            group.ticks_since_route_complete >= definition->despawn_linger_ticks;
        if (!retire && definition->despawn_distance_meters > 0.0f) {
            const float nearest =
                nearest_player_distance(kernel, group.cursor, &actor_states);
            retire = nearest >= 0.0f &&
                nearest > definition->despawn_distance_meters;
        }
        if (retire) {
            retiring.push_back(group.group_id);
        }
    }

    for (const std::uint32_t group_id : retiring) {
        const PatrolGroup* group = groups->find_group(group_id);
        if (group == nullptr) {
            continue;
        }
        for (const std::uint32_t net_id : group->member_net_ids) {
            KernelEntityLifecycleCommand command{};
            command.struct_size = sizeof(command);
            command.command_type = KernelEntityLifecycleCommandType_Destroy;
            command.net_id = net_id;
            command.reason = kPatrolRetiredReason;
            Kernel_ServerEnqueueEntityLifecycle(
                kernel, KernelCommandSource_Internal, &command);
        }
        spdlog::info(
            "patrol retired group={} members={}",
            group_id,
            group->member_net_ids.size());
        groups->remove_group(group_id);
        ++retired_group_count_;
    }
}

// Draws a start and an end out of the area until one pair produces a route the
// definition will accept. With a navmesh that is a real path, corners and all;
// without one it is the straight chord, which is what the degenerate first
// version shipped and what a flat map makes indistinguishable from a path.
bool draw_route(
    const PatrolDefinitionConfig& definition,
    const PatrolNavigation* navigation,
    std::uint64_t* random_state,
    KernelVec3* origin,
    std::vector<KernelVec3>* waypoints) {
    const bool navigable = navigation != nullptr && navigation->valid();
    const std::uint32_t attempts =
        navigable ? std::max<std::uint32_t>(1u, definition.route_attempts) : 1u;
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        KernelVec3 start = sample_area(definition.area, KernelVec3{0.0f, 0.0f, 0.0f}, random_state);
        const KernelVec3 end = sample_area(definition.area, KernelVec3{0.0f, 0.0f, 0.0f}, random_state);
        if (!navigable) {
            *origin = start;
            // One waypoint, not two: the cursor starts at the origin, so the
            // chord is the single leg from there to the far end.
            *waypoints = {end};
            return true;
        }
        PatrolRouteRequest request;
        // Snapped first, so the squad is placed where it can actually stand
        // rather than wherever in the rectangle the draw landed.
        if (!navigation->snap(start, 1.0f, &start)) {
            continue;
        }
        request.start = start;
        request.end = end;
        request.max_detour_ratio = definition.max_detour_ratio;
        if (navigation->find_route(request, waypoints)) {
            *origin = start;
            return true;
        }
    }
    return false;
}

bool PatrolDirector::spawn_patrol(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups,
    const PatrolNavigation* navigation,
    const PatrolDefinitionConfig& definition,
    DefinitionRuntime* runtime) {
    // Derived from the definition's seed and how many squads it has spawned, so
    // the same definition replays identically and a test can name a squad's
    // size before it exists.
    std::uint64_t random_state =
        static_cast<std::uint64_t>(definition.seed) * 0x9e3779b97f4a7c15ull +
        static_cast<std::uint64_t>(runtime->spawn_ordinal);

    const std::uint32_t count = next_in_range(
        &random_state, definition.count_min, definition.count_max);
    const std::vector<std::uint32_t> drawn =
        draw_spawn_composition(definition.composition, count, &random_state);

    KernelVec3 route_start{0.0f, 0.0f, 0.0f};
    std::vector<KernelVec3> waypoints;
    if (!draw_route(
            definition, navigation, &random_state, &route_start, &waypoints)) {
        ++route_failure_count_;
        spdlog::warn(
            "patrol found no route patrol={} attempts={}",
            definition.name,
            definition.route_attempts);
        return false;
    }
    const KernelVec3 route_end = waypoints.back();

    const std::vector<KernelVec3> offsets =
        formation_offsets(count, definition.formation_spacing_meters);

    std::vector<std::uint32_t> member_net_ids;
    std::vector<KernelVec3> member_offsets;
    member_net_ids.reserve(count);
    member_offsets.reserve(count);

    std::size_t member = 0;
    for (std::size_t entry = 0; entry < drawn.size(); ++entry) {
        for (std::uint32_t spawned = 0; spawned < drawn[entry]; ++spawned) {
            if (member >= offsets.size()) {
                break;
            }
            const KernelVec3 offset =
                rotate_into_heading(offsets[member], route_start, route_end);
            KernelServerEntityCreateInfo create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.owner_peer = 0;
            create_info.entity_template_id =
                definition.composition[entry].entity_template_id;
            create_info.position = KernelVec3{
                route_start.x + offset.x,
                route_start.y + offset.y,
                route_start.z + offset.z,
            };
            create_info.rotation = kIdentityRotation;
            std::uint32_t net_id = 0;
            if (!Kernel_ServerCreateEntity(kernel, &create_info, &net_id) ||
                net_id == 0) {
                // A squad that came out half spawned is still a squad; the ones
                // that made it walk the route rather than standing where they
                // were created with nothing driving them.
                spdlog::warn(
                    "patrol member spawn failed patrol={} template_id={}",
                    definition.name,
                    definition.composition[entry].entity_template_id);
                continue;
            }
            member_net_ids.push_back(net_id);
            member_offsets.push_back(offsets[member]);
            ++member;
        }
    }

    if (member_net_ids.empty()) {
        return false;
    }
    const std::uint32_t group_id = groups->create_group(
        definition.id,
        std::move(waypoints),
        route_start,
        member_net_ids,
        member_offsets,
        definition.group);
    if (group_id == 0) {
        return false;
    }
    ++runtime->spawn_ordinal;
    ++spawned_group_count_;
    // The breakdown, not just the total: a mixed composition is drawn per
    // squad, so "how many of each did this one actually get" is the question an
    // operator has when a warband reads wrong.
    std::string composition;
    for (std::size_t entry = 0; entry < drawn.size(); ++entry) {
        if (!composition.empty()) {
            composition += ", ";
        }
        composition += definition.composition[entry].entity_template_ref + " x" +
            std::to_string(drawn[entry]);
    }
    spdlog::info(
        "patrol spawned patrol={} group={} members={} [{}] from=({}, {}, {}) "
        "to=({}, {}, {})",
        definition.name,
        group_id,
        member_net_ids.size(),
        composition,
        route_start.x,
        route_start.y,
        route_start.z,
        route_end.x,
        route_end.y,
        route_end.z);
    return true;
}

const std::vector<PatrolDefinitionConfig>& PatrolDirector::definitions() const {
    return definitions_;
}

std::uint32_t PatrolDirector::spawned_group_count() const {
    return spawned_group_count_;
}

std::uint32_t PatrolDirector::retired_group_count() const {
    return retired_group_count_;
}

std::uint32_t PatrolDirector::route_failure_count() const {
    return route_failure_count_;
}

}  // namespace network_example::game_server
