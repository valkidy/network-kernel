// Hand-placed terrain (docs/LEVEL_AUTHORING_TOOLS_PLAN.md P7):
//   - scene template 102 places, by hand, the very stamps template 101's seed-1
//     layout chose; the bundle build cooked it with no rule of its own (G6),
//     and it composes to template 101's map exactly -- same placements, bases,
//     rings and heights hash -- so the hand path gives a stamp the base and
//     ring the layout would;
//   - a placement the generator cannot honour is refused with the reason,
//     never moved.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "terrain_placements_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

gs::terrain::StoredRecipe cooked(const std::vector<std::uint8_t>& bundle, const gs::GameServerGameplayConfig& config) {
    const std::vector<std::uint8_t> bytes = gs::read_cooked_terrain_recipe(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), config.terrain);
    require(!bytes.empty());
    gs::terrain::StoredRecipe recipe;
    std::string error;
    require(gs::terrain::parse_recipe(bytes, &recipe, &error));
    return recipe;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const auto size = static_cast<std::uint32_t>(bundle.size());
    const gs::GameServerGameplayConfig laid = gs::load_scene_template_config_from_bundle_memory(
        bundle.data(), size, "gameplay_catalog.yaml", "kernel_terrain_test");
    const gs::terrain::StoredRecipe six = cooked(bundle, laid);

    // Template 6's placements as template 8 writes them: centre in the map's
    // frame, turns, mirror. Printed so the template can be (re)authored.
    const double half = laid.terrain.base.cells_x * 0.15;
    std::printf("template 6 placements, as terrain.placements:\n");
    for (const gs::terrain::TerrainPlacement& p : six.recipe.placements) {
        const gs::terrain::StampAsset& s = laid.terrain.library.stamps[p.stamp];
        const double edge = s.cells_x * 0.3;
        std::printf("    - {stamp: %s, position: {x: %.2f, z: %.2f}, turns: %d, mirror: %s}\n", s.name.c_str(),
            p.x_cells * 0.3 - half + edge / 2.0, p.z_cells * 0.3 - half + edge / 2.0, p.turns,
            p.mirror ? "true" : "false");
    }

    const gs::GameServerGameplayConfig placed = gs::load_scene_template_config_from_bundle_memory(
        bundle.data(), size, "gameplay_catalog.yaml", "placed_terrain_test");
    require(placed.terrain.fixed_placements.has_value());
    const gs::terrain::StoredRecipe eight = cooked(bundle, placed);
    require(eight.recipe.placements.size() == six.recipe.placements.size());
    for (std::size_t i = 0; i < six.recipe.placements.size(); ++i) {
        const gs::terrain::TerrainPlacement& a = six.recipe.placements[i];
        const gs::terrain::TerrainPlacement& b = eight.recipe.placements[i];
        if (a.stamp != b.stamp || a.x_cells != b.x_cells || a.z_cells != b.z_cells || a.base_cm != b.base_cm ||
            a.ring_mm != b.ring_mm || a.turns != b.turns || a.mirror != b.mirror) {
            std::fprintf(stderr, "placement %zu: layout (%u %d,%d base %d ring %d t%d m%d) vs hand (%u %d,%d base %d ring %d t%d m%d)\n",
                i, a.stamp, a.x_cells, a.z_cells, a.base_cm, a.ring_mm, a.turns, a.mirror ? 1 : 0,
                b.stamp, b.x_cells, b.z_cells, b.base_cm, b.ring_mm, b.turns, b.mirror ? 1 : 0);
            require(false);
        }
    }
    require(eight.heights_hash == six.heights_hash);
    std::printf("hand-placed template 8 composes to template 101's map: %zu placements, heights %016llx\n",
        eight.recipe.placements.size(), static_cast<unsigned long long>(eight.heights_hash));

    // Refused, with the reason, rather than moved.
    const auto refused = [&](std::vector<gs::terrain::FixedPlacement> fixed, const std::string& expected) {
        gs::TerrainCatalogConfig t = placed.terrain;
        t.fixed_placements = std::move(fixed);
        gs::PreparedTerrain out;
        std::string error;
        require(!gs::generate_catalog_terrain(t, &out, nullptr, &error));
        std::printf("refused: %s\n", error.c_str());
        require(error.find(expected) != std::string::npos);
    };
    std::vector<gs::terrain::FixedPlacement> base = *placed.terrain.fixed_placements;
    {
        auto twice = base;
        twice.push_back(base.front());  // the same site again
        refused(twice, "overlaps");
    }
    {
        auto off = base;
        off.front().x_cells = placed.terrain.base.cells_x - 10;
        refused(off, "leaves the map");
    }
    {
        auto deep = base;
        deep.front().has_base = true;
        deep.front().base_cm = 4000;  // 40 m over ground that is ~20 m up
        refused(deep, "ring");
    }
    std::printf("terrain_placements_test passed\n");
    return 0;
}
