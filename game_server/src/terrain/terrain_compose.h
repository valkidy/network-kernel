// Composes a terrain recipe into a heightfield, bit-identically on every
// platform (implementation plan P1.3-P1.5). A port of unity-terrain-example's
// layered Compose Test Map with a painted ground map (TerrainMapLayers.cs,
// TerrainMapComposer.cs at fe57fb3), in integer arithmetic:
//
//   1. ground map (1.2 m gray) -> optional whole-map turn / mirror -> integer
//      bilinear to 0.3 m -> terrace levels -> per-level chamfer distances to the
//      next higher / lower level -> risers rebuilt as walkable slopes, or
//      cliff_width faces where the cliff noise says so or the band is too
//      narrow for a walkable step                                 [terrace]
//   2. detail noise outside every stamp's ring, then every stamp caps the
//      ground around it to a cone of ring_slope                    [ground]
//   3. stamps written at their base, in recipe order              [composed]
//
// Everything a client needs that took trigonometry or a search to find (the
// riser run, the ring slope as a tangent, the placements) comes in the recipe
// as integers; the layout that chooses them runs on the server only (P2).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_assets.h"
#include "game_server/src/terrain/terrain_types.h"

namespace network_example::game_server::terrain {

struct TerraceParams {
    std::int32_t levels = 19;
    std::int32_t level_height_mm = 1200;
    std::int32_t black_mm = 12000;
    std::int32_t riser_run_mm = 2257;       // level_height / tan(28 deg)
    std::int32_t cliff_width_mm = 300;
    std::int64_t cliff_share_q16 = 9830;    // 0.15
    std::int32_t cliff_wavelength_mm = 25000;
};

struct DetailParams {
    std::int32_t amplitude_mm = 400;
    std::int32_t wavelength_mm = 12000;
};

struct TerrainPlacement {
    std::uint32_t stamp = 0;        // index into TerrainLibrary::stamps
    std::int32_t x_cells = 0;       // footprint's low corner, 0.3 m cells
    std::int32_t z_cells = 0;
    std::int32_t base_cm = 0;       // the stamp's border lands here
    std::int32_t ring_mm = 0;       // blend ring (detail fades out over 4 m past it)
    std::uint8_t turns = 0;         // quarter turns, TerrainMapComposer.Compose's rule
    bool mirror = false;            // stamp's own x flipped, before the turn
};

struct TerrainRecipe {
    std::uint32_t version = 1;
    std::int32_t seed = 0;
    std::int32_t cells_x = 0;               // map edge / 0.3 m; a multiple of 40
    std::int32_t cells_z = 0;
    std::uint32_t ground_map = 0;           // index into TerrainLibrary::ground_maps
    std::uint8_t ground_orientation = 0;    // bits 0-1 quarter turns, bit 2 mirror
    TerraceParams terrace;
    DetailParams detail;
    std::int64_t ring_tan_q16 = 37838;      // tan(30 deg)
    std::vector<TerrainPlacement> placements;
};

struct TerrainLibrary {
    std::vector<StampAsset> stamps;
    std::vector<GroundMapAsset> ground_maps;
};

// Integer gradient noise in Q16, about [-1, 1]: the shape of
// TerrainMapLayers.Noise (same lattice hash, a turned domain per seed, 256
// gradient directions).
std::int64_t gradient_noise_q16(std::int64_t x_mm, std::int64_t z_mm, std::int64_t wavelength_mm, std::int32_t seed);

// Source sample for output sample (u, v) of an e x e square turned / mirrored
// by the recipe rule (turns, then mirror on the source's x).
void turn_source(std::int32_t u, std::int32_t v, std::int32_t e, std::uint8_t turns, bool mirror,
    std::int32_t* lx, std::int32_t* lz);

// A gray ground map (1.2 m) turned into the terrace levels compose would
// derive from it on the 0.3 m grid -- the same code path, so composing either
// gives the same heights. For storing a ground compactly (NKGL).
GroundMapAsset ground_levels_from_gray(const GroundMapAsset& gray, std::int32_t levels);

// Stage 1, in mm on the 0.3 m grid.
bool build_terrace_mm(const TerrainRecipe& recipe, const TerrainLibrary& library,
    std::vector<std::int32_t>* out, std::string* error);

// Stage 2, in place.
void apply_detail_and_cones(const TerrainRecipe& recipe, const TerrainLibrary& library, std::vector<std::int32_t>* mm);

HeightGrid mm_to_cm_grid(const TerrainRecipe& recipe, const std::vector<std::int32_t>& mm);

// Stage 3, in place.
bool write_stamps(const TerrainRecipe& recipe, const TerrainLibrary& library, HeightGrid* grid, std::string* error);

struct ComposeStages {
    HeightGrid terrace;
    HeightGrid ground;
};

bool validate_recipe(const TerrainRecipe& recipe, const TerrainLibrary& library, std::string* error);

// All three stages. stages, when given, receives the first two.
bool compose(const TerrainRecipe& recipe, const TerrainLibrary& library, HeightGrid* out,
    std::string* error, ComposeStages* stages = nullptr);

}  // namespace network_example::game_server::terrain
