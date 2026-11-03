#include "game_server/src/patrol_director.h"

#include "game_server/src/patrol_allocation.h"
#include "game_server/src/patrol_targeting.h"
#include "game_server/src/spawn_sampling.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
// The lifecycle command's `reason` is a KernelDespawnReason, not a free tag:
// the kernel maps Destroyed to a Destroyed lifecycle event, and clients read
// Destroyed on an actor as a kill and splat it. A retired squad is alive, so it
// has to leave under a reason that is not Destroyed.
constexpr std::uint32_t kPatrolRetiredReason = KernelDespawnReason_Retired;
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

namespace {

std::string validate_around_players(const PatrolAroundPlayersConfig& around) {
    const auto positive = [](float value) {
        return std::isfinite(value) && value > 0.0f;
    };
    if (!positive(around.ring_min_meters) ||
        !positive(around.ring_max_meters) ||
        around.ring_min_meters >= around.ring_max_meters) {
        return "around_players ring needs 0 < min_meters < max_meters";
    }
    // A route that only has to pass the target somewhere inside the ring it
    // started on is no constraint at all.
    if (!positive(around.pass_within_meters) ||
        around.pass_within_meters >= around.ring_min_meters) {
        return "around_players pass_within_meters must be above zero and "
               "below ring min_meters";
    }
    if (!std::isfinite(around.exit_spread_degrees) ||
        around.exit_spread_degrees < 0.0f ||
        around.exit_spread_degrees > 90.0f) {
        return "around_players exit_spread_degrees must be within 0..90";
    }
    if (!positive(around.group_radius_meters)) {
        return "around_players group_radius_meters must be above zero";
    }
    if (!positive(around.groups_per_member) ||
        !positive(around.groups_per_player)) {
        return "around_players groups_per_member and groups_per_player must "
               "be above zero";
    }
    return {};
}

// Tiers have to cover every group size exactly once, from a lone player up,
// or some group has no squad to draw and finds out at run time.
std::string validate_squad_tiers(const std::vector<PatrolSquadTier>& tiers) {
    std::uint32_t next_members = 1;
    for (std::size_t index = 0; index < tiers.size(); ++index) {
        const PatrolSquadTier& tier = tiers[index];
        const bool last = index + 1 == tiers.size();
        if (tier.members_min != next_members) {
            return "squad_by_group_size tiers must start at members 1 and "
                   "leave no gap, expected min " +
                std::to_string(next_members);
        }
        if (tier.members_max == 0) {
            if (!last) {
                return "squad_by_group_size only the last tier may leave "
                       "members max unbounded";
            }
        } else {
            if (tier.members_max < tier.members_min) {
                return "squad_by_group_size tier members max is below min";
            }
            if (last) {
                return "squad_by_group_size last tier must leave members max "
                       "unbounded";
            }
            next_members = tier.members_max + 1;
        }
        const std::string composition_error = validate_spawn_composition(
            tier.composition, tier.count_min, tier.count_max);
        if (!composition_error.empty()) {
            return "squad_by_group_size members " +
                std::to_string(tier.members_min) + " tier " + composition_error;
        }
    }
    return {};
}

}  // namespace

