// Scene templates in the catalog (docs/SCENE_TABLE_PLAN.md S2).
//
// - The shipping bundle: the root (the camp) lists the templates but does not
//   resolve them; a template resolves into a whole config with its own map,
//   no mission, the root's scene ids, and a recipe cooked into the bundle
//   that composes a different map from the camp's.
// - Edited copies of the catalog on disk: what a template must and must not
//   say, the version it needs, and that the catalog hash follows the
//   templates although the root never loads them.

#include <cmath>
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

// `text` without its top-level `key:` block (the key line and the indented or
// blank lines under it), wherever it sits; unchanged if it has none.
std::string without_key(const std::string& text, const std::string& key) {
    std::string out;
    bool dropping = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1u;
        const std::string line = text.substr(at, end - at);
        at = end;
        const bool top = !line.empty() && line[0] != ' ' && line[0] != '\n';
        if (top) dropping = line.rfind(key + ":", 0) == 0;
        if (!dropping) out += line;
    }
    return out;
}

// Whether `config` places a prop of the entity template `name`.
bool has_prop(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::EntityTemplateConfig& t : config.entity_templates) {
        if (t.name != name) continue;
        for (const gs::ScenePropConfig& prop : config.scene_props) {
            if (prop.entity_template_id == t.actor_template_id) return true;
        }
    }
    return false;
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

    // The default entry starts on the initial camp: nothing hostile.
    const gs::GameServerGameplayConfig root = gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, kEntry);
    require(root.weapons.catalog_version == 21u);
    require(root.scene_template.empty());
    require(root.start_scene == "initial_camp");
    require(root.preload_director_template_ids.empty());
    require(root.patrols.empty());
    require(root.scene_props.size() == 2u);
    // 0 the camp, 1-4 levels, 5-6 the test maps; only the levels are levels.
    require(root.scene_templates.size() == 8u);
    for (const gs::SceneTemplateRef& t : root.scene_templates) require(t.is_level == (t.id >= 1u && t.id <= 4u));
    require(root.scene_templates[1].id == 1u && root.scene_templates[1].name == kLevel);
    require(root.scene_templates[2].id == 2u && root.scene_templates[3].id == 3u);
    require(root.scene_templates[1].display_name == "Terrain test (seed 2)");
    require(root.scene_templates[3].description == "One nest and the warband.");
    require(!root.scene_templates[1].overlay_yaml.empty());

    // Template 5, the map the tests were written against, plays the root's mission.
    const gs::GameServerGameplayConfig mission =
        gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, "gameplay_catalog.scene_5.yaml");
    require(mission.start_scene == "composed_mesh_test");
    require(!mission.preload_director_template_ids.empty());
    require(mission.weapons.catalog_hash != root.weapons.catalog_hash);

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
        bundle.data(), size, "gameplay_catalog.scene_6.yaml");
    const std::vector<std::uint8_t> camp_recipe =
        gs::read_cooked_terrain_recipe(bundle.data(), size, terrain_camp.terrain);
    gs::PreparedTerrain camp_map;
    require(gs::prepare_catalog_terrain(&terrain_camp, camp_recipe, gs::TerrainRole::kClient, &camp_map, &error));
    require(level_map.recipe.heights_hash != camp_map.recipe.heights_hash);
    std::printf("bundle %zu B, level recipe %zu B\n", bundle.size(), level_recipe.size());
}

// Every template that can start a session has an entry of its own, which
// plays exactly what asking for that start scene does; the random level has
// none (docs/START_SCENE_PLAN.md).
void start_scene_entries() {
    const std::vector<std::uint8_t> bundle = read_file("game_server/gameplay_catalog_bundle/bundle.zip");
    const auto size = static_cast<std::uint32_t>(bundle.size());
    for (const std::uint16_t id : {0, 1, 2, 3, 5, 6}) {
        const std::string entry = "gameplay_catalog.scene_" + std::to_string(id) + ".yaml";
        const gs::GameServerGameplayConfig from_entry = gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, entry);
        const gs::GameServerGameplayConfig asked = gs::load_start_scene_config_from_bundle_memory(bundle.data(), size, kEntry, id);
        require(!from_entry.start_scene.empty() && from_entry.start_scene == asked.start_scene);
        require(from_entry.weapons.catalog_hash == asked.weapons.catalog_hash);
    }
    require(gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, "gameplay_catalog.scene_0.yaml").weapons.catalog_hash ==
        gs::load_gameplay_config_from_bundle_memory(bundle.data(), size, kEntry).weapons.catalog_hash);
    bool random_has_none = false;
    try {
        // The file itself, not a load: loading a random start scene throws too.
        gs::load_gameplay_bundle_entry_bytes(bundle.data(), size, "gameplay_catalog.scene_4.yaml");
    } catch (const std::exception&) {
        random_has_none = true;
    }
    require(random_has_none);
    std::printf("start scene entries: 0, 1, 2, 3, 5, 6 each play their template; 4 has none\n");
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

    refused(replaced(yaml, "catalog_version: 21", "catalog_version: 18"), levels, "catalog_version 20");
    refused(yaml, with(seed2, replaced(level, "preload_directors: []\n", "")), "needs preload_directors");
    refused(yaml, with(seed2, replaced(level, "name: level_terrain_seed2\n", "name: Level-Two\n")), "lower-case");
    refused(yaml, with(seed2, replaced(level, "name: level_terrain_seed2\n", "name: other_name\n")), "must be named");
    refused(yaml, with(seed2, replaced(level, "id: 1\n", "id: 70000\n")), "0..65535");
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

