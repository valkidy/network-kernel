// Scene templates in the catalog (docs/SCENE_TABLE_PLAN.md S2).
//
// - The shipping bundle: the root (the camp) lists the templates but does not
//   resolve them; a template resolves into a whole config with its own map,
//   no mission, the root's scene ids, and a recipe cooked into the bundle
//   that composes a different map from the camp's.
// - Edited copies of the catalog on disk: what a template must and must not
//   say, the version it needs, and that the catalog hash follows the
//   templates although the root never loads them.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_template_catalog_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr const char* kLevel = "level_terrain_seed2";

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string shipping_yaml() {
    std::ifstream in("game_server/gameplay_catalog/gameplay_catalog.yaml");
    require(in.good());
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    require(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// The catalog from a directory: the edited yaml next to links to the real
// catalog's folders (its paths are relative to the catalog, like the bundle's).
std::string write_catalog(const std::string& yaml) {
    namespace fs = std::filesystem;
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = fs::path(tmp != nullptr ? tmp : "/tmp") / "scene_template_catalog";
    static bool linked = false;
    if (!linked) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        for (const fs::directory_entry& e : fs::directory_iterator(fs::absolute("game_server/gameplay_catalog"))) {
            if (e.is_directory()) fs::create_directory_symlink(e.path(), dir / e.path().filename());
        }
        linked = true;
    }
    const fs::path file = dir / "catalog.yaml";
    { std::ofstream(file) << yaml; }
    return file.string();
}

bool loads(const std::string& yaml, std::string* why, std::uint64_t* hash = nullptr) {
    try {
        const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_catalog_file(write_catalog(yaml));
        if (hash != nullptr) *hash = config.weapons.catalog_hash;
        return true;
    } catch (const std::exception& e) {
        *why = e.what();
        return false;
    }
}

void refused(const std::string& yaml, const std::string& expected) {
    std::string why;
    require(!loads(yaml, &why));
    std::printf("refused: %s\n", why.c_str());
    require(why.find(expected) != std::string::npos);
}

void bundle_templates() {
    const std::vector<std::uint8_t> bundle = read_file("game_server/gameplay_catalog_bundle/bundle.zip");
    const auto size = static_cast<std::uint32_t>(bundle.size());

    const gs::GameServerGameplayConfig camp = gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, kEntry);
    require(camp.weapons.catalog_version == 18u);
    require(camp.scene_template.empty());
    require(camp.scene_templates.size() == 2u);
    require(camp.scene_templates[0].name == kLevel);
    require(!camp.scene_templates[0].overlay_yaml.empty());
    require(!camp.preload_director_template_ids.empty());  // the camp's mission

    const gs::GameServerGameplayConfig level =
        gs::load_scene_template_config_from_bundle_memory(bundle.data(), size, kEntry, kLevel);
    require(level.scene_template == kLevel);
    require(level.scene_templates.size() == 2u);
    require(level.terrain.enabled);
    require(level.terrain.seed == 2);
    require(level.terrain.recipe_entry_path == "terrain/level_terrain_seed2.nktr");
    require(level.preload_director_template_ids.empty());  // no mission leaks in
    require(level.scene_props.empty());
    require(level.agent_routes.empty());
    require(level.player.has_spawn_position);
    require(level.static_collision_scene.scene_id == camp.static_collision_scene.scene_id);
    require(level.static_collision_scene.collider_id == camp.static_collision_scene.collider_id);
    require(level.weapons.catalog_hash != camp.weapons.catalog_hash);
    // Everything that is not the map is the root's.
    require(level.entity_templates.size() == camp.entity_templates.size());
    require(level.weapons.definitions.size() == camp.weapons.definitions.size());

    bool unknown_refused = false;
    try {
        gs::load_scene_template_config_from_bundle_memory(bundle.data(), size, kEntry, "no_such_level");
    } catch (const std::exception&) {
        unknown_refused = true;
    }
    require(unknown_refused);

    // The cooked recipe composes, and it is not the camp's map.
    gs::GameServerGameplayConfig level_for_prepare = level;
    const std::vector<std::uint8_t> level_recipe =
        gs::read_cooked_terrain_recipe(bundle.data(), size, level.terrain);
    require(!level_recipe.empty());
    gs::PreparedTerrain level_map;
    std::string error;
    require(gs::prepare_catalog_terrain(&level_for_prepare, level_recipe, gs::TerrainRole::kClient, &level_map, &error));
    require(!level_map.generated);
    require(!level_map.collision_artifact.empty());

    gs::GameServerGameplayConfig terrain_camp = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), size, "gameplay_catalog.kernel_terrain.yaml");
    const std::vector<std::uint8_t> camp_recipe =
        gs::read_cooked_terrain_recipe(bundle.data(), size, terrain_camp.terrain);
    gs::PreparedTerrain camp_map;
    require(gs::prepare_catalog_terrain(&terrain_camp, camp_recipe, gs::TerrainRole::kClient, &camp_map, &error));
    std::printf("level seed %d: %zu placements, heights %016llx (camp seed %d: %zu, %016llx); recipe %zu B, scene %zu B\n",
        level_map.recipe.recipe.seed, level_map.recipe.recipe.placements.size(),
        static_cast<unsigned long long>(level_map.recipe.heights_hash),
        camp_map.recipe.recipe.seed, camp_map.recipe.recipe.placements.size(),
        static_cast<unsigned long long>(camp_map.recipe.heights_hash),
        level_recipe.size(), level_map.collision_artifact.size());
    require(level_map.recipe.heights_hash != camp_map.recipe.heights_hash);
    std::printf("bundle %zu B\n", bundle.size());
}

