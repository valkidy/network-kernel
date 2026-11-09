// Server-side layout: picks stamps and finds each one a site, a base height and
// a turn, producing the recipe both ends then compose (implementation plan
// P2.1). A port of TerrainMapLayers.Layout (unity-terrain-example fe57fb3) for
// the painted-map ground:
//
//   - pick stamp_count stamps from the pool (repeats allowed), biggest first;
//   - for each, try every site on the 12 m grid that keeps the footprint one
//     grid cell in from the map edge: base = midway between the lowest and
//     highest ground 8 m out, rounded to base_step (and high enough that the
//     stamp's lowest point stays 1 m above zero); ring = how far a ring_slope
//     cone needs to reach that ground (at least min_ring, at most max_ring);
//     the claim (footprint + ring) may not overlap an earlier claim nor cross a
//     cliff; the narrowest ring wins, with a little noise;
//   - turn: of the four turns and the mirror, the one that faces the stamp's
//     open sides toward the sides where the ground is closest to its base.
//
// This runs on the server only and its result travels in the recipe, so it may
// use float and does not need to match across platforms; it is still
// deterministic for a given seed on one build (own PRNG, no <random>).
// It does not reproduce Unity's placements (Unity draws from System.Random);
// P2.5 compares the quality of the result, not the sites.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_compose.h"

namespace network_example::game_server::terrain {

struct LayoutParams {
    std::int32_t stamp_count = 12;
    std::vector<std::uint32_t> stamp_pool;   // indices into the library; empty = all
    double base_step_m = 1.5;
    double ring_slope_deg = 30.0;
    double min_ring_m = 4.0;
    double max_ring_m = 24.0;
    double around_m = 8.0;                   // where the base looks at the ground
    double cliff_probe_m = 1.5;              // CrossesCliff's step
    double cliff_rise_m = 1.1;               // a rise this big over one probe step is a cliff
    double min_floor_m = 1.0;                // a stamp's lowest point stays this far above 0
    double ring_noise_m = 4.0;               // site score = ring + noise in [0, ring_noise)
    double center_clear_m = 0.0;             // no stamp or its ring within this of the map centre (half-size of a square)
};

struct LayoutReport {
    std::int32_t picked = 0;
    std::int32_t placed = 0;
    std::int32_t rejected_ring = 0;
    std::int32_t rejected_overlap = 0;
    std::int32_t rejected_cliff = 0;
    std::vector<std::string> unplaced;       // stamp names that found no site
};

// Small deterministic PRNG (splitmix64).
class LayoutRng {
public:
    explicit LayoutRng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next();
    double next_double();                    // [0, 1)
    std::uint32_t next_below(std::uint32_t n);

private:
    std::uint64_t state_;
};

// Per stamp side (S, E, N, W in the stamp's own frame): the share of a band
// 3 m inside the border that sits within 0.6 m of border height.
std::vector<double> stamp_openness(const StampAsset& stamp);

// Fills recipe->placements. recipe must already describe the map and ground
// (cells, ground map, orientation, terrace params, ring slope): the layout
// reads the composed terrace stage to score sites. ring_tan_q16 is set from
// params.ring_slope_deg.
bool layout_stamps(TerrainRecipe* recipe, const TerrainLibrary& library, const LayoutParams& params,
    LayoutReport* report, std::string* error);

}  // namespace network_example::game_server::terrain
