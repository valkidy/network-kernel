// P1b of docs/AI_CONTROLLER_PLAN.md, with the names of docs/AI_BEHAVIOR_PLAN.md
// B8: spawn sources choose their agents' behavior. A spawn_group, a spawner or
// a world rule may name a `behavior:` block, where a route makes a patrol; a
// squad makes every member squad. The catalog refuses a behavior that does not
// suit the template it spawns, and the runtime applies what a source asked for
// once it has discovered the agent.

#include <cmath>
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
#include "kernel/src/kernel_api_internal.h"

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

// The giant_open_combat node's spawn_group: a patrol, by its route.
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

// Four giants, each a patrol on a route: the narrow pass's nine waypoints,
// and the three strips' two (P1c). The world rules route by offsets. None
// says `type: patrol`; the route does.
void the_shipped_routes_are_controller_routes(const gs::GameServerGameplayConfig& config) {
    int nine = 0;
    int two = 0;
    for (const gs::GameRuleConfig& rule : config.game_rules) {
        for (const gs::GameRuleNodeConfig& node : rule.nodes) {
            if (node.spawn.controller.route.empty()) continue;
            require(node.spawn.controller.role == gs::AgentControllerRole::kPatrol);
            if (node.spawn.controller.route.size() == 9u) ++nine;
            if (node.spawn.controller.route.size() == 2u) ++two;
        }
    }
    require(nine == 1 && two == 3);
    int world_rules = 0;
    for (const gs::WorldRuleSpawnConfig& rule : config.world_rule_spawns) {
        if (rule.controller.route.size() != 2u) continue;
        require(rule.controller.route[0].x == -30.0f && rule.controller.route[1].x == 30.0f);
        ++world_rules;
    }
    require(world_rules >= 1);
}

void mismatched_controllers_are_refused() {
    // Controls: an edit that keeps the catalog valid loads, so each refusal
    // below is the edit's and not the copy's.
    require(loads_after("giant route kept, type patrol", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["behavior"]["type"] = "patrol";
    }));
    require(loads_after("nest spawner, type guard", "entity_templates/208_prop_gingerbread_nest.yaml",
                        [](YAML::Node& root) { root["spawner"]["behavior"]["type"] = "guard"; }));

    // The rename (B8): the old key and the old names are refused, not read.
    require(!loads_after("legacy controller key", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["controller"] = spawn["behavior"];
        spawn.remove("behavior");
    }));
    require(!loads_after("renamed type garrison", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["behavior"]["type"] = "garrison";
    }));
    require(!loads_after("guard with a route", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["behavior"]["type"] = "guard";
    }));
    require(!loads_after("patrol without a route", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["behavior"].remove("route");
        spawn["behavior"]["type"] = "patrol";
    }));

    require(!loads_after("legacy patrol_route", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["patrol_route"].push_back(YAML::Load("{x: -60.0, y: 0.0, z: -56.0}"));
        spawn["patrol_route"].push_back(YAML::Load("{x: -40.0, y: 0.0, z: -56.0}"));
    }));
    require(!loads_after("type squad", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["behavior"]["type"] = "squad";
    }));
    // A hunter since P6a, with its own tuning on the spawn source.
    require(loads_after("type hunter", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["behavior"].remove("route");
        spawn["behavior"]["type"] = "hunter";
        spawn["behavior"]["on_target_lost"] = "guard";
    }));
    require(!loads_after("hunter tuning without type hunter", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["behavior"]["target_source"] = "nearest_hostile";
    }));
    require(!loads_after("hunter on a sentry template", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["entity_template"] = "sentry_grunt";
        spawn["behavior"].remove("route");
        spawn["behavior"]["type"] = "hunter";
    }));
    require(!loads_after("guard on a sentry template", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["entity_template"] = "sentry_grunt";
        spawn["behavior"].remove("route");
        spawn["behavior"]["type"] = "guard";
    }));
    require(!loads_after("route on a sentry", kGameRule, [](YAML::Node& root) {
        giant_spawn(root)["entity_template"] = "sentry_grunt";
    }));
    require(!loads_after("sentry on a guard template", kGameRule, [](YAML::Node& root) {
        YAML::Node spawn = giant_spawn(root);
        spawn["behavior"].remove("route");
        spawn["behavior"]["type"] = "sentry";
    }));
    require(!loads_after("short route", kGameRule, [](YAML::Node& root) {
        YAML::Node route = giant_spawn(root)["behavior"]["route"];
        route.remove(1);
    }));
    require(!loads_after("behavior on a template with no ai", kGameRule, [](YAML::Node& root) {
        nest_spawn(root)["behavior"]["type"] = "guard";
    }));
    require(!loads_after("sentry on a template that moves",
                         "entity_templates/208_prop_gingerbread_nest.yaml",
                         [](YAML::Node& root) { root["spawner"]["behavior"]["type"] = "sentry"; }));
    require(!loads_after("route on a spawner",
                         "entity_templates/208_prop_gingerbread_nest.yaml", [](YAML::Node& root) {
        root["spawner"]["behavior"]["route"].push_back(YAML::Load("{x: 0.0, y: 0.0, z: 0.0}"));
        root["spawner"]["behavior"]["route"].push_back(YAML::Load("{x: 5.0, y: 0.0, z: 0.0}"));
    }));
    require(!loads_after("sentry template in a squad",
                         "patrol_templates/1_patrol_gingerbread_warband.yaml", [](YAML::Node& root) {
        root["composition"][0]["entity_template"] = "sentry_grunt";
    }));
}

