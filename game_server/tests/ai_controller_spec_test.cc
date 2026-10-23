// P1b of docs/AI_CONTROLLER_PLAN.md: spawn sources choose their agents'
// controller. A spawn_group, a spawner or a world rule may name a `controller:`
// block; a squad makes every member patrol. The catalog refuses a controller
// that does not suit the template it spawns, and the runtime applies what a
// source asked for once it has discovered the agent.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;

fs::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

gs::GameServerGameplayConfig load(const fs::path& root) {
    return gs::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

void write_yaml(const fs::path& path, const YAML::Node& node) {
    YAML::Emitter out;
    out << node;
    std::ofstream file(path);
    file << out.c_str() << "\n";
    require(file.good());
}

// Copies the catalog, lets `edit` change one file, and reports whether the
// result loads.
bool loads_after(
    const char* label,
    const std::string& relative,
    const std::function<void(YAML::Node&)>& edit) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / "edited";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    const fs::path file = root / relative;
    YAML::Node node = YAML::LoadFile(file.string());
    edit(node);
    write_yaml(file, node);
    try {
        (void)load(root);
        std::printf("[loads] %s\n", label);
        return true;
    } catch (const std::exception& error) {
        std::printf("[refused] %s: %s\n", label, error.what());
        return false;
    }
}

const char* kGameRule = "entity_templates/101_director_game_rule.yaml";

// The giant_open_combat node's spawn_group (a sentry passive walker today).
YAML::Node giant_spawn(YAML::Node& root) {
    for (YAML::Node node : root["director"]["graph"]["nodes"]) {
        if (node["id"].as<std::string>() == "giant_open_combat") {
            return node["on_activate"][0];
        }
    }
    require(false);
    return {};
}

// The clear_the_nests node's spawn_group: nests are props with no ai.
YAML::Node nest_spawn(YAML::Node& root) {
    for (YAML::Node node : root["director"]["graph"]["nodes"]) {
        if (node["id"].as<std::string>() == "clear_the_nests") {
            return node["on_activate"][0];
        }
    }
    require(false);
    return {};
}

void the_shipped_route_moved_to_controller(const gs::GameServerGameplayConfig& config) {
    bool found = false;
    for (const gs::GameRuleConfig& rule : config.game_rules) {
        for (const gs::GameRuleNodeConfig& node : rule.nodes) {
            if (node.spawn.controller.route.empty()) continue;
            require(node.spawn.controller.route.size() == 9u);
            require(node.spawn.controller.role == gs::AgentControllerRole::kNone);
            found = true;
        }
    }
    require(found);
}

void mismatched_controllers_are_refused() {
    // Controls: an edit that keeps the catalog valid loads, so each refusal
    // below is the edit's and not the copy's.
    require(loads_after("giant route kept, type sentry", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["controller"]["type"] = "sentry";
        spawn["controller"]["route"].push_back(YAML::Load("{x: -60.0, y: 0.0, z: -56.0}"));
        spawn["controller"]["route"].push_back(YAML::Load("{x: -40.0, y: 0.0, z: -56.0}"));
    }));
    require(loads_after("nest spawner, type garrison", "entity_templates/208_prop_gingerbread_nest.yaml",
                        [](YAML::Node& root) { root["spawner"]["controller"]["type"] = "garrison"; }));

    require(!loads_after("legacy patrol_route", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["patrol_route"].push_back(YAML::Load("{x: -60.0, y: 0.0, z: -56.0}"));
        spawn["patrol_route"].push_back(YAML::Load("{x: -40.0, y: 0.0, z: -56.0}"));
    }));
    require(!loads_after("type patrol", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["controller"]["type"] = "patrol";
    }));
    require(!loads_after("type hunter", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["controller"]["type"] = "hunter";
    }));
    require(!loads_after("garrison on a sentry template", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["controller"]["type"] = "garrison";
    }));
    require(!loads_after("short route", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["controller"]["route"].push_back(
            YAML::Load("{x: -60.0, y: 0.0, z: -56.0}"));
    }));
    require(!loads_after("controller on a template with no ai", kGameRule, [](YAML::Node& root) {
        nest_spawn(root)["controller"]["type"] = "garrison";
    }));
    require(!loads_after("sentry on a template that moves",
                         "entity_templates/208_prop_gingerbread_nest.yaml",
                         [](YAML::Node& root) { root["spawner"]["controller"]["type"] = "sentry"; }));
    require(!loads_after("route on a spawner",
                         "entity_templates/208_prop_gingerbread_nest.yaml", [](YAML::Node& root) {
        root["spawner"]["controller"]["route"].push_back(YAML::Load("{x: 0.0, y: 0.0, z: 0.0}"));
        root["spawner"]["controller"]["route"].push_back(YAML::Load("{x: 5.0, y: 0.0, z: 0.0}"));
    }));
    require(!loads_after("sentry template in a squad",
                         "patrol_templates/1_patrol_gingerbread_warband.yaml", [](YAML::Node& root) {
        root["composition"][0]["entity_template"] = "sentry_grunt";
    }));
}

// A squad's members run as patrol and everyone else as their template says.
void squad_members_run_as_patrol(const gs::GameServerGameplayConfig& config) {
    const gs::KernelGameplayCatalogStorage storage = gs::build_kernel_gameplay_catalog(config);
    std::ifstream scene_file(catalog_root() / config.static_collision_scene.entry_path,
                             std::ios::binary);
    const std::vector<std::uint8_t> scene(
        (std::istreambuf_iterator<char>(scene_file)), std::istreambuf_iterator<char>());
    require(!scene.empty());

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 1024;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 8473));
    require(Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr));

    // A player, so the game rule opens and nothing is retired for want of one.
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = config.player.spawn_position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &player));

    gs::AgentRuntimeManager manager(kernel, config);
    std::size_t members = 0;
    for (int tick = 0; tick < 1200 && members == 0u; ++tick) {
        manager.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        for (const gs::AgentRuntimeState& agent : manager.agents()) {
            if (agent.patrol.has_slot) ++members;
        }
    }
    require(members > 0u);

    std::size_t patrol = 0;
    std::size_t others = 0;
    for (const gs::AgentRuntimeState& agent : manager.agents()) {
        const gs::ActorTemplateConfig* actor_template =
            gs::find_actor_template(config, agent.actor_template_id);
        require(actor_template != nullptr);
        if (agent.patrol.has_slot) {
            require(agent.controller_role == gs::AgentControllerRole::kPatrol);
            ++patrol;
        } else {
            // Nothing in the shipped catalog names a controller yet, so every
            // agent outside a squad keeps its template's default.
            require(agent.controller_role == actor_template->default_controller_role);
            ++others;
        }
    }
    std::printf("[runtime] patrol members %zu, others %zu\n", patrol, others);
    require(others > 0u);
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = load(catalog_root());
    the_shipped_route_moved_to_controller(config);
    mismatched_controllers_are_refused();
    squad_members_run_as_patrol(config);
    std::printf("ai_controller_spec_test passed\n");
    return 0;
}