// Catalog version 20 (docs/START_SCENE_PLAN.md): slot 0 is a scene template,
// 0 by default, and map variants are gone.
gs::GameServerGameplayConfig load_or_die(const std::string& yaml, const LevelFiles& levels) {
    try {
        return gs::load_gameplay_config_from_catalog_file(write_catalog(yaml, levels));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "load failed: %s\n", e.what());
        std::exit(1);
    }
}

void start_scene_rules() {
    const std::string yaml = shipping_yaml();
    const LevelFiles levels = shipping_levels();
    const auto starting_on = [&](const std::string& id) {
        return replaced(yaml, "\nstart_scene: 0\n", "\nstart_scene: " + id + "\n");
    };

    // Nothing named: the camp, with the props the game flow runs on and none
    // of the root's mission.
    const gs::GameServerGameplayConfig camp =
        load_or_die(replaced(yaml, "\nstart_scene: 0\n", "\n"), levels);
    require(camp.start_scene == "initial_camp");
    require(camp.scene_template.empty());
    require(has_prop(camp, "initial_camp") && has_prop(camp, "level_terminal"));
    require(camp.patrols.empty());
    require(camp.preload_director_template_ids.empty());
    // The root names no spawn, so having one says it is the camp's.
    require(camp.player.has_spawn_position);
    require(camp.scene_templates.front().id == 0u && !camp.scene_templates.front().is_level);
    for (const gs::SceneTemplateRef& t : camp.scene_templates) require(t.is_level == (t.id >= 1u && t.id <= 4u));

    // Another start scene: a level's map and mission, but no level revive pool.
    const gs::GameServerGameplayConfig mission = load_or_die(starting_on("2"), levels);
    require(mission.start_scene == "level_composed_mission");
    require(!mission.preload_director_template_ids.empty());
    require(mission.scene_props.empty());
    const gs::GameServerGameplayConfig seed2_start = load_or_die(starting_on("1"), levels);
    require(seed2_start.start_scene == "level_terrain_seed2");
    require(seed2_start.player.respawn.revives_per_player == 0);
    const gs::GameServerGameplayConfig seed2_level =
        gs::load_scene_template_config_from_catalog_file(write_catalog(starting_on("1"), levels), "level_terrain_seed2");
    require(seed2_level.player.respawn.revives_per_player == 5);
    // Asking for a level wins over the start scene.
    require(seed2_level.scene_template == "level_terrain_seed2" && seed2_level.start_scene.empty());
    std::printf("start scene: camp by default, any fixed map by id, a level when asked\n");

    refused(yaml + "map_variant: camp\n", levels, "gone in catalog_version 20");
    refused(replaced(yaml, "catalog_version: 21", "catalog_version: 19"), levels, "start_scene needs catalog_version 20");
    refused(starting_on("99"), levels, "start_scene 99 is not a scene template");
    refused(starting_on("4"), levels, "random terrain");
    LevelFiles no_camp = levels;
    no_camp.erase("0_scene_initial_camp.yaml");
    refused(yaml, no_camp, "scene template 0, the initial camp, is missing");
    LevelFiles camp_level = levels;
    camp_level["0_scene_initial_camp.yaml"] += "level:\n  display_name: Camp\n";
    refused(yaml, camp_level, "cannot have a level: block");
    refused(replaced(yaml, "scene_template_dir: scene_templates\n", ""), levels, "start_scene needs scene_template_dir");
    // A level must name its directors; a start scene may play the root's.
    LevelFiles no_directors = levels;
    no_directors["2_scene_level_composed_mission.yaml"] =
        replaced(levels.at("2_scene_level_composed_mission.yaml"), "preload_directors:\n  - game_rule\n", "");
    refused(yaml, no_directors, "needs preload_directors");
}

