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
#include <algorithm>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_table.h"
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

// The catalog from a directory: the root yaml and the level files as given,
// next to links to the real catalog's other folders (its paths are relative
// to the catalog, like the bundle's).
using LevelFiles = std::map<std::string, std::string>;

LevelFiles shipping_levels() {
    namespace fs = std::filesystem;
    LevelFiles files;
    for (const fs::directory_entry& e : fs::directory_iterator("game_server/gameplay_catalog/scene_templates")) {
        std::ifstream in(e.path());
        std::stringstream text;
        text << in.rdbuf();
        files[e.path().filename().string()] = text.str();
    }
    require(!files.empty());
    return files;
}

std::string write_catalog(const std::string& yaml, const LevelFiles& levels) {
    namespace fs = std::filesystem;
    const char* tmp = std::getenv("TEST_TMPDIR");
    const fs::path dir = fs::path(tmp != nullptr ? tmp : "/tmp") / "scene_template_catalog";
    static bool linked = false;
    if (!linked) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        for (const fs::directory_entry& e : fs::directory_iterator(fs::absolute("game_server/gameplay_catalog"))) {
            if (e.is_directory() && e.path().filename() != "scene_templates") {
                fs::create_directory_symlink(e.path(), dir / e.path().filename());
            }
        }
        linked = true;
    }
    fs::remove_all(dir / "scene_templates");
    fs::create_directories(dir / "scene_templates");
    for (const auto& [name, text] : levels) {
        std::ofstream(dir / "scene_templates" / name) << text;
    }
    const fs::path file = dir / "catalog.yaml";
    { std::ofstream(file) << yaml; }
    return file.string();
}

bool loads(const std::string& yaml, const LevelFiles& levels, std::string* why, std::uint64_t* hash = nullptr) {
    try {
        const gs::GameServerGameplayConfig config =
            gs::load_gameplay_config_from_catalog_file(write_catalog(yaml, levels));
        if (hash != nullptr) *hash = config.weapons.catalog_hash;
        return true;
    } catch (const std::exception& e) {
        *why = e.what();
        return false;
    }
}

void refused(const std::string& yaml, const LevelFiles& levels, const std::string& expected) {
    std::string why;
    require(!loads(yaml, levels, &why));
    std::printf("refused: %s\n", why.substr(0, why.find(": /")).c_str());
    require(why.find(expected) != std::string::npos);
}

void bundle_templates() {
    const std::vector<std::uint8_t> bundle = read_file("game_server/gameplay_catalog_bundle/bundle.zip");
    const auto size = static_cast<std::uint32_t>(bundle.size());

    const gs::GameServerGameplayConfig root = gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, kEntry);
    require(root.weapons.catalog_version == 19u);
    require(root.scene_template.empty());
    require(root.scene_templates.size() == 3u);
    require(root.scene_templates[0].id == 1u && root.scene_templates[0].name == kLevel);
    require(root.scene_templates[1].id == 2u && root.scene_templates[2].id == 3u);
    require(root.scene_templates[0].display_name == "Terrain test (seed 2)");
    require(root.scene_templates[2].description == "One nest and the warband.");
    require(!root.scene_templates[0].overlay_yaml.empty());
    require(!root.preload_director_template_ids.empty());  // root's mission, for tests

    // A clean camp: same levels, nothing hostile.
    const gs::GameServerGameplayConfig camp =
        gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, "gameplay_catalog.camp.yaml");
    require(camp.preload_director_template_ids.empty());
    require(camp.patrols.empty());
    require(camp.scene_props.size() == 1u);
    require(camp.scene_templates.size() == 3u);
    require(camp.weapons.catalog_hash != root.weapons.catalog_hash);

    const gs::GameServerGameplayConfig level =
        gs::load_scene_template_config_from_bundle_memory(bundle.data(), size, kEntry, kLevel);
    require(level.scene_template == kLevel);
    require(level.terrain.enabled);
    require(level.terrain.seed == 2);
    require(level.terrain.recipe_entry_path == "terrain/level_terrain_seed2.nktr");
    require(level.preload_director_template_ids.empty());
    require(level.patrols.empty());
    require(level.scene_props.empty());
    require(level.agent_routes.empty());
    require(level.player.has_spawn_position);
    require(level.static_collision_scene.scene_id == root.static_collision_scene.scene_id);
    require(level.static_collision_scene.collider_id == root.static_collision_scene.collider_id);
    require(level.weapons.catalog_hash != root.weapons.catalog_hash);
    require(level.entity_templates.size() == root.entity_templates.size());
    require(level.weapons.definitions.size() == root.weapons.definitions.size());

    // A level with its own rule and patrols.
    const gs::GameServerGameplayConfig patrol =
        gs::load_scene_template_config_from_bundle_memory(bundle.data(), size, kEntry, "level_composed_patrol");
    require(patrol.preload_director_template_ids.size() == 1u);
    require(patrol.entity_templates.size() == root.entity_templates.size() + 1u);
    const auto rule = std::find_if(patrol.game_rules.begin(), patrol.game_rules.end(), [&](const gs::GameRuleConfig& r) {
        return r.director_template_id == patrol.preload_director_template_ids[0];
    });
    require(rule != patrol.game_rules.end() && rule->nodes.size() == 2u);
    require(rule->nodes[1].has_spawn_effect);
    require(patrol.patrols.size() == 1u && patrol.patrols[0].id == 1u);

    // Patrol areas move with the slot.
    gs::GameServerGameplayConfig moved = patrol;
    moved.patrols[0].area.center = KernelVec3{5.0f, 1.0f, 7.0f};
    gs::offset_scene_config(&moved, 2u);
    require(moved.patrols.size() == 1u);
    require(moved.patrols[0].area.center.x == 5.0f + 2.0f * gs::kSceneSlotSpacing);
    require(moved.patrols[0].area.center.z == 7.0f);

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

    gs::GameServerGameplayConfig terrain_camp = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), size, "gameplay_catalog.kernel_terrain.yaml");
    const std::vector<std::uint8_t> camp_recipe =
        gs::read_cooked_terrain_recipe(bundle.data(), size, terrain_camp.terrain);
    gs::PreparedTerrain camp_map;
    require(gs::prepare_catalog_terrain(&terrain_camp, camp_recipe, gs::TerrainRole::kClient, &camp_map, &error));
    require(level_map.recipe.heights_hash != camp_map.recipe.heights_hash);
    std::printf("bundle %zu B, level recipe %zu B\n", bundle.size(), level_recipe.size());
}

