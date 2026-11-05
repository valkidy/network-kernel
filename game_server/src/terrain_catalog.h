// A catalog's `terrain:` turned into what the kernel and game_server load
// (implementation plan P3.3-P3.5): the static collision scene artifact, and on
// a server the navmesh.
//
// The recipe normally comes cooked in the bundle (//game_server:terrain_recipe_cook
// writes it at bundle build time), so a dedicated server, a Unity Host and every
// client compose the same map:
//   - a cooked recipe is checked against this build's library (stamps + ground
//     maps) and, once composed, against the heights hash it recorded; either
//     mismatch is refused, never composed into a different map;
//   - no cooked recipe: a client refuses (it has nothing to agree on); a server
//     generates one (layout, checks, retries, fallback) -- which only works for
//     itself, so a server that serves a bundle must refuse to go on without one.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain/terrain_recipe.h"

class dtNavMesh;

namespace network_example::game_server {

enum class TerrainRole { kClient, kServer };

struct PreparedTerrain {
    terrain::StoredRecipe recipe;
    terrain::HeightGrid heights;
    std::vector<std::uint8_t> collision_artifact;   // for KernelStaticCollisionSceneConfig
    bool generated = false;                         // no cooked recipe: generated here
    std::vector<std::string> report;                // one line per generation attempt
};

// The cooked recipe in a bundle, or empty when the bundle has none.
std::vector<std::uint8_t> read_cooked_terrain_recipe(
    const std::uint8_t* bundle_bytes, std::uint32_t bundle_size, const TerrainCatalogConfig& terrain);

// Server-side generation from the catalog: what the cook tool runs, and what a
// server without a cooked recipe falls back to. *navmesh (optional) receives
// the accepted map's navmesh, owned by the caller.
bool generate_catalog_terrain(const TerrainCatalogConfig& terrain, PreparedTerrain* out,
    dtNavMesh** navmesh, std::string* error);

// Composes (or, on a server without a recipe, generates) the map, builds the
// collision artifact, and on a server writes the navmesh into
// config->navigation_mesh.artifact. config->terrain must be enabled.
bool prepare_catalog_terrain(GameServerGameplayConfig* config, std::span<const std::uint8_t> cooked_recipe,
    TerrainRole role, PreparedTerrain* out, std::string* error);

}  // namespace network_example::game_server
