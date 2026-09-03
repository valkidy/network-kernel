#ifndef GAME_SERVER_PATROL_DIRECTOR_H_
#define GAME_SERVER_PATROL_DIRECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/agent_runtime.h"
#include "game_server/patrol_group_runtime.h"
#include "game_server/patrol_navigation.h"
#include "game_server/spawn_sampling.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// Which of the three questions a patrol definition answers, and how. They are
// separate axes rather than one mode enum because the answers are independent:
// a quick simulation setup wants a fixed cadence and an explicitly sized squad,
// and a paced encounter system wants accumulated pressure and a credit budget,
// but every mixture of those is a sensible thing to author.
enum class PatrolSpawnTrigger : std::uint8_t {
    // A countdown. The whole of WHEN, for now.
    kFixedInterval = 0,
};

enum class PatrolCompositionMode : std::uint8_t {
    // Author a total and a per-template band: `count: 8-10` with
    // `[A: 2-8, B: 4-20]`. The bands are floors plus capacity, not a second
    // opinion about the total -- see draw_spawn_composition.
    kExplicitRanges = 0,
};

// One authored kind of patrol. The catalog holds a list of these; each one
// spawns squads on its own cadence, into its own area, out of its own mix.
struct PatrolDefinitionConfig {
    std::uint32_t id = 0;
    std::string name;
    SpawnAreaConfig area;
    // Not a PRNG seed in the existing spawners, where it is a starting index
    // into a golden-angle sequence. Here it really is one: every draw a
    // definition makes is derived from this and the number of squads it has
    // spawned, so a definition replays identically and a test can name a
    // squad's size before it exists.
    std::uint32_t seed = 1;

    PatrolSpawnTrigger trigger = PatrolSpawnTrigger::kFixedInterval;
    std::uint32_t interval_ticks = 900;
    // Squads alive at once. A squad that has walked its route out still counts
    // until something despawns it, which nothing does yet.
    std::uint32_t max_live_groups = 1;

    PatrolCompositionMode composition_mode = PatrolCompositionMode::kExplicitRanges;
    std::uint32_t count_min = 1;
    std::uint32_t count_max = 1;
    std::vector<SpawnCompositionEntry> composition;

    float formation_spacing_meters = 1.5f;
    PatrolGroupTuning group{};

    // How far a route may walk relative to the straight line between its ends.
    // The bench measured 1.01 median on rolling ground and 1.23 on a walled
    // level, against a worst case of 18.10 -- so this is not about typical
    // routes, it is about refusing the pair that would have a squad spend its
    // whole life walking round one wall. Ignored without a navmesh, where every
    // route is the straight line.
    float max_detour_ratio = 4.0f;
    // Start/end pairs to try before giving up for this interval. A rectangle
    // that is mostly unwalkable will fail some draws; one that is entirely
    // unwalkable should give up rather than search forever.
    std::uint32_t route_attempts = 8;

    // A squad that has walked its route out is finished, and finished squads
    // are what a one-shot route leaves behind. Retiring them is not an
    // optimisation: without it a definition's live ceiling fills with squads
    // standing at the far end of their route and never spawns again.
    //
    // The linger is so that a squad does not vanish in front of whoever walked
    // it down. Zero retires on the tick it finishes.
    std::uint32_t despawn_linger_ticks = 300;
    // Retire early when the nearest player is further away than this, finished
    // or not. Zero leaves a squad walking its route however far from anyone it
    // gets, which is what to author when the patrol is the point rather than
    // the encounter. Never applies to a squad that is fighting.
    float despawn_distance_meters = 0.0f;
};

// A ceiling across every definition, which is the one thing per-definition
// ceilings cannot express: three definitions of four squads each are twelve
// squads, and nothing but this notices. Zero is unbounded.
struct PatrolBudgetConfig {
    std::uint32_t max_live_agents = 0;
};

// Rejects a definition that cannot be satisfied, with a sentence saying why.
// Empty means usable. Called at load, so an unsatisfiable patrol is a catalog
// that does not load rather than a squad that quietly comes out the wrong size.
std::string validate_patrol_definition(const PatrolDefinitionConfig& definition);

// Where each member of a squad of `count` stands, relative to the squad, in the
// squad's own frame: +X is the direction of travel.
std::vector<KernelVec3> formation_offsets(std::uint32_t count, float spacing);

// Spawns squads and hands them to the group runtime.
//
// It lives here rather than on a director entity in the world because a patrol
// is gameplay: the kernel owns entity storage and validated mutation, and knows
// nothing about squads. Everything below runs on the public server API.
class PatrolDirector {
public:
    explicit PatrolDirector(
        std::vector<PatrolDefinitionConfig> definitions = {},
        PatrolBudgetConfig budget = {});

    // Runs before the agent list is resynced, so that the entities it creates
    // are discovered on the same tick their squad is. The other order drops
    // every new member the moment the group runtime looks for it.
    // `navigation` may be null, or loaded from a catalog that authored no
    // navmesh, in which case a route is the straight chord between two points
    // in the area -- exactly a pathed route on flat ground, and wrong anywhere
    // else. See //game_server:patrol_nav_bench.
    void tick(
        KernelHandle* kernel,
        PatrolGroupRuntime* groups,
        const PatrolNavigation* navigation);

    const std::vector<PatrolDefinitionConfig>& definitions() const;
    // Squads spawned since the server started, across all definitions. The
    // draws are derived from it, so it is also what makes them replayable.
    std::uint32_t spawned_group_count() const;
    std::uint32_t retired_group_count() const;
    // Spawns abandoned because no start/end pair in the area produced an
    // acceptable route. Worth watching: a definition whose area is mostly
    // unwalkable reports it here rather than by quietly never spawning.
    std::uint32_t route_failure_count() const;

private:
    struct DefinitionRuntime {
        std::uint32_t ticks_until_spawn = 0;
        std::uint32_t spawn_ordinal = 0;
    };

    void retire_finished_patrols(
        KernelHandle* kernel,
        PatrolGroupRuntime* groups);

    bool spawn_patrol(
        KernelHandle* kernel,
        PatrolGroupRuntime* groups,
        const PatrolNavigation* navigation,
        const PatrolDefinitionConfig& definition,
        DefinitionRuntime* runtime);

    std::vector<PatrolDefinitionConfig> definitions_;
    PatrolBudgetConfig budget_;
    std::vector<DefinitionRuntime> runtimes_;
    std::uint32_t spawned_group_count_ = 0;
    std::uint32_t retired_group_count_ = 0;
    std::uint32_t route_failure_count_ = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_DIRECTOR_H_