// Catalog version 21 (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G1/G2): a scene prop's
// rotation and grounded flag, authored player barriers and the map-edge rule,
// all on the camp, which the start scene loads in full.
void level_authoring_keys() {
    const std::string yaml = shipping_yaml();
    const LevelFiles levels = shipping_levels();
    const std::string camp_file = "0_scene_initial_camp.yaml";
    const std::string camp_text = levels.at(camp_file);
    const auto with_camp = [&](const std::string& text) {
        LevelFiles files = levels;
        files[camp_file] = text;
        return files;
    };

    // The shipping camp is exported from unity-terrain-example and changes
    // whenever it is re-authored, so only what the game flow needs of it is
    // checked here: the camp and the terminal, and a wall at its edge (the
    // bounding-rectangle rule or walls along its outline).
    const gs::GameServerGameplayConfig shipping = load_or_die(yaml, levels);
    require(shipping.map_edge_barrier.enabled || !shipping.player_barriers.empty());
    require(has_prop(shipping, "initial_camp") && has_prop(shipping, "level_terminal"));

    // The keys themselves on a fixed camp: the shipping one with its
    // authored keys replaced, so nothing below reads what was exported.
    const std::string fixture = without_key(without_key(without_key(camp_text,
        "scene_props"), "player_barriers"), "map_edge_barrier") +
        "scene_props:\n"
        "  - entity_template: initial_camp\n"
        "    position: {x: 0.0, y: 0.0, z: 0.0}\n"
        "    grounded: true\n"
        "  - entity_template: level_terminal\n"
        "    position: {x: 5.0, y: 0.0, z: 6.0}\n"
        "    rotation: {x: 0.0, y: 0.5, z: 0.0, w: 0.866025}\n"
        "    grounded: true\n"
        "map_edge_barrier: {height: 4.0}\n";
    const gs::GameServerGameplayConfig fixed = load_or_die(yaml, with_camp(fixture));
    require(fixed.map_edge_barrier.enabled && fixed.map_edge_barrier.height_meters == 4.0f);
    require(fixed.player_barriers.empty());
    require(fixed.scene_props.size() == 2u);
    for (const gs::ScenePropConfig& prop : fixed.scene_props) require(prop.grounded);
    require(fixed.scene_props.front().rotation.w == 1.0f);
    require(std::fabs(fixed.scene_props.back().rotation.y - 0.5f) < 1e-5f);

    const std::string terminal_rotation = "    rotation: {x: 0.0, y: 0.5, z: 0.0, w: 0.866025}\n";
    const std::string authored = replaced(fixture, terminal_rotation,
        "    rotation: {x: 0.0, y: 2.0, z: 0.0, w: 0.0}\n") +
        "player_barriers:\n"
        "  - {center: {x: 0.0, y: 2.0, z: -6.0}, half_extents: {x: 6.0, y: 2.0, z: 0.25}, yaw_degrees: 90}\n";
    std::uint64_t authored_hash = 0;
    std::string why;
    require(loads(yaml, with_camp(authored), &why, &authored_hash));
    const gs::GameServerGameplayConfig config = load_or_die(yaml, with_camp(authored));
    const gs::ScenePropConfig& terminal = config.scene_props.back();
    require(terminal.grounded);
    // Normalised: (0, 2, 0, 0) is a half turn about +y.
    require(terminal.rotation.y == 1.0f && terminal.rotation.w == 0.0f);
    require(config.player_barriers.size() == 1u);
    require(std::fabs(config.player_barriers[0].yaw_radians - 1.5707964f) < 1e-5f);
    std::uint64_t fixed_hash = 0;
    require(loads(yaml, with_camp(fixture), &why, &fixed_hash));
    // Barriers are predicted against: both ends must see a change in them.
    require(authored_hash != fixed_hash);
    std::printf("level authoring: rotation, grounded and barriers load and are hashed\n");

    refused(yaml, with_camp(fixture + "player_barriers: {}\n"), "player_barriers must be a sequence");
    refused(yaml, with_camp(fixture + "player_barriers:\n  - {center: {x: 0, y: 0, z: 0}}\n"),
        "needs center and half_extents");
    refused(yaml, with_camp(fixture +
        "player_barriers:\n  - {center: {x: 0, y: 0, z: 0}, half_extents: {x: 0, y: 1, z: 1}}\n"),
        "half extents in (0,");
    refused(yaml, with_camp(fixture +
        "player_barriers:\n  - {center: {x: 0, y: 0, z: 0}, half_extents: {x: 1, y: 1, z: 1}, tilt: 3}\n"),
        "tilt");
    refused(yaml, with_camp(replaced(fixture, "map_edge_barrier: {height: 4.0}", "map_edge_barrier: {height: 0.0}")),
        "map_edge_barrier needs positive");
    refused(yaml, with_camp(replaced(fixture, "map_edge_barrier: {height: 4.0}", "map_edge_barrier: {height: 4.0, colour: red}")),
        "colour");
    refused(yaml, with_camp(replaced(fixture, terminal_rotation, "    rotation: {x: 0, y: 0, z: 0, w: 0}\n")),
        "non-zero quaternion");
}

