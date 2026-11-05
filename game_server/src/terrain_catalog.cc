#include "game_server/src/terrain_catalog.h"

#include <DetourNavMesh.h>
#include <zip.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "game_server/src/terrain/terrain_generate.h"
#include "game_server/src/terrain/terrain_navmesh.h"
#include "game_server/src/terrain/terrain_physics.h"

namespace network_example::game_server {
namespace {

std::string hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string normalized(std::string path) {
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    return path;
}

}  // namespace

std::vector<std::uint8_t> read_cooked_terrain_recipe(
    const std::uint8_t* bundle_bytes, std::uint32_t bundle_size, const TerrainCatalogConfig& terrain) {
    std::vector<std::uint8_t> out;
    if (bundle_bytes == nullptr || bundle_size == 0 || terrain.recipe_entry_path.empty()) return out;
    struct Closer { void operator()(zip_t* z) const { zip_stream_close(z); } };
    std::unique_ptr<zip_t, Closer> z(zip_stream_open(reinterpret_cast<const char*>(bundle_bytes), bundle_size, 0, 'r'));
    if (!z) return out;
    const std::string want = normalized(terrain.recipe_entry_path);
    const ssize_t n = zip_entries_total(z.get());
    for (ssize_t k = 0; k < n; ++k) {
        if (zip_entry_openbyindex(z.get(), static_cast<std::size_t>(k)) != 0) continue;
        const char* name = zip_entry_name(z.get());
        if (name != nullptr && normalized(name) == want) {
            void* buf = nullptr;
            size_t size = 0;
            if (zip_entry_read(z.get(), &buf, &size) >= 0 && buf != nullptr) {
                out.assign(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + size);
            }
            std::free(buf);
            zip_entry_close(z.get());
            break;
        }
        zip_entry_close(z.get());
    }
    return out;
}

bool generate_catalog_terrain(const TerrainCatalogConfig& t, PreparedTerrain* out, dtNavMesh** navmesh, std::string* error) {
    terrain::GenerateRequest req;
    req.base = t.base;
    req.seed = t.seed;
    req.layout = t.layout;
    req.min_stamp_count = t.min_stamp_count;
    req.max_retries = t.max_retries;
    req.accept = t.accept;
    const std::uint64_t library_hash = terrain::terrain_library_hash(t.library);
    if (!t.fallback_recipe.empty()) {
        terrain::StoredRecipe fb;
        if (!terrain::parse_recipe(t.fallback_recipe, &fb, error)) {
            *error = "terrain fallback_recipe: " + *error;
            return false;
        }
        if (fb.library_hash != library_hash) {
            *error = "terrain fallback_recipe was made from a different stamp / ground library (" +
                hex(fb.library_hash) + ", this build " + hex(library_hash) + "): regenerate it";
            return false;
        }
        req.fallback = fb.recipe;
        req.fallback_hash = fb.heights_hash;
    }
    terrain::GenerateResult res;
    const bool ok = terrain::generate_terrain(req, t.library, &res, error);
    out->report.clear();
    for (const terrain::AttemptReport& a : res.attempts) out->report.push_back(terrain::format_attempt(a, t.library));
    if (!ok) return false;
    out->recipe.recipe = std::move(res.recipe);
    out->recipe.library_hash = library_hash;
    out->recipe.heights_hash = res.hash;
    out->heights = std::move(res.heights);
    out->generated = true;
    if (navmesh != nullptr) {
        *navmesh = res.navmesh;
    } else {
        dtFreeNavMesh(res.navmesh);
    }
    return true;
}

bool prepare_catalog_terrain(GameServerGameplayConfig* config, std::span<const std::uint8_t> cooked,
    TerrainRole role, PreparedTerrain* out, std::string* error) {
    const TerrainCatalogConfig& t = config->terrain;
    *out = PreparedTerrain{};
    if (!t.enabled) {
        *error = "catalog has no terrain";
        return false;
    }
    dtNavMesh* nav = nullptr;
    if (!cooked.empty()) {
        if (!terrain::parse_recipe(cooked, &out->recipe, error)) {
            *error = t.recipe_entry_path + ": " + *error;
            return false;
        }
        const std::uint64_t library_hash = terrain::terrain_library_hash(t.library);
        if (out->recipe.library_hash != library_hash) {
            *error = t.recipe_entry_path + " was cooked from a different stamp / ground library (" +
                hex(out->recipe.library_hash) + ", this build " + hex(library_hash) + ")";
            return false;
        }
        if (!terrain::compose(out->recipe.recipe, t.library, &out->heights, error)) {
            *error = t.recipe_entry_path + ": " + *error;
            return false;
        }
        const std::uint64_t hash = terrain::height_grid_hash(out->heights);
        if (hash != out->recipe.heights_hash) {
            *error = t.recipe_entry_path + " composes to " + hex(hash) + ", recorded " + hex(out->recipe.heights_hash) +
                ": this build composes the map differently";
            return false;
        }
    } else if (role == TerrainRole::kClient) {
        *error = "the bundle has no cooked terrain recipe at " + t.recipe_entry_path;
        return false;
    } else if (!generate_catalog_terrain(t, out, &nav, error)) {
        return false;
    }

    terrain::TerrainArtifactStats stats;
    if (!terrain::build_static_scene_artifact(out->heights, terrain::TerrainArtifactOptions{}, &out->collision_artifact, &stats, error)) {
        dtFreeNavMesh(nav);
        return false;
    }
    if (role == TerrainRole::kServer) {
        if (nav == nullptr) {
            terrain::NavBakeStats bake;
            nav = terrain::bake_terrain_navmesh(out->heights, terrain::NavBakeParams{}, &bake, error);
            if (nav == nullptr) return false;
        }
        config->navigation_mesh.artifact = terrain::write_navmesh_tiles(*nav);
    }
    dtFreeNavMesh(nav);
    return true;
}

}  // namespace network_example::game_server
