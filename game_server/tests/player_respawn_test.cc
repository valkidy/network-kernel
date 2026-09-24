// The respawn loop end to end on the real catalog: a listen-server player dies,
// lies dead for the authored delay, and comes back above its body with its full
// health and starting inventory -- until the team's revives run out.
//
// The death is reported by hand (health set to zero, EntityDied handed to the
// game server) rather than dealt through damage: that the kernel reports
// EntityDied for a real kill is entity_lifecycle_system_test's to show.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

using network_example::game_server::GameServer;
using network_example::game_server::GameServerGameplayConfig;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "player_respawn_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;

std::vector<std::uint8_t> read_bundle() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog_bundle" / "bundle.zip";
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

struct Harness {
    Harness(
        const GameServerGameplayConfig& config,
        const std::vector<std::uint8_t>& bundle) {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_ListenServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 30;
        kernel = Kernel_Create(&kernel_config);
        require(kernel != nullptr);
        // The map, under the catalog's own ids: with made-up ones the call
        // still succeeds and the player falls forever.
        scene_bytes = network_example::game_server::load_gameplay_bundle_entry_bytes(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            config.static_collision_scene.entry_path);
        require(!scene_bytes.empty());
        KernelStaticCollisionSceneConfig scene{};
        scene.struct_size = sizeof(scene);
        scene.artifact_bytes = scene_bytes.data();
        scene.artifact_size = static_cast<std::uint32_t>(scene_bytes.size());
        scene.scene_id = config.static_collision_scene.scene_id;
        scene.collider_id = config.static_collision_scene.collider_id;
        scene.collision_layer = config.static_collision_scene.collision_layer;
        require(Kernel_SetStaticCollisionScene(kernel, &scene));
        require(Kernel_StartListenServer(kernel, 7841));
        server = new GameServer(kernel, config);
        pump();
        require(player != 0u);
    }

    ~Harness() {
        delete server;
        Kernel_Destroy(kernel);
    }

    void pump() {
        std::vector<KernelEvent> events(256);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (events[index].type == KernelEventType_PlayerJoined && player == 0u) {
                player = events[index].net_id;
            }
            server->handle_event(events[index]);
        }
    }

    void frame() {
        server->tick(kTick);
        Kernel_Update(kernel, kTick);
        pump();
    }

    KernelServerEntityState state() const {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel, player, &out));
        return out;
    }

    std::uint32_t inventory_items() const {
        KernelInventoryContainerView container{};
        container.struct_size = sizeof(container);
        require(Kernel_CopyOwnedInventoryContainers(kernel, player, &container, 1) == 1u);
        require(Kernel_GetInventoryContainer(
            kernel, container.inventory_container_id, &container));
        return container.occupied_slot_count;
    }

    KernelInventoryContainerId container_id() const {
        KernelInventoryContainerView container{};
        container.struct_size = sizeof(container);
        require(Kernel_CopyOwnedInventoryContainers(kernel, player, &container, 1) == 1u);
        return container.inventory_container_id;
    }

    void kill() {
        require(Kernel_ServerSetEntityHealth(kernel, player, 0u));
        KernelEvent died{};
        died.type = KernelEventType_EntityDied;
        died.net_id = player;
        server->handle_event(died);
    }

    // Runs frames until the player is alive again, checking after the game
    // server's tick and before the kernel's, so a revive is seen on the very
    // tick it lands, before movement has touched it. Returns the frame count,
    // or 0 if it never came back within `limit`.
    std::uint32_t frames_until_revived(std::uint32_t limit, KernelServerEntityState* at) {
        for (std::uint32_t frames = 1; frames <= limit; ++frames) {
            server->tick(kTick);
            *at = state();
            if (at->hp != 0u) {
                return frames;
            }
            Kernel_Update(kernel, kTick);
            pump();
        }
        return 0u;
    }

    std::vector<std::uint8_t> scene_bytes;
    KernelHandle* kernel = nullptr;
    GameServer* server = nullptr;
    std::uint32_t player = 0;
};

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    GameServerGameplayConfig config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "gameplay_catalog.yaml");
    // The shipped values, straight from gameplay_catalog.yaml.
    require(config.player.respawn.delay_seconds == 4.0f);
    require(config.player.respawn.height_offset_meters == 5.0f);
    require(config.player.respawn.invulnerable_seconds == 2.0f);
    require(config.player.respawn.team_revive_times == -1);
    // One revive, so the second death is the one that sticks.
    config.player.respawn.team_revive_times = 1;

    Harness harness(config, bundle);
    for (int settle = 0; settle < 60; ++settle) {
        harness.frame();
    }
    const KernelServerEntityState alive = harness.state();
    // Standing on the map, or every height below means nothing.
    require(std::fabs(alive.position.y) < 0.1f);
    require(alive.hp == alive.max_hp);
    require(alive.max_hp == 1000u);
    const std::uint32_t starting_items = harness.inventory_items();
    require(starting_items == 5u);

    // Spend the inventory, then die.
    require(Kernel_ServerClearInventoryContainer(
        harness.kernel, harness.container_id()));
    require(harness.inventory_items() == 0u);
    harness.kill();
    const float body_y = harness.state().position.y;

    KernelServerEntityState revived{};
    const std::uint32_t frames = harness.frames_until_revived(300u, &revived);
    // 4 s at 30 Hz; the death is resolved on the first tick after it.
    require(frames == 120u);
    require(revived.hp == 1000u);
    // Open sky over the flat test map: the whole five metres.
    require(revived.position.y > body_y + 4.99f);
    require(revived.position.y < body_y + 5.01f);
    require(harness.inventory_items() == starting_items);

    // The pool of one is spent: this death stays.
    harness.kill();
    require(harness.frames_until_revived(300u, &revived) == 0u);
    require(harness.state().hp == 0u);

    // And arriving now is arriving dead.
    KernelEvent joined{};
    joined.type = KernelEventType_PlayerJoined;
    joined.net_id = harness.player;
    harness.server->handle_event(joined);
    require(harness.state().hp == 0u);

    std::puts("player_respawn_test: ok");
    return 0;
}