// Stamp sites in the stamp yaml (docs/LEVEL_AUTHORING_TOOLS_PLAN.md §3.2): the
// test catalog's stamp folder becomes a copy whose first stamp is edited, and
// the random level -- the one that composes stamps -- is loaded from it.
void stamp_site_keys() {
    namespace fs = std::filesystem;
    const std::string yaml = shipping_yaml();
    const LevelFiles levels = shipping_levels();
    const fs::path file = write_catalog(yaml, levels);
    const fs::path stamps = file.parent_path() / "stamp_templates";
    const fs::path real = fs::absolute("game_server/gameplay_catalog/stamp_templates");
    const std::string stamp_file = "1_stamp_fortress_0001.yaml";
    std::string original;
    {
        std::ifstream in(real / stamp_file);
        std::stringstream text;
        text << in.rdbuf();
        original = text.str();
    }
    const auto with_stamp = [&](const std::string& text) {
        fs::remove_all(stamps);
        fs::create_directories(stamps);
        for (const fs::directory_entry& e : fs::directory_iterator(real)) {
            fs::copy_file(e.path(), stamps / e.path().filename());
        }
        std::ofstream(stamps / stamp_file, std::ios::trunc) << text;
    };
    const auto level = [&]() {
        return gs::load_scene_template_config_from_catalog_file(file.string(), "level_random");
    };
    const auto refused_stamp = [&](const std::string& text, const std::string& expected) {
        with_stamp(text);
        std::string why;
        try {
            level();
        } catch (const std::exception& e) {
            why = e.what();
        }
        std::printf("refused stamp: %s\n", why.substr(0, why.find(": /")).c_str());
        require(!why.empty() && why.find(expected) != std::string::npos);
    };

    const std::string sites =
        "sites:\n"
        "  - name: courtyard\n"
        "    position: {x: 12.0, y: 0.0, z: 12.0}\n"
        "    yaw_degrees: 90\n"
        "    options:\n"
        "      - {weight: 10}\n"
        "      - {weight: 20, entity_template: gingerbread_nest}\n"
        "      - {weight: 70, entity_template: initial_camp, grounded: false}\n"
        "  - name: gate\n"
        "    options:\n"
        "      - {entity_template: level_terminal, position: {x: 3, y: 6, z: 12}, rotation: {x: 0, y: 0, z: 0, w: 1}}\n"
        "per_map_limits:\n"
        "  level_terminal: {max: 1}\n"
        "player_barriers:\n"
        "  - {center: {x: 12, y: 2, z: 3}, half_extents: {x: 4, y: 2, z: 0.5}}\n";
    with_stamp(original + sites);
    const gs::GameServerGameplayConfig config = level();
    const gs::StampContentConfig& content = config.terrain.stamp_contents.front();
    require(config.terrain.stamp_template_ids.front() == 1u);
    require(content.has_sites && content.sites.size() == 2u);
    const gs::StampSiteDefinition& courtyard = content.sites[0];
    require(courtyard.options.size() == 3u);
    require(courtyard.options[0].entity_template_id == 0u);  // nothing
    require(courtyard.options[1].entity_template_id != 0u);
    // The site's position, facing and grounding fill what an option leaves out.
    require(courtyard.options[2].position.x == 12.0f && courtyard.options[2].grounded == false);
    require(courtyard.options[1].grounded);
    require(std::fabs(courtyard.options[1].rotation.y - 0.70710677f) < 1e-5f);
    require(content.sites[1].options[0].position.y == 6.0f);
    require(content.per_map_limits.size() == 1u && content.per_map_limits[0].max == 1u &&
        content.per_map_limits[0].entity_template_id != 0u);
    require(content.player_barriers.size() == 1u);
    // The other stamps author nothing, and keep the old centre nest.
    require(!config.terrain.stamp_contents.back().has_sites);
    std::printf("stamp sites: options, defaults, limits and barriers load\n");

    refused_stamp(original + "nest_sites:\n  - {x: 12, z: 12}\n", "nest_sites");
    refused_stamp(original + replaced(sites, "entity_template: initial_camp", "entity_template: no_such_thing"),
        "unknown entity_template name");
    refused_stamp(original + replaced(sites, "position: {x: 12.0, y: 0.0, z: 12.0}", "position: {x: 1.0, y: 0.0, z: 12.0}"),
        "2 m rim");
    refused_stamp(original + replaced(sites, "{weight: 10}", "{weight: 0}"), "weight must be at least 1");
    refused_stamp(original + replaced(sites, "level_terminal: {max: 1}", "level_terminal: {min: 2, max: 1}"),
        "min over max");
    refused_stamp(original + replaced(sites, "yaw_degrees: 90\n", "yaw_degrees: 90\n    rotation: {x: 0, y: 0, z: 0, w: 1}\n"),
        "both rotation and yaw_degrees");
    refused_stamp(original + "sites:\n  - name: empty\n    options: []\n", "needs at least one option");

    // Put the shared folder back for anything after this.
    fs::remove_all(stamps);
    fs::create_directory_symlink(real, stamps);
}