// A squad's members run as squad and everyone else as their template says --
// or as patrol, where a route spawned them.
void squad_members_run_as_squad(const gs::GameServerGameplayConfig& config) {
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
    require(Kernel_StartDedicatedServer(kernel, 8481));
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

    std::size_t squad = 0;
    std::size_t others = 0;
    for (const gs::AgentRuntimeState& agent : manager.agents()) {
        const gs::ActorTemplateConfig* actor_template =
            gs::find_actor_template(config, agent.actor_template_id);
        require(actor_template != nullptr);
        if (agent.patrol.has_slot) {
            require(agent.controller_role == gs::AgentControllerRole::kSquad);
            ++squad;
        } else if (!agent.route_patrol.path.empty()) {
            require(agent.controller_role == gs::AgentControllerRole::kPatrol);
            ++others;
        } else {
            // Nothing in the shipped catalog names a behavior type, so every
            // agent outside a squad and off a route keeps its template's
            // default.
            require(agent.controller_role == actor_template->default_controller_role);
            ++others;
        }
    }
    std::printf("[runtime] squad members %zu, others %zu\n", squad, others);
    require(others > 0u);
    Kernel_Destroy(kernel);
}

// The one behaviour P1c adds: a patrol on a route walks it, leaves it to
// fight what it sees, and once it has forgotten the target walks back onto it.
void a_patrol_leaves_its_route_to_fight_and_returns(gs::GameServerGameplayConfig config) {
    // Only this test's world rule: a tripod (a patrol, by its route) on a 2-point route
    // across the camp pad, where the catalog puts players -- flat ground.
    const KernelVec3 pad = config.player.spawn_position;
    config.preload_director_template_ids.clear();
    config.game_rules.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    require(!config.world_rule_spawns.empty());
    gs::WorldRuleSpawnConfig rule = config.world_rule_spawns.front();
    rule.spawn_entity_template_id = 22u;  // tripod_actor
    rule.spawn_actor_template_id = 0u;
    rule.target_count = 1;
    rule.position = KernelVec3{pad.x, pad.y + 1.0f, pad.z};
    rule.radius = 0.0f;
    rule.controller.route = {KernelVec3{-10.0f, 0.0f, 0.0f}, KernelVec3{10.0f, 0.0f, 0.0f}};
    config.world_rule_spawns = {rule};
    // The preload list is what makes a director live.
    config.preload_director_template_ids = {rule.director_template_id};

    const gs::KernelGameplayCatalogStorage storage = gs::build_kernel_gameplay_catalog(config);
    std::ifstream scene_file(catalog_root() / config.static_collision_scene.entry_path,
                             std::ios::binary);
    const std::vector<std::uint8_t> scene(
        (std::istreambuf_iterator<char>(scene_file)), std::istreambuf_iterator<char>());
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
    require(Kernel_StartDedicatedServer(kernel, 8475));
    require(Kernel_LoadGameplayCatalog(kernel, &storage.definition, nullptr));

    gs::AgentRuntimeManager manager(kernel, config);
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            manager.tick(kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    };
    const auto tripod = [&]() -> const gs::AgentRuntimeState& {
        require(manager.agents().size() == 1u);
        return manager.agents().front();
    };

    // Walking the route.
    run(90);
    require(tripod().controller_role == gs::AgentControllerRole::kPatrol);
    require(tripod().route_patrol.path.size() >= 2u);
    // The path walked, tick by tick, not the net displacement: at full speed
    // the tripod reaches its route's corner inside this second and turns back.
    float walked = 0.0f;
    for (int tick = 0; tick < 30; ++tick) {
        const KernelVec3 from = tripod().position;
        run(1);
        const KernelVec3 to = tripod().position;
        walked += std::hypot(to.x - from.x, to.z - from.z);
    }
    const KernelVec3 walked_to = tripod().position;
    std::printf("[patrol] route walk: %.2f m in 1 s, state %d\n", walked,
                static_cast<int>(tripod().sentry.state));
    // At its 2.5 m/s. 1.35 m/s while every re-aim dropped the controller's
    // anchor and seated the capsule at the tripod's body height.
    require(walked > 2.0f);
    require(tripod().sentry.state == gs::AgentSentryState::kIdle);

    // Players on every side 5 m off, so one is in the cone whichever way it
    // faces: it stops walking the route and engages.
    std::vector<std::uint32_t> players;
    for (const KernelVec2 side : {KernelVec2{5.0f, 0.0f}, KernelVec2{-5.0f, 0.0f},
                                  KernelVec2{0.0f, 5.0f}, KernelVec2{0.0f, -5.0f}}) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = gs::kEntityTypeActor;
        create.actor_type = gs::kActorTypePlayer;
        create.entity_template_id = config.player.actor_template_id;
        create.actor_template_id = config.player.actor_template_id;
        create.position = KernelVec3{walked_to.x + side.x, walked_to.y + 0.5f, walked_to.z + side.y};
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t player = 0;
        require(Kernel_ServerCreateEntity(kernel, &create, &player));
        players.push_back(player);
    }
    bool engaged = false;
    for (int tick = 0; tick < 60 && !engaged; ++tick) {
        run(1);
        engaged = gs::is_engaged(tripod().sentry.state);
    }
    std::printf("[patrol] engaged=%d target=%u\n", engaged ? 1 : 0, tripod().sentry.target);
    require(engaged);

    // Gone: it forgets them, goes idle, and walks the route again.
    for (const std::uint32_t player : players) {
        KernelEntityLifecycleCommand command{};
        command.struct_size = sizeof(command);
        command.command_type = KernelEntityLifecycleCommandType_Destroy;
        command.net_id = player;
        require(Kernel_ServerEnqueueEntityLifecycle(kernel, KernelCommandSource_Test, &command));
    }
    bool idle = false;
    for (int tick = 0; tick < 600 && !idle; ++tick) {
        run(1);
        idle = tripod().sentry.state == gs::AgentSentryState::kIdle;
    }
    require(idle);
    // Two seconds, not one: it may have to turn round at a route end first.
    const KernelVec3 resumed_from = tripod().position;
    run(60);
    const KernelVec3 resumed_to = tripod().position;
    const float resumed = std::hypot(resumed_to.x - resumed_from.x, resumed_to.z - resumed_from.z);
    std::printf("[patrol] after forgetting: %.2f m in 2 s\n", resumed);
    require(resumed > 1.0f);
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = load(catalog_root());
    the_shipped_routes_are_controller_routes(config);
    mismatched_controllers_are_refused();
    squad_members_run_as_squad(config);
    a_patrol_leaves_its_route_to_fight_and_returns(config);
    std::printf("ai_controller_spec_test passed\n");
    return 0;
}