std::string validate_patrol_definition(const PatrolDefinitionConfig& definition) {
    if (!definition.squad_tiers.empty()) {
        if (definition.placement != PatrolPlacementMode::kAroundPlayers) {
            return "patrol squad_by_group_size needs where mode around_players";
        }
        const std::string tier_error =
            validate_squad_tiers(definition.squad_tiers);
        if (!tier_error.empty()) {
            return "patrol " + tier_error;
        }
    } else {
        const std::string composition_error = validate_spawn_composition(
            definition.composition, definition.count_min, definition.count_max);
        if (!composition_error.empty()) {
            return "patrol " + composition_error;
        }
    }
    if (definition.placement == PatrolPlacementMode::kAroundPlayers) {
        const std::string around_error =
            validate_around_players(definition.around_players);
        if (!around_error.empty()) {
            return "patrol " + around_error;
        }
        return {};
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
    PatrolBudgetConfig budget,
    PlayableBoundsConfig playable_bounds)
    : definitions_(std::move(definitions)),
      budget_(budget),
      playable_bounds_(playable_bounds),
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
    const PatrolNavigation* navigation,
    const ActorStateView& actors,
    AgentBudget* agent_budget) {
    if (kernel == nullptr || groups == nullptr) {
        return;
    }
    ++tick_;
    retire_finished_patrols(kernel, groups, actors);

    std::uint32_t live_agents = 0;
    std::vector<KernelVec3> squad_cursors;
    squad_cursors.reserve(groups->groups().size());
    for (const PatrolGroup& group : groups->groups()) {
        live_agents += static_cast<std::uint32_t>(group.member_net_ids.size());
        squad_cursors.push_back(group.cursor);
    }
    const std::vector<TargetPlayer> players = target_players_from(actors);
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        const PatrolDefinitionConfig& definition = definitions_[index];
        DefinitionRuntime& runtime = runtimes_[index];
        if (definition.trigger != PatrolSpawnTrigger::kFixedInterval) {
            continue;
        }
        if (definition.placement == PatrolPlacementMode::kAroundPlayers) {
            // The tighter of the two ceilings, under the same largest-draw
            // rule as below, but against the chosen tier's largest draw.
            std::uint32_t agent_room = agent_budget != nullptr
                ? agent_budget->room()
                : std::numeric_limits<std::uint32_t>::max();
            if (budget_.max_live_agents != 0) {
                agent_room = std::min(
                    agent_room,
                    live_agents >= budget_.max_live_agents
                        ? 0u
                        : budget_.max_live_agents - live_agents);
            }
            const std::uint32_t spent = tick_around_players(
                kernel, groups, navigation, players, squad_cursors, agent_room,
                definition, &runtime);
            if (spent != 0) {
                live_agents += spent;
                if (agent_budget != nullptr) {
                    agent_budget->spend(spent);
                }
                // Its cursor counts for every definition after this one.
                squad_cursors.push_back(groups->groups().back().cursor);
            }
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
        // The server-wide ceiling, by the same largest-draw rule: it counts
        // every agent, not just squads, so a full world holds patrols back.
        if (agent_budget != nullptr &&
            agent_budget->room() < definition.count_max) {
            continue;
        }
        if (spawn_patrol(kernel, groups, navigation, definition, &runtime)) {
            runtime.ticks_until_spawn = definition.interval_ticks;
            live_agents += definition.count_max;
            if (agent_budget != nullptr) {
                agent_budget->spend(definition.count_max);
            }
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
//
// Reads the caller's snapshot rather than querying. Nothing between the
// snapshot and here creates or moves a player, so this sees exactly what a
// query at this point would have -- and one squad no longer pays for the walk
// the next squad is about to repeat.
float nearest_player_distance(const ActorStateView& actors, const KernelVec3& from) {
    float nearest = -1.0f;
    for (std::uint32_t index = 0; index < actors.count; ++index) {
        const KernelServerEntityState& state = actors.states[index];
        if (state.valid == 0u || state.actor_type != kActorTypePlayer) {
            continue;
        }
        const float distance =
            std::sqrt(horizontal_distance_squared(from, state.position));
        if (nearest < 0.0f || distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

}  // namespace

void PatrolDirector::retire_finished_patrols(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups,
    const ActorStateView& actors) {
    std::vector<std::uint32_t> retiring;
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
            const float nearest = nearest_player_distance(actors, group.cursor);
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

namespace {

constexpr float kRingPi = 3.14159265358979323846f;
// Bearings a ring is offered in. A sector whose middle lies outside the
// playable bounds is never drawn from, so a player against an edge does not
// spend half the attempts on starts that were never going to be inside.
constexpr int kRingSectors = 24;
// How far inside the playable bounds a start or an end must sit.
constexpr float kBoundsInsetMeters = 1.0f;
// Ends land wherever the clamp put them, which on an eroded edge is often just
// off the mesh; this is how far to look for ground under one.
constexpr float kRingEndSnapMeters = 3.0f;
// Ticks to wait after a ring draw found nothing.
constexpr std::uint32_t kRingRetryTicks = 30;

struct HorizontalBounds {
    bool bounded = false;
    float min_x = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_z = 0.0f;

    bool contains(float x, float z) const {
        return !bounded ||
            (x >= min_x + kBoundsInsetMeters && x <= max_x - kBoundsInsetMeters &&
             z >= min_z + kBoundsInsetMeters && z <= max_z - kBoundsInsetMeters);
    }

    KernelVec3 clamp(KernelVec3 point) const {
        if (bounded) {
            point.x = std::clamp(
                point.x, min_x + kBoundsInsetMeters, max_x - kBoundsInsetMeters);
            point.z = std::clamp(
                point.z, min_z + kBoundsInsetMeters, max_z - kBoundsInsetMeters);
        }
        return point;
    }
};

// Authored bounds first, the navmesh's extent second, unbounded last.
HorizontalBounds resolve_bounds(
    const PlayableBoundsConfig& authored,
    const PatrolNavigation* navigation) {
    HorizontalBounds bounds;
    if (authored.authored) {
        bounds = HorizontalBounds{
            true, authored.min_x, authored.min_z, authored.max_x, authored.max_z};
    } else if (navigation != nullptr &&
               navigation->horizontal_bounds(
                   &bounds.min_x, &bounds.min_z, &bounds.max_x, &bounds.max_z)) {
        bounds.bounded = true;
    }
    return bounds;
}

float horizontal_distance(const KernelVec3& from, const KernelVec3& to) {
    return std::sqrt(horizontal_distance_squared(from, to));
}

// Horizontal distance from `point` to the segment from `from` to `to`.
float distance_to_segment(
    const KernelVec3& point, const KernelVec3& from, const KernelVec3& to) {
    const float segment_x = to.x - from.x;
    const float segment_z = to.z - from.z;
    const float length_squared = segment_x * segment_x + segment_z * segment_z;
    float along = 0.0f;
    if (length_squared > 0.0f) {
        along = ((point.x - from.x) * segment_x + (point.z - from.z) * segment_z) /
            length_squared;
        along = std::clamp(along, 0.0f, 1.0f);
    }
    const KernelVec3 nearest{
        from.x + segment_x * along, point.y, from.z + segment_z * along};
    return horizontal_distance(point, nearest);
}

KernelVec3 on_ring(const KernelVec3& center, float bearing, float radius) {
    return KernelVec3{
        center.x + std::cos(bearing) * radius,
        center.y,
        center.z + std::sin(bearing) * radius,
    };
}

// Uniform over the ring's area rather than its radius, which would crowd the
// inner edge.
float ring_radius(const PatrolAroundPlayersConfig& around, std::uint64_t* random_state) {
    const float inner = around.ring_min_meters * around.ring_min_meters;
    const float outer = around.ring_max_meters * around.ring_max_meters;
    return std::sqrt(inner + next_unit(random_state) * (outer - inner));
}

// §6 of the plan: a start on the ring out of everyone's sight, an end across
// from it, and a route between them that passes the target.
bool draw_ring_route(
    const PatrolDefinitionConfig& definition,
    const PatrolNavigation* navigation,
    const HorizontalBounds& bounds,
    const KernelVec3& target,
    const std::vector<TargetPlayer>& players,
    std::uint64_t* random_state,
    KernelVec3* origin,
    std::vector<KernelVec3>* waypoints) {
    const PatrolAroundPlayersConfig& around = definition.around_players;
    const bool navigable = navigation != nullptr && navigation->valid();
    const float sector = 2.0f * kRingPi / static_cast<float>(kRingSectors);
    const float middle_radius = 0.5f * (around.ring_min_meters + around.ring_max_meters);
    std::vector<int> open_sectors;
    for (int index = 0; index < kRingSectors; ++index) {
        const KernelVec3 middle =
            on_ring(target, (static_cast<float>(index) + 0.5f) * sector, middle_radius);
        if (bounds.contains(middle.x, middle.z)) {
            open_sectors.push_back(index);
        }
    }
    if (open_sectors.empty()) {
        return false;
    }
    const float spread = around.exit_spread_degrees * kRingPi / 180.0f;

    const std::uint32_t attempts = std::max<std::uint32_t>(1u, definition.route_attempts);
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        const int chosen = open_sectors[next_in_range(
            random_state, 0u, static_cast<std::uint32_t>(open_sectors.size() - 1))];
        const float bearing =
            (static_cast<float>(chosen) + next_unit(random_state)) * sector;
        KernelVec3 start = on_ring(target, bearing, ring_radius(around, random_state));
        const float exit_bearing =
            bearing + kRingPi + (next_unit(random_state) * 2.0f - 1.0f) * spread;
        // Clamped rather than thrown back: a player in a corner has no "across"
        // inside the map, and a route that ends at the edge behind them still
        // walks past them.
        KernelVec3 end =
            bounds.clamp(on_ring(target, exit_bearing, ring_radius(around, random_state)));
        if (!bounds.contains(start.x, start.z)) {
            continue;
        }
        // Out of everyone's relevance, not only the target's.
        const bool seen = std::any_of(
            players.begin(), players.end(), [&](const TargetPlayer& player) {
                return horizontal_distance(start, player.position) < around.ring_min_meters;
            });
        if (seen) {
            continue;
        }

        if (navigable) {
            if (!navigation->snap(start, 1.0f, &start) ||
                !navigation->snap(end, kRingEndSnapMeters, &end)) {
                continue;
            }
            PatrolRouteRequest request;
            request.start = start;
            request.end = end;
            request.max_detour_ratio = definition.max_detour_ratio;
            if (!navigation->find_route(request, waypoints)) {
                continue;
            }
        } else {
            *waypoints = {end};
        }

        // The cross itself. On a navmesh this is also what proves the start can
        // walk to the target at all, which the rooftop item in
        // AI_PATROL_SYSTEM.md was deferred waiting for.
        float closest = distance_to_segment(target, start, waypoints->front());
        for (std::size_t corner = 1; corner < waypoints->size(); ++corner) {
            closest = std::min(
                closest,
                distance_to_segment(target, (*waypoints)[corner - 1], (*waypoints)[corner]));
        }
        if (closest > around.pass_within_meters) {
            continue;
        }
        *origin = start;
        return true;
    }
    return false;
}

}  // namespace

std::uint32_t PatrolDirector::tick_around_players(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups,
    const PatrolNavigation* navigation,
    const std::vector<TargetPlayer>& players,
    const std::vector<KernelVec3>& squad_cursors,
    std::uint32_t agent_room,
    const PatrolDefinitionConfig& definition,
    DefinitionRuntime* runtime) {
    const PatrolAroundPlayersConfig& around = definition.around_players;
    // Every tick, whatever else happens, so a floor interval is measured in
    // ticks that actually passed.
    runtime->threats.observe(tick_, players, squad_cursors, around.ring_max_meters);

    const bool interval_elapsed = runtime->ticks_until_spawn == 0;
    if (!interval_elapsed) {
        --runtime->ticks_until_spawn;
    }
    if (runtime->ticks_until_retry > 0) {
        --runtime->ticks_until_retry;
        return 0;
    }

    const std::vector<PlayerGroup> player_groups =
        group_players(players, around.group_radius_meters);
    std::vector<PatrolTargetCandidate> candidates;
    candidates.reserve(player_groups.size());
    for (const PlayerGroup& group : player_groups) {
        candidates.push_back(PatrolTargetCandidate{
            static_cast<std::uint32_t>(group.member_net_ids.size()),
            squads_near_group(group, squad_cursors, around.ring_max_meters),
            runtime->threats.group_starved_ticks(group, tick_),
        });
    }
    PatrolAllocationState state;
    for (const PatrolGroup& group : groups->groups()) {
        state.live_groups += group.definition_id == definition.id ? 1u : 0u;
    }
    state.player_count = static_cast<std::uint32_t>(players.size());
    state.interval_elapsed = interval_elapsed;
    state.agent_room = agent_room;
    const std::optional<PatrolTargetChoice> choice =
        choose_patrol_target(definition, candidates, state);
    if (!choice.has_value()) {
        return 0;
    }
    const PlayerGroup& target = player_groups[choice->candidate_index];
    KernelVec3 center = target.centroid;
    // Onto the ground when there is some under it: a centroid between two
    // players can hang over a drop, and the route is judged against it.
    if (navigation != nullptr && navigation->valid()) {
        (void)navigation->snap(center, kRingEndSnapMeters, &center);
    }

    std::uint64_t random_state =
        static_cast<std::uint64_t>(definition.seed) * 0x9e3779b97f4a7c15ull +
        static_cast<std::uint64_t>(runtime->spawn_ordinal);
    const PatrolSquadShape& shape = choice->shape;
    const std::uint32_t count =
        next_in_range(&random_state, shape.count_min, shape.count_max);
    const std::vector<std::uint32_t> drawn =
        draw_spawn_composition(*shape.composition, count, &random_state);

    const HorizontalBounds bounds = resolve_bounds(playable_bounds_, navigation);
    if (!bounds.bounded && !warned_unbounded_) {
        warned_unbounded_ = true;
        spdlog::warn(
            "patrol {} aims at players with no map.playable_bounds and no "
            "navmesh; its squads can start off the map",
            definition.name);
    }
    KernelVec3 route_start{0.0f, 0.0f, 0.0f};
    std::vector<KernelVec3> waypoints;
    if (!draw_ring_route(
            definition, navigation, bounds,
            center, players, &random_state, &route_start, &waypoints)) {
        // The interval is not spent: the next try is the same choice, a
        // second later.
        ++route_failure_count_;
        runtime->ticks_until_retry = kRingRetryTicks;
        spdlog::warn(
            "patrol found no ring route patrol={} target_members={} "
            "at=({}, {}, {}) attempts={}",
            definition.name,
            target.member_net_ids.size(),
            center.x,
            center.y,
            center.z,
            definition.route_attempts);
        return 0;
    }
    if (!place_squad(
            kernel, groups, definition, *shape.composition, drawn, count,
            route_start, std::move(waypoints), runtime)) {
        return 0;
    }
    runtime->ticks_until_spawn = definition.interval_ticks;
    spdlog::info(
        "patrol aimed patrol={} reason={} target_members={} target_starved_ticks={}",
        definition.name,
        static_cast<int>(choice->reason),
        target.member_net_ids.size(),
        candidates[choice->candidate_index].starved_ticks);
    return shape.count_max;
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
    return place_squad(
        kernel, groups, definition, definition.composition, drawn, count,
        route_start, std::move(waypoints), runtime);
}

bool PatrolDirector::place_squad(
    KernelHandle* kernel,
    PatrolGroupRuntime* groups,
    const PatrolDefinitionConfig& definition,
    const std::vector<SpawnCompositionEntry>& composition_entries,
    const std::vector<std::uint32_t>& drawn,
    std::uint32_t count,
    const KernelVec3& route_start,
    std::vector<KernelVec3> waypoints,
    DefinitionRuntime* runtime) {
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
                composition_entries[entry].entity_template_id;
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
                    composition_entries[entry].entity_template_id);
                continue;
            }
            member_net_ids.push_back(net_id);
            member_offsets.push_back(offsets[member]);
            // A squad's members are in it, whatever their template defaults
            // to; the catalog only lets a squad name templates that can be.
            spawned_controllers_.push_back(SpawnedControllerSpec{
                net_id, AgentControllerSpec{AgentControllerRole::kSquad, {}}});
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
        composition += composition_entries[entry].entity_template_ref + " x" +
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

std::vector<SpawnedControllerSpec> PatrolDirector::take_spawned_controllers() {
    return std::exchange(spawned_controllers_, {});
}

}  // namespace network_example::game_server
