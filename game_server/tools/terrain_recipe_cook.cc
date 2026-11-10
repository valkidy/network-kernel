// Cooks a catalog's terrain recipe at bundle build time (implementation plan
// P3.2): loads <entry> from <bundle.zip>, runs the server's generation
// (layout, checks, retries, fallback) from the catalog's seed, and writes the
// accepted recipe (NKTR) to <out>, plus one report line per attempt to stdout.
//
//   terrain_recipe_cook <bundle.zip> <entry.yaml> <out.nktr> [<map_variant> | scene_template:<name>]
//
// With <map_variant> (catalog_version 19 and older; version 20 has none),
// cooks that one of the catalog's map_variants whichever
// one its map_variant plays, so switching the catalog to a mesh map and back
// never leaves the bundle without a recipe. With scene_template:<name>, cooks
// that one of its scene_templates (docs/SCENE_TABLE_PLAN.md S2): every terrain
// template ships its own recipe, at the path its terrain.recipe names.
//
// With --all, cooks every scene template that has a terrain with a fixed seed
// (or hand-placed stamps) into <out_dir>, each at the path its terrain.recipe
// names (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G6): what the bundle build runs, so
// a new terrain template needs no BUILD rule of its own.
//
//   terrain_recipe_cook --all <bundle.zip> <entry.yaml> <out_dir>
//
// Fails (non-zero) when no map passes: a bundle never ships an unchecked map.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"

namespace {

namespace gs = network_example::game_server;

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

int cook_all(const std::vector<std::uint8_t>& bundle, const char* entry, const char* out_dir) {
    const gs::GameServerGameplayConfig root = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), entry);
    std::set<std::string> paths;
    int cooked = 0;
    for (const gs::SceneTemplateRef& ref : root.scene_templates) {
        const gs::GameServerGameplayConfig config = gs::load_scene_template_config_from_bundle_memory(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()), entry, ref.name);
        if (!config.terrain.enabled) continue;
        if (config.terrain.seed == gs::kTerrainSeedRandom) {
            std::printf("%s: seed -1, generated when opened\n", ref.name.c_str());
            continue;
        }
        const std::string& path = config.terrain.recipe_entry_path;
        if (path.empty()) {
            std::fprintf(stderr, "terrain_recipe_cook: scene template %s has a fixed terrain and no terrain.recipe\n", ref.name.c_str());
            return 1;
        }
        if (!paths.insert(path).second) {
            std::fprintf(stderr, "terrain_recipe_cook: two scene templates name the recipe %s\n", path.c_str());
            return 1;
        }
        gs::PreparedTerrain prepared;
        std::string error;
        const bool ok = gs::generate_catalog_terrain(config.terrain, &prepared, nullptr, &error);
        for (const std::string& line : prepared.report) std::printf("%s: %s\n", ref.name.c_str(), line.c_str());
        if (!ok) {
            std::fprintf(stderr, "terrain_recipe_cook: scene template %s: %s\n", ref.name.c_str(), error.c_str());
            return 1;
        }
        const std::filesystem::path file = std::filesystem::path(out_dir) / path;
        if (!write_bytes(file, gs::terrain::write_recipe(prepared.recipe))) {
            std::fprintf(stderr, "terrain_recipe_cook: cannot write %s\n", file.string().c_str());
            return 1;
        }
        std::printf("%s: seed=%d placements=%zu heights_hash=%016llx -> %s\n", ref.name.c_str(),
            prepared.recipe.recipe.seed, prepared.recipe.recipe.placements.size(),
            static_cast<unsigned long long>(prepared.recipe.heights_hash), path.c_str());
        ++cooked;
    }
    std::printf("terrain_recipe_cook: %d recipes\n", cooked);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--all") {
        std::ifstream in(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bundle.empty()) {
            std::fprintf(stderr, "terrain_recipe_cook: cannot read %s\n", argv[2]);
            return 1;
        }
        try {
            return cook_all(bundle, argv[3], argv[4]);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "terrain_recipe_cook: %s\n", e.what());
            return 1;
        }
    }
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s <bundle.zip> <entry.yaml> <out.nktr> [<map_variant> | scene_template:<name>]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bundle.empty()) {
        std::fprintf(stderr, "terrain_recipe_cook: cannot read %s\n", argv[1]);
        return 1;
    }
    try {
        const std::string choice = argc == 5 ? argv[4] : "";
        const std::string kTemplatePrefix = "scene_template:";
        const gs::GameServerGameplayConfig config = choice.rfind(kTemplatePrefix, 0) == 0
            ? gs::load_scene_template_config_from_bundle_memory(
                  bundle.data(), static_cast<std::uint32_t>(bundle.size()), argv[2],
                  choice.substr(kTemplatePrefix.size()))
            : gs::load_gameplay_config_from_bundle_memory(
                  bundle.data(), static_cast<std::uint32_t>(bundle.size()), argv[2], choice);
        if (!config.terrain.enabled) {
            std::fprintf(stderr, "terrain_recipe_cook: %s has no terrain\n", argv[2]);
            return 1;
        }
        if (config.terrain.seed == gs::kTerrainSeedRandom) {
            std::fprintf(stderr, "terrain_recipe_cook: %s has seed -1: it is generated when opened, not cooked\n", argv[2]);
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
