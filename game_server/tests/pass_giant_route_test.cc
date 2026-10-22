// The narrow-pass giant walks its authored route, on the real map.
//
// Its route goes down a valley as narrow as 9 m beside a cliff, so the only
// meaningful check is against the shipping terrain itself: the baked 4-zone
// collision under the character controller, and the baked navmesh joining the
// waypoints. A filesystem catalog load reads neither artifact (only a bundle
// does), so both are handed over here exactly as the bundle path would.
//
// What is asserted is the shape of the walk -- it goes through the valley's
// narrowest stretch, out onto the open ground at its north end and down to its
// south end, and never climbs a cliff -- not the timing, which follows move
// speed.

#include <algorithm>
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
        network_example::game_server::default_game_server_gameplay_config();
    require(!config.navigation_mesh.entry_path.empty());
    config.navigation_mesh.artifact =
        read_catalog_file(config.navigation_mesh.entry_path);
    const std::vector<std::uint8_t> scene =
        read_catalog_file(config.static_collision_scene.entry_path);
    const std::uint32_t giant_template =
        template_id_of(config, "gingerbread_giant");
    require(giant_template != 0);

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
    player.position = KernelVec3{0.0f, 2.0f, 0.0f};
    player.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player, &player_net_id));
    for (int tick = 0; tick < 30; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }

    std::uint32_t pass_giant = 0;
    std::vector<std::pair<std::uint32_t, float>> sweep_lanes;
    for (const KernelServerEntityState& giant : giants(kernel, giant_template)) {
        if (giant.position.x > 0.0f && giant.position.z < 0.0f) {
            pass_giant = giant.net_id;
        } else {
            sweep_lanes.emplace_back(giant.net_id, giant.position.z);
        }
    }
    require(pass_giant != 0u);
    require(sweep_lanes.size() == 3u);

    // Long enough to walk the whole valley both ways from wherever it joins:
    // the corner nearest its spawn is mid-valley, so at worst one end, then
    // the full length to the other.
    bool corridor_deep = false;   // In the valley's narrowest stretch.
    float northmost = -1e9f;
    float southmost = 1e9f;
    float highest = -1e9f;
    float sweep_drift = 0.0f;
    float lowest = 1e9f;
    for (int tick = 0; tick < 3300; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        for (const KernelServerEntityState& giant : giants(kernel, giant_template)) {
            lowest = std::min(lowest, giant.position.y);
            if (tick % 300 == 0) {
                std::fprintf(stderr, "DIAG tick=%d giant=%u pos=(%.2f, %.2f, %.2f)\n",
                    tick, giant.net_id, giant.position.x, giant.position.y,
                    giant.position.z);
            }
            if (giant.net_id == pass_giant) {
                const KernelVec3& p = giant.position;
                corridor_deep = corridor_deep ||
                    (p.z < -40.0f && p.z > -60.0f && p.x > 44.0f && p.x < 54.0f);
                northmost = std::max(northmost, p.z);
                southmost = std::min(southmost, p.z);
                highest = std::max(highest, p.y);
                continue;
            }
            for (const auto& [net_id, lane_z] : sweep_lanes) {
                if (net_id == giant.net_id) {
                    sweep_drift =
                        std::max(sweep_drift, std::abs(giant.position.z - lane_z));
                }
            }
        }
    }
    std::fprintf(
        stderr,
        "pass giant: corridor=%d northmost_z=%.1f southmost_z=%.1f highest_y=%.2f; "
        "other giants' lane drift=%.2f lowest_y=%.2f\n",
        corridor_deep,
        northmost,
        southmost,
        highest,
        sweep_drift,
        lowest);
    require(corridor_deep);
    // Out of the valley's north end onto the open ground (its first waypoint
    // is at z 10), and down to its south end (its last waypoint, z -85).
    require(northmost > 5.0f);
    require(southmost < -80.0f);
    // The valley floor is ~0.2-0.6 m, the plateau ~11 m and the ridge up to
    // ~9 m: anything above a couple of metres is a giant that climbed out.
    require(highest < 2.0f);
    // A route only for the node that authored one: the other three still
    // sweep their lane along x.
    require(sweep_drift < 0.5f);

    Kernel_Destroy(kernel);
    return 0;
}
