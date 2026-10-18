// A joining player is put at the catalog's player.spawn_position, on the real
// map.
//
// The kernel spawns every player at the origin; game_server moves it on
// PlayerJoined. The shipping spawn is on the mound beside the camp pad, ~5 m
// up, so the check is against the baked collision scene: the player ends up
// there and standing on it, not at the origin and not falling through.

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

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    require(config.player.has_spawn_position);
    const KernelVec3 spawn = config.player.spawn_position;
    const std::vector<std::uint8_t> scene =
        read_catalog_file(config.static_collision_scene.entry_path);

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
    require(Kernel_StartDedicatedServer(kernel, 7874));

    network_example::game_server::GameServer game_server(kernel, config);

    // Where the kernel puts every player, and has stepped once, before the
    // join reaches game_server -- the order a real join arrives in.
    KernelServerEntityCreateInfo player{};
    player.struct_size = sizeof(player);
    player.entity_type = network_example::game_server::kEntityTypeActor;
    player.actor_type = network_example::game_server::kActorTypePlayer;
    player.position = KernelVec3{0.0f, 0.0f, 0.0f};
    player.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t player_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player, &player_net_id));
    Kernel_Update(kernel, kTickSeconds);

    KernelEvent joined{};
    joined.type = KernelEventType_PlayerJoined;
    joined.net_id = player_net_id;
    joined.peer_id = 1;
    game_server.handle_event(joined);
    for (int tick = 0; tick < 60; ++tick) {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
    }

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, player_net_id, &state));
    std::fprintf(
        stderr,
        "player at (%.2f, %.2f, %.2f) flags 0x%x; spawn (%.1f, %.1f, %.1f)\n",
        state.position.x,
        state.position.y,
        state.position.z,
        state.visual_flags,
        spawn.x,
        spawn.y,
        spawn.z);
    require(std::abs(state.position.x - spawn.x) < 0.5f);
    require(std::abs(state.position.z - spawn.z) < 0.5f);
    // Settled onto the mound, not sunk through it or still dropping: the
    // mound top under the spawn is ~4.7 m.
    require(state.position.y > 4.0f && state.position.y < 5.5f);
    require((state.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u);

    Kernel_Destroy(kernel);
    return 0;
}
