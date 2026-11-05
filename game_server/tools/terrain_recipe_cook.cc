// Cooks a catalog's terrain recipe at bundle build time (implementation plan
// P3.2): loads <entry> from <bundle.zip>, runs the server's generation
// (layout, checks, retries, fallback) from the catalog's seed, and writes the
// accepted recipe (NKTR) to <out>, plus one report line per attempt to stdout.
//
//   terrain_recipe_cook <bundle.zip> <entry.yaml> <out.nktr>
//
// Fails (non-zero) when no map passes: a bundle never ships an unchecked map.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"

int main(int argc, char** argv) {
    namespace gs = network_example::game_server;
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <bundle.zip> <entry.yaml> <out.nktr>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bundle.empty()) {
        std::fprintf(stderr, "terrain_recipe_cook: cannot read %s\n", argv[1]);
        return 1;
    }
    try {
        const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()), argv[2]);
        if (!config.terrain.enabled) {
            std::fprintf(stderr, "terrain_recipe_cook: %s has no terrain\n", argv[2]);
            return 1;
        }
        gs::PreparedTerrain prepared;
        std::string error;
        const bool ok = gs::generate_catalog_terrain(config.terrain, &prepared, nullptr, &error);
        for (const std::string& line : prepared.report) std::printf("%s\n", line.c_str());
        if (!ok) {
            std::fprintf(stderr, "terrain_recipe_cook: %s\n", error.c_str());
            return 1;
        }
        const std::vector<std::uint8_t> bytes = gs::terrain::write_recipe(prepared.recipe);
        std::ofstream out(argv[3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::fprintf(stderr, "terrain_recipe_cook: cannot write %s\n", argv[3]);
            return 1;
        }
        std::printf("terrain_recipe_cook: %s seed=%d placements=%zu heights_hash=%016llx -> %s\n", argv[2],
            prepared.recipe.recipe.seed, prepared.recipe.recipe.placements.size(),
            static_cast<unsigned long long>(prepared.recipe.heights_hash), argv[3]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "terrain_recipe_cook: %s\n", e.what());
        return 1;
    }
}
