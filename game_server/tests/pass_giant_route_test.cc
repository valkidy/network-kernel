// The mission's giants walk their authored routes, on the real map.
//
// A giant's stance is 5.6 m and the navmesh is baked for a 0.6 m agent, so the
// only meaningful check is against the shipping terrain itself: the baked
// collision under the character controller, and the baked navmesh joining the
// waypoints. A filesystem catalog load reads neither artifact (only a bundle
// does), so both are handed over here exactly as the bundle path would.
//
// What is asserted is the shape of the walk -- every giant gets to both ends of
// its route and stays on the ground -- not the timing, which follows move speed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

std::vector<std::uint8_t> read_catalog_file(const std::string& entry_path) {
    std::ifstream file(catalog_root() / entry_path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

std::vector<KernelServerEntityState> giants(
    KernelHandle* kernel,
    std::uint32_t giant_template) {
    std::vector<KernelServerEntityState> states(256);
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        KernelEntityType_Actor,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    states.resize(count);
    std::erase_if(states, [giant_template](const KernelServerEntityState& state) {
        return state.entity_template_id != giant_template;
    });
    return states;
}

}  // namespace

int main() {
    network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config(100);
    require(!config.navigation_mesh.entry_path.empty());
    config.navigation_mesh.artifact =
        read_catalog_file(config.navigation_mesh.entry_path);
    const std::vector<std::uint8_t> scene =
        read_catalog_file(config.static_collision_scene.entry_path);
    const std::uint32_t giant_template =
        template_id_of(config, "gingerbread_giant");
    require(giant_template != 0);
    // Only the mission's giants: the patrols spawn the same template.
    config.patrols.clear();

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 64;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));
    // Before the server starts: the kernel refuses a scene after.
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 7873));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());

    // The mission opens on a player's arrival, which places the giants.
    KernelServerEntityCreateInfo player{};
    player.struct_size = sizeof(player);
    player.entity_type = network_example::game_server::kEntityTypeActor;
    player.actor_type = network_example::game_server::kActorTypePlayer;
    player.position = config.player.spawn_position;
    player.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player, &player_net_id));
    for (int tick = 0; tick < 30; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }

    // Each giant, with the route it was spawned on.
    struct Walk {
        std::uint32_t net_id = 0;
        std::vector<KernelVec3> route;
        std::vector<float> closest;
        float lowest = 1e9f;
    };
    std::vector<Walk> walks;
    for (const auto& rule : config.game_rules) {
        if (rule.name != "game_rule") continue;
        for (const auto& node : rule.nodes) {
            if (node.spawn.entity_template_id != giant_template) continue;
            for (const KernelServerEntityState& giant : giants(kernel, giant_template)) {
                if (std::hypot(giant.position.x - node.spawn.position.x,
                               giant.position.z - node.spawn.position.z) < 5.0f) {
                    walks.push_back(Walk{
                        giant.net_id,
                        node.spawn.controller.route,
                        std::vector<float>(node.spawn.controller.route.size(), 1e9f)});
                }
            }
        }
    }
    require(walks.size() == 4u);

    // Long enough for the longest route (79 m) out and back.
    for (int tick = 0; tick < 3300; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        for (const KernelServerEntityState& giant : giants(kernel, giant_template)) {
            for (Walk& walk : walks) {
                if (walk.net_id != giant.net_id) continue;
                walk.lowest = std::min(walk.lowest, giant.position.y);
                for (std::size_t point = 0; point < walk.route.size(); ++point) {
                    walk.closest[point] = std::min(
                        walk.closest[point],
                        std::hypot(giant.position.x - walk.route[point].x,
                                   giant.position.z - walk.route[point].z));
                }
            }
        }
    }
    for (const Walk& walk : walks) {
        std::fprintf(stderr, "giant #%u lowest y %.2f, closest to its route's points:", walk.net_id, walk.lowest);
        for (const float closest : walk.closest) {
            std::fprintf(stderr, " %.1f", closest);
        }
        std::fprintf(stderr, "\n");
    }
    for (const Walk& walk : walks) {
        // The map's floor is at y 8; below it is a giant that fell through.
        require(walk.lowest > 8.0f);
        for (const float closest : walk.closest) {
            require(closest < 4.0f);
        }
    }

    Kernel_Destroy(kernel);
    return 0;
}