void catalog_rules() {
    const std::string yaml = shipping_yaml();
    std::string why;
    std::uint64_t shipping_hash = 0;
    require(loads(yaml, &why, &shipping_hash));

    // The root never reads a template's files: a template naming a ground map
    // that does not exist still loads the camp. Only resolving it fails.
    const std::string missing_ground = replaced(
        yaml, "seed: 2\n      recipe: terrain/level_terrain_seed2.nktr\n      ground_maps: [terrain_assets/ground_0001.nkgl]",
        "seed: 2\n      recipe: terrain/level_terrain_seed2.nktr\n      ground_maps: [terrain_assets/missing.nkgl]");
    std::uint64_t missing_hash = 0;
    require(loads(missing_ground, &why, &missing_hash));
    bool resolve_failed = false;
    try {
        gs::load_scene_template_config_from_catalog_file(write_catalog(missing_ground), kLevel);
    } catch (const std::exception&) {
        resolve_failed = true;
    }
    require(resolve_failed);

    // The hash follows the templates as written.
    require(missing_hash != shipping_hash);
    std::uint64_t seed3_hash = 0;
    require(loads(replaced(yaml, "seed: 2\n", "seed: 3\n"), &why, &seed3_hash));
    require(seed3_hash != shipping_hash);
    std::uint64_t again = 0;
    require(loads(yaml, &why, &again));
    require(again == shipping_hash);

    refused(replaced(yaml, "catalog_version: 18", "catalog_version: 17"), "catalog_version 18");
    refused(replaced(yaml, "    preload_directors: []\n    scene_props: []\n", "    scene_props: []\n"),
        "needs preload_directors");
    refused(replaced(yaml, "  level_terrain_seed2:\n", "  Level-Two:\n"), "lower-case");
    refused(replaced(yaml, "    scene_props: []\n    agent_routes: []\n",
                "    scene_props: []\n    agent_routes: []\n    patrols: []\n"),
        "patrols");
    // No map of its own: drop the terrain block's opening line and its body.
    const std::size_t from = yaml.find("  level_terrain_seed2:\n    terrain:\n");
    require(from != std::string::npos);
    const std::size_t body = from + std::string("  level_terrain_seed2:\n").size();
    const std::size_t to = yaml.find("    stamp_template_dir: stamp_templates\n    preload_directors: []");
    require(to != std::string::npos && to > body);
    std::string no_map = yaml;
    no_map.erase(body, to - body);
    refused(no_map, "needs terrain or static_collision_scene");
}

}  // namespace

int main() {
    bundle_templates();
    catalog_rules();
    std::printf("scene_template_catalog_test passed\n");
    return 0;
}