void catalog_rules() {
    const std::string yaml = shipping_yaml();
    const LevelFiles levels = shipping_levels();
    const std::string seed2 = "1_scene_level_terrain_seed2.yaml";
    std::string why;
    std::uint64_t shipping_hash = 0;
    require(loads(yaml, levels, &why, &shipping_hash));

    const auto with = [&](const std::string& file, const std::string& text) {
        LevelFiles edited = levels;
        edited[file] = text;
        return edited;
    };
    const std::string level = levels.at(seed2);

    // The root never reads a level's files: a level naming a ground map that
    // does not exist still loads the catalog. Only resolving it fails.
    const LevelFiles missing = with(seed2, replaced(level, "terrain_assets/ground_0001.nkgl", "terrain_assets/missing.nkgl"));
    std::uint64_t missing_hash = 0;
    require(loads(yaml, missing, &why, &missing_hash));
    bool resolve_failed = false;
    try {
        gs::load_scene_template_config_from_catalog_file(write_catalog(yaml, missing), kLevel);
    } catch (const std::exception&) {
        resolve_failed = true;
    }
    require(resolve_failed);

    // The hash follows the levels as written.
    require(missing_hash != shipping_hash);
    std::uint64_t seed3_hash = 0;
    require(loads(yaml, with(seed2, replaced(level, "seed: 2\n", "seed: 3\n")), &why, &seed3_hash));
    require(seed3_hash != shipping_hash);
    std::uint64_t again = 0;
    require(loads(yaml, levels, &why, &again));
    require(again == shipping_hash);

    refused(replaced(yaml, "catalog_version: 19", "catalog_version: 18"), levels, "catalog_version 19");
    refused(yaml, with(seed2, replaced(level, "preload_directors: []\n", "")), "needs preload_directors");
    refused(yaml, with(seed2, replaced(level, "name: level_terrain_seed2\n", "name: Level-Two\n")), "lower-case");
    refused(yaml, with(seed2, replaced(level, "name: level_terrain_seed2\n", "name: other_name\n")), "must be named");
    refused(yaml, with(seed2, replaced(level, "id: 1\n", "id: 0\n")), "1..65535");
    refused(yaml, with(seed2, level + "unknown_key: 1\n"), "unknown_key");
    LevelFiles duplicate = levels;
    duplicate["2_scene_level_terrain_seed2.yaml"] = replaced(level, "id: 1\n", "id: 2\n");
    refused(yaml, duplicate, "duplicate scene template");
    const std::size_t from = level.find("terrain:\n");
    const std::size_t to = level.find("stamp_template_dir:");
    require(from != std::string::npos && to > from);
    std::string no_map = level;
    no_map.erase(from, to - from);
    refused(yaml, with(seed2, no_map), "needs terrain or static_collision_scene");
}

}  // namespace

int main() {
    bundle_templates();
    catalog_rules();
    std::printf("scene_template_catalog_test passed\n");
    return 0;
}