// terrain.placements (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G5), on the shipping
// hand-placed template: what it may not be combined with, and what it names.
void placement_keys() {
    const std::string yaml = shipping_yaml();
    const LevelFiles levels = shipping_levels();
    const std::string file = "8_scene_placed_terrain_test.yaml";
    const std::string text = levels.at(file);
    const auto with = [&](const std::string& changed) {
        LevelFiles files = levels;
        files[file] = changed;
        return files;
    };
    const auto load = [&](const LevelFiles& files, std::string* why) {
        try {
            const gs::GameServerGameplayConfig c =
                gs::load_scene_template_config_from_catalog_file(write_catalog(yaml, files), "placed_terrain_test");
            return c.terrain.fixed_placements.has_value() ? static_cast<int>(c.terrain.fixed_placements->size()) : -1;
        } catch (const std::exception& e) {
            *why = e.what();
            return -2;
        }
    };
    std::string why;
    require(load(levels, &why) == 12);
    const gs::GameServerGameplayConfig placed =
        gs::load_scene_template_config_from_catalog_file(write_catalog(yaml, levels), "placed_terrain_test");
    // Centre (78, 54) of a 36 m stamp on a 216 m map: low corner at 168 m, 144 m.
    const gs::terrain::FixedPlacement& first = placed.terrain.fixed_placements->front();
    require(first.x_cells == 560 && first.z_cells == 480 && first.turns == 1 && first.mirror && !first.has_base);
    std::printf("placements: 12 load, centres become low corners\n");

    const auto refused_with = [&](const std::string& changed, const std::string& expected) {
        std::string reason;
        require(load(with(changed), &reason) == -2);
        std::printf("refused placements: %s\n", reason.substr(0, reason.find(": /")).c_str());
        require(reason.find(expected) != std::string::npos);
    };
    refused_with(replaced(text, "  ground_maps:", "  stamps: {count: 3}\n  ground_maps:"), "do not apply");
    refused_with(replaced(text, "  ground_maps:", "  max_retries: 2\n  ground_maps:"), "do not apply");
    refused_with(replaced(text, "{stamp: depression_0001,", "{stamp: no_such_stamp,"), "has no template");
    refused_with(replaced(text, "turns: 1, mirror: true}", "turns: 5, mirror: true}"), "turns must be 0..3");
    refused_with(replaced(text, "{stamp: depression_0001, position: {x: 78.0, z: 54.0},",
        "{stamp: depression_0001, position: {x: 78.0, y: 1.0, z: 54.0},"), "y");
    refused_with(replaced(replaced(text, "  seed: 1\n", "  seed: -1\n"), "  recipe: terrain/placed_terrain_test.nktr\n", ""),
        "seed -1 does not apply");
}

}  // namespace

int main() {
    bundle_templates();
    start_scene_entries();
    catalog_rules();
    start_scene_rules();
    level_authoring_keys();
    stamp_site_keys();
    placement_keys();
    std::printf("scene_template_catalog_test passed\n");
    return 0;
}
