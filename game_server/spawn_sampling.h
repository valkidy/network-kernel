#ifndef GAME_SERVER_SPAWN_SAMPLING_H_
#define GAME_SERVER_SPAWN_SAMPLING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// The draws every spawner in game_server shares: a deterministic stream, an
// area to place things in, and a mix to fill a group with.
//
// Extracted from the patrol director when a second kind of spawner needed the
// same maths. Two copies of a composition draw would have been two chances to
// get it subtly wrong, and this one has already been wrong once -- see
// draw_spawn_composition.

// splitmix64. Deterministic and stateless beyond the seed, so a spawner that
// derives its stream from authored values replays identically.
std::uint64_t next_random(std::uint64_t* state);
// Uniform in [0, 1).
float next_unit(std::uint64_t* state);
// Uniform in [low, high], inclusive.
std::uint32_t next_in_range(
    std::uint64_t* state,
    std::uint32_t low,
    std::uint32_t high);

enum class SpawnAreaShape : std::uint8_t {
    // A disc: uniform over the area, not over (radius, angle), which would
    // crowd the middle.
    kCircle = 0,
    kRect = 1,
};

struct SpawnAreaConfig {
    SpawnAreaShape shape = SpawnAreaShape::kCircle;
    KernelVec3 center{0.0f, 0.0f, 0.0f};
    // Circle reads x as the radius and ignores the rest. Rect reads x and z; y
    // is not a thickness, because nothing here places anything vertically.
    KernelVec3 half_extents{0.0f, 0.0f, 0.0f};
};

// A point in the area. `origin` is added to the authored centre, so an area
// authored relative to something that moves can follow it.
KernelVec3 sample_area(
    const SpawnAreaConfig& area,
    const KernelVec3& origin,
    std::uint64_t* state);

// Empty when the area can place anything at all.
std::string validate_spawn_area(const SpawnAreaConfig& area);

struct SpawnCompositionEntry {
    std::string entity_template_ref;
    std::uint32_t entity_template_id = 0;
    std::uint32_t min_count = 0;
    std::uint32_t max_count = 0;
};

// Empty when a group of any size in [count_min, count_max] can be filled from
// these entries. The floors have to fit inside the smallest group and the
// ceilings have to reach the largest, or the definition draws a total it cannot
// then fill.
std::string validate_spawn_composition(
    const std::vector<SpawnCompositionEntry>& composition,
    std::uint32_t count_min,
    std::uint32_t count_max);

// How many of each entry a group of `count` is made of.
//
// The bands are floors plus capacity, not a second opinion about the total:
// every entry gets its minimum and the remainder is handed out one at a time.
// Read any other way, `count: 8-10` with `[A: 2-8, B: 4-20]` is unsatisfiable --
// the floors sum to 6 and the ceilings to 28, and neither is 8 to 10.
//
// Each unit of the remainder goes to an entry in proportion to the room it has
// left, not uniformly among the entries that have any. The difference is not a
// detail. Uniformly, a narrow band beside a wide one saturates: 1-5 of one next
// to 6-24 of another, over a remainder of thirteen, fills the narrow one about
// 95% of the time, so `max` reads as "always". Weighting by remaining room
// makes the choice equivalent to dealing the remainder into the spare slots
// uniformly, which makes each entry's count hypergeometric -- and the same
// narrow band reaches its ceiling about 20% of the time.
std::vector<std::uint32_t> draw_spawn_composition(
    const std::vector<SpawnCompositionEntry>& composition,
    std::uint32_t count,
    std::uint64_t* random_state);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SPAWN_SAMPLING_H_
