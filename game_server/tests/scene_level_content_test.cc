// What a generated level puts on its map (docs/SCENE_TABLE_PLAYABLE_LOOP_PLAN.md
// L4), over many seeds, because nothing else refuses a spawn in mid-air or
// underground: the spawn at the map's centre with no stamp over it, the return
// gate and the nests on the ground, reachable from the spawn, and stamp sites
// landing where the recipe's turn and mirror really put the stamp.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "game_server/src/patrol_navigation.h"
#include "game_server/src/scene_table.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_level_content_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr const char* kLevel = "level_random";
constexpr std::uint32_t kSlot = 1u;
constexpr int kSeeds = 30;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(read_bundle());
    int nests_total = 0;
    int reachable_total = 0;
    int stamps_checked = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        gs::SceneCatalogSource source;
        source.bundle = bundle;
        source.entry_path = kEntry;
        source.seed_override = seed;
        gs::PreparedScene scene = gs::prepare_scene(source, kLevel, kSlot);
        if (!scene.ok) std::fprintf(stderr, "seed %d: %s\n", seed, scene.error.c_str());
        require(scene.ok);
        const gs::GameServerGameplayConfig& config = scene.config;
        const gs::terrain::TerrainRecipe& recipe = scene.terrain_recipe.recipe;
        const float offset = gs::kSceneSlotSpacing;
        const auto ground = [&](float world_x, float z) {
            return gs::terrain_ground_height(scene.terrain_heights, world_x - offset, z);
        };
        require(config.terrain.layout.center_clear_m > 0.0);

        // The spawn: the middle of the map, on the ground, nothing built on it.
        const KernelVec3 spawn = config.player.spawn_position;
        require(config.player.has_spawn_position);
        require(spawn.x == offset && spawn.z == 0.0f);
        require(std::fabs(spawn.y - (ground(spawn.x, spawn.z) + 1.5f)) < 0.01f);
        const float clear = static_cast<float>(config.terrain.layout.center_clear_m);
        for (const gs::terrain::TerrainPlacement& p : recipe.placements) {
            const float edge = static_cast<float>(config.terrain.library.stamps[p.stamp].cells_x) * 0.3f;
            const float x0 = static_cast<float>(p.x_cells * 0.3 - recipe.cells_x * 0.15);
            const float z0 = static_cast<float>(p.z_cells * 0.3 - recipe.cells_z * 0.15);
            require(x0 >= clear || x0 + edge <= -clear || z0 >= clear || z0 + edge <= -clear);
        }

        // The gate: 3 m along +x, on the ground.
        require(config.has_return_gate_position);
        const KernelVec3 gate = config.return_gate_position;
        require(gate.x == spawn.x + 3.0f && gate.z == spawn.z);
        require(std::fabs(gate.y - ground(gate.x, gate.z)) < 0.01f);

        // Stamp points land where the composed map really has that stamp.
        for (const gs::terrain::TerrainPlacement& p : recipe.placements) {
            const gs::terrain::StampAsset& stamp = config.terrain.library.stamps[p.stamp];
            for (const float share : {0.3f, 0.45f, 0.62f}) {
                const std::int32_t i = static_cast<std::int32_t>(static_cast<float>(stamp.cells_x) * share);
                const std::int32_t j = static_cast<std::int32_t>(static_cast<float>(stamp.cells_x) * (1.0f - share * 0.7f));
                const gs::StampSiteConfig site{static_cast<float>(i) * 0.3f, static_cast<float>(j) * 0.3f};
                const KernelVec3 at = gs::stamp_site_on_map(recipe, p, stamp, site);
                const float want = static_cast<float>(p.base_cm + stamp.at(i, j)) / 100.0f;
                const float got = gs::terrain_ground_height(scene.terrain_heights, at.x, at.z);
                if (std::fabs(want - got) > 0.02f) {
                    std::fprintf(stderr, "seed %d stamp %s turns %d mirror %d: want %.2f got %.2f\n",
                        seed, stamp.name.c_str(), p.turns, p.mirror ? 1 : 0, want, got);
                }
                require(std::fabs(want - got) <= 0.02f);
                ++stamps_checked;
            }
        }

        // Nests: on the ground, inside the map, and the player can walk to them.
        gs::PatrolNavigation navigation;
        std::string error;
        require(navigation.load(config.navigation_mesh.artifact, &error));
        navigation.set_origin(config.scene_offset);
        KernelVec3 snapped{};
        require(navigation.snap(spawn, 4.0f, &snapped));
        require(!config.level_nests.positions.empty());
        require(config.level_nests.positions.size() <= config.level_nests.max_count);
        int reachable = 0;
        for (const KernelVec3& nest : config.level_nests.positions) {
            require(std::fabs(nest.y - ground(nest.x, nest.z)) < 0.01f);
            // Inside the map: level_random is 864 m a side, centred on the slot.
            require(std::fabs(nest.x - offset) < 432.0f && std::fabs(nest.z) < 432.0f);
            gs::PatrolRouteRequest route;
            route.start = spawn;
            route.end = nest;
            route.snap_half_extent_meters = 3.0f;
            std::vector<KernelVec3> waypoints;
            if (navigation.find_route(route, &waypoints)) ++reachable;
        }
        nests_total += static_cast<int>(config.level_nests.positions.size());
        reachable_total += reachable;
        printf("seed %2d: stamps %2zu, nests %zu (%d reachable), spawn y %.2f\n", seed, recipe.placements.size(),
            config.level_nests.positions.size(), reachable, spawn.y);
        require(reachable >= 1);
    }
    std::printf("%d seeds: %d stamp points matched, %d of %d nests reachable\n", kSeeds, stamps_checked, reachable_total, nests_total);
    std::printf("scene_level_content_test passed\n");
    return 0;
}
