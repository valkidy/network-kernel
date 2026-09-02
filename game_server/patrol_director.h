#ifndef GAME_SERVER_PATROL_DIRECTOR_H_
#define GAME_SERVER_PATROL_DIRECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/agent_runtime.h"
#include "game_server/patrol_group_runtime.h"
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
    // opinion about the total -- see draw_composition.
    kExplicitRanges = 0,
};

enum class PatrolAreaShape : std::uint8_t {
    // What the existing spawners do: a ring at exactly `radius`.
    kCircle = 0,
    // Anywhere inside the rectangle, which is the shape a patrol wants: a route
    // needs two points that are not both on one circle.
    kRect = 1,
};

struct PatrolAreaConfig {
    PatrolAreaShape shape = PatrolAreaShape::kCircle;
    KernelVec3 center{0.0f, 0.0f, 0.0f};
    // Circle reads x as the radius and ignores the rest. Rect reads x and z;
    // y is not a thickness, because nothing here places anything vertically.
    KernelVec3 half_extents{0.0f, 0.0f, 0.0f};
};

struct PatrolCompositionEntry {
    std::string entity_template_ref;
    std::uint32_t entity_template_id = 0;
    std::uint32_t min_count = 0;
    std::uint32_t max_count = 0;
};

// One authored kind of patrol. The catalog holds a list of these; each one
// spawns squads on its own cadence, into its own area, out of its own mix.
struct PatrolDefinitionConfig {
    std::uint32_t id = 0;
    std::string name;
    PatrolAreaConfig area;
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
    std::vector<PatrolCompositionEntry> composition;

    float formation_spacing_meters = 1.5f;
    PatrolGroupTuning group{};
};

// Rejects a definition that cannot be satisfied, with a sentence saying why.
// Empty means usable. Called at load, so an unsatisfiable patrol is a catalog
// that does not load rather than a squad that quietly comes out the wrong size.
std::string validate_patrol_definition(const PatrolDefinitionConfig& definition);

// How many of each composition entry a squad of `count` is made of.
//
// The bands are read as floors plus capacity, not as a second opinion about the
// total: every entry gets its minimum, and what is left over goes to entries
// still under their maximum, one at a time, chosen by the same stream every
// other draw comes from. Read any other way, `count: 8-10` with
// `[A: 2-8, B: 4-20]` is unsatisfiable -- the floors sum to 6 and the ceilings
// to 28, and neither is 8 to 10.
std::vector<std::uint32_t> draw_composition(
    const PatrolDefinitionConfig& definition,
    std::uint32_t count,
    std::uint64_t* random_state);

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
    explicit PatrolDirector(std::vector<PatrolDefinitionConfig> definitions = {});

    // Runs before the agent list is resynced, so that the entities it creates
    // are discovered on the same tick their squad is. The other order drops
    // every new member the moment the group runtime looks for it.
    void tick(KernelHandle* kernel, PatrolGroupRuntime* groups);

    const std::vector<PatrolDefinitionConfig>& definitions() const;
    // Squads spawned since the server started, across all definitions. The
    // draws are derived from it, so it is also what makes them replayable.
    std::uint32_t spawned_group_count() const;

private:
    struct DefinitionRuntime {
        std::uint32_t ticks_until_spawn = 0;
        std::uint32_t spawn_ordinal = 0;
    };

    bool spawn_patrol(
        KernelHandle* kernel,
        PatrolGroupRuntime* groups,
        const PatrolDefinitionConfig& definition,
        DefinitionRuntime* runtime);

    std::vector<PatrolDefinitionConfig> definitions_;
    std::vector<DefinitionRuntime> runtimes_;
    std::uint32_t spawned_group_count_ = 0;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_DIRECTOR_H_
