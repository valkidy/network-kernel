// The server's whole map generation (implementation plan P2.4, plan §4.1):
// layout -> compose -> navmesh -> checks, retried until a map passes.
//
//   1. retry   up to max_retries layouts with seeds derived from the request's;
//   2. relax   the same again with the stamp count lowered to min_stamp_count;
//   3. fallback the level's pre-validated recipe, accepted only if it still
//              composes to the hash recorded with it (a stamp or compose change
//              that moved it must regenerate it, not ship a different map);
//   4. fail    nothing passed: the server must not start on an unchecked map.
//
// Every attempt leaves a report line (seed, placed / unplaced stamps and why,
// islands, reach, seams), so failures can be tuned in the level data and any
// failing recipe replayed exactly.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_compose.h"
#include "game_server/src/terrain/terrain_layout.h"
#include "game_server/src/terrain/terrain_navmesh.h"

class dtNavMesh;

namespace network_example::game_server::terrain {

struct AcceptRules {
    double min_reach_fraction = 0.995;      // of probes on the navmesh
    double max_unreachable_m2 = 400.0;
    std::uint32_t max_seam_failures = 0;
    std::int32_t min_placed = 0;            // 0 = the stamp count asked for
};

struct GenerateRequest {
    TerrainRecipe base;                      // map, ground, terrace, detail; placements ignored
    std::int32_t seed = 1;
    LayoutParams layout;
    std::int32_t min_stamp_count = 0;        // relax step; 0 = skip it
    std::int32_t max_retries = 8;
    AcceptRules accept;
    NavBakeParams nav;
    std::optional<TerrainRecipe> fallback;
    std::uint64_t fallback_hash = 0;
};

struct AttemptReport {
    std::int32_t seed = 0;
    std::string stage;                       // "retry", "relax", "fallback"
    LayoutReport layout;
    NavCheck check;
    NavBakeStats bake;
    double compose_ms = 0, layout_ms = 0;
    bool accepted = false;
    std::string reason;                      // why it was turned down
};

enum class GenerateOutcome { kGenerated, kRelaxed, kFallback, kFailed };

struct GenerateResult {
    GenerateOutcome outcome = GenerateOutcome::kFailed;
    TerrainRecipe recipe;
    HeightGrid heights;
    std::uint64_t hash = 0;
    dtNavMesh* navmesh = nullptr;            // owned by the caller (dtFreeNavMesh)
    std::vector<AttemptReport> attempts;
};

// Seed of attempt k: a hash of the request seed and k, so attempts are
// reproducible one by one.
std::int32_t attempt_seed(std::int32_t seed, std::int32_t attempt);

bool generate_terrain(const GenerateRequest& request, const TerrainLibrary& library, GenerateResult* out, std::string* error);

std::string format_attempt(const AttemptReport& a, const TerrainLibrary& library);

}  // namespace network_example::game_server::terrain
