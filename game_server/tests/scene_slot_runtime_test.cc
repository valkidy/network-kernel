// The scene table on the server (docs/SCENE_TABLE_PLAN.md S3), on a real
// listen host built the way the apps build one, with the shipping bundle:
// the camp (composed_mesh, its mission running) in slot 0 and the
// level_composed_mission template -- the same map and mission -- loaded into
// level slots beside it.
//
//   1. A level loads in the background and commits: its terrain is in the
//      kernel, its own AI runs, and everything its mission spawns is inside
//      its slot, while the camp's AI keeps to the camp.
//   2. A player who falls off a level comes back on that level.
//   3. Unloading clears the slot completely and touches nothing in the camp;
//      20 load/unload rounds leave nothing behind.
//   4. Two levels at once (the multiverse's smallest case) do not see each
//      other: a mission waiting for a player starts only in the slot that has
//      one.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine/src/kernel/public/kernel_api.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_table.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_slot_runtime_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr const char* kLevel = "level_composed_mission";
// Kernel tests bind real ports; 8239 is unused elsewhere (test-port-collisions).
constexpr std::uint16_t kPort = 8239;
constexpr float kDt = 1.0f / 30.0f;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Host {
    KernelHandle* kernel = nullptr;
    std::unique_ptr<gs::GameServer> server;
    std::uint32_t player = 0;
    std::uint32_t sequence = 1;

    void frame() {
        KernelPlayerInput input{};
        input.input_seq = sequence++;
        input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
        Kernel_SubmitPlayerInput(kernel, 1, &input);
        Kernel_Update(kernel, kDt);
        KernelEvent events[64];
        std::uint32_t count = 0;
        while ((count = Kernel_PollEvents(kernel, events, 64u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) server->handle_event(events[i]);
        }
        server->tick(kDt);
    }

    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }

    // Frames until the slot is Ready, up to `seconds` of wall time: the load
    // runs on a worker thread and frames here do not wait for the clock.
    bool ready_within(std::uint32_t slot, double seconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
            frame();
            if (server->scene_state(slot) == gs::SceneSlotState::kReady) {
                std::printf("slot %u ready after %.0f ms\n", slot,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                return true;
            }
            if (server->scene_state(slot) == gs::SceneSlotState::kEmpty) {
                std::fprintf(stderr, "slot %u load failed: %s\n", slot, server->scene_error(slot).c_str());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    KernelVec3 player_position() const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, player, &state));
        return state.position;
    }

    void teleport_player(const KernelVec3& to) const {
        const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(kernel, player, &to, &facing));
    }

    // Every entity of the given type, as the kernel holds them now.
    std::vector<KernelServerEntityState> entities(std::uint16_t type) const {
        std::vector<KernelServerEntityState> states(1024);
        for (KernelServerEntityState& state : states) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, type, states.data(), static_cast<std::uint32_t>(states.size()));
        require(count < states.size());
        states.resize(count);
        return states;
    }

    // Non-player entities (actors, props, projectiles) inside `bounds`.
    std::uint32_t entities_in(const gs::SceneBounds& bounds) const {
        std::uint32_t total = 0;
        for (const std::uint16_t type : {static_cast<std::uint16_t>(KernelEntityType_Actor),
                 static_cast<std::uint16_t>(KernelEntityType_Prop),
                 static_cast<std::uint16_t>(KernelEntityType_Projectile)}) {
            for (const KernelServerEntityState& state : entities(type)) {
                if (type == KernelEntityType_Actor && state.actor_type == KernelActorType_Player) continue;
                if (bounds.contains(state.position.x)) ++total;
            }
        }
        return total;
    }
};

void require_agents_inside(const gs::GameServer& server, std::uint32_t slot) {
    const gs::AgentRuntimeManager* manager = server.scene_manager(slot);
    require(manager != nullptr);
    const gs::SceneBounds bounds = gs::scene_slot_bounds(slot);
    for (const gs::AgentRuntimeState& agent : manager->agents()) {
        if (!bounds.contains(agent.position.x)) {
            std::fprintf(stderr, "slot %u agent %u at x=%.1f\n", slot, agent.net_id, agent.position.x);
        }
        require(bounds.contains(agent.position.x));
    }
}

}  // namespace

int main() {
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(read_bundle());
    const auto size = static_cast<std::uint32_t>(bundle->size());
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(bundle->data(), size, kEntry);
    require(!config.static_collision_scene.entry_path.empty());  // composed_mesh camp
    const std::vector<std::uint8_t> scene =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.static_collision_scene.entry_path);
    config.navigation_mesh.artifact =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.navigation_mesh.entry_path);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 1024;
    kernel_config.max_render_states = 2048;
    Host host;
    host.kernel = Kernel_Create(&kernel_config);
    require(host.kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(host.kernel, config));
    KernelStaticCollisionSceneConfig camp{};
    camp.struct_size = sizeof(camp);
    camp.artifact_bytes = scene.data();
    camp.artifact_size = static_cast<std::uint32_t>(scene.size());
    camp.scene_id = config.static_collision_scene.scene_id;
    camp.collider_id = config.static_collision_scene.collider_id;
    camp.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(host.kernel, &camp));
    require(Kernel_StartListenServer(host.kernel, kPort));
    host.server = std::make_unique<gs::GameServer>(host.kernel, config);
    require(host.server->preload_directors());
    host.server->enable_scene_table(gs::SceneCatalogSource{bundle, kEntry, ""});
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(host.kernel, &local));
    host.player = local.player_net_id;
    host.frames(90);  // the camp's mission sees the host and opens its waves
    gs::GameServer& server = *host.server;
    const gs::SceneBounds camp_bounds = gs::scene_slot_bounds(0);
    const gs::SceneBounds slot1 = gs::scene_slot_bounds(1);
    const gs::SceneBounds slot2 = gs::scene_slot_bounds(2);
    require(host.entities_in(camp_bounds) > 0u);

    // Refusals.
    require(!server.load_scene(0u, kLevel));
    require(!server.load_scene(gs::kSceneSlotCount, kLevel));
    require(!server.unload_scene(1u));  // empty

    // 1. Load slot 1; the player walks in; its mission plays there.
    require(server.load_scene(1u, kLevel));
    require(server.scene_state(1u) == gs::SceneSlotState::kLoading);
    require(!server.load_scene(1u, kLevel));  // busy
    require(host.ready_within(1u, 30.0));
    require(server.scene_generation(1u) == 0u);
    const gs::GameServerGameplayConfig* level = server.scene_config(1u);
    require(level != nullptr);
    require(level->scene_offset.x == gs::kSceneSlotSpacing);
    // Template 2's own spawn (106.8, its 2_scene_*.yaml), moved to slot 1.
    require(level->player.spawn_position.x == 106.8f + gs::kSceneSlotSpacing);
    require(host.entities_in(slot1) == 0u);  // nothing until a player is there
    host.teleport_player(level->player.spawn_position);
    host.frames(300);
    const KernelVec3 standing = host.player_position();
    std::printf("player in slot 1 at (%.1f, %.2f, %.1f); spawn y %.2f\n",
        standing.x, standing.y, standing.z, level->player.spawn_position.y);
    require(slot1.contains(standing.x));
    require(standing.y > level->player.spawn_position.y - 4.0f);  // on the slot's ground
    const std::uint32_t level_entities = host.entities_in(slot1);
    std::printf("slot 1 entities %u, agents %zu; camp agents %zu\n",
        level_entities, server.scene_manager(1u)->agent_count(), server.scene_manager(0u)->agent_count());
    require(level_entities > 0u);
    require(server.scene_manager(1u)->agent_count() > 0u);
    require_agents_inside(server, 1u);
    require_agents_inside(server, 0u);

    // 2. Falling off the level brings the player back on the level.
    host.teleport_player(KernelVec3{level->player.spawn_position.x, -30.0f, level->player.spawn_position.z});
    host.frames(240);
    const KernelVec3 revived = host.player_position();
    std::printf("after falling off slot 1: (%.1f, %.2f, %.1f)\n", revived.x, revived.y, revived.z);
    require(slot1.contains(revived.x));
    require(revived.y > 0.0f);

    // 3. Unload: the slot empties, the player is back at the camp, the camp
    // keeps everything it had.
    std::set<std::uint32_t> camp_ids;
    for (const std::uint16_t type : {static_cast<std::uint16_t>(KernelEntityType_Actor),
             static_cast<std::uint16_t>(KernelEntityType_Prop)}) {
        for (const KernelServerEntityState& state : host.entities(type)) {
            if (camp_bounds.contains(state.position.x)) camp_ids.insert(state.net_id);
        }
    }
    require(server.unload_scene(1u));
    host.frames(5);
    require(server.scene_state(1u) == gs::SceneSlotState::kEmpty);
    require(server.scene_generation(1u) == 1u);
    require(server.scene_manager(1u) == nullptr);
    require(host.entities_in(slot1) == 0u);
    require(camp_bounds.contains(host.player_position().x));
    for (const std::uint32_t id : camp_ids) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(host.kernel, id, &state));
    }

    for (int round = 0; round < 20; ++round) {
        require(server.load_scene(1u, kLevel));
        require(host.ready_within(1u, 30.0));
        host.teleport_player(server.scene_config(1u)->player.spawn_position);
        host.frames(45);
        require(host.entities_in(slot1) > 0u);
        require_agents_inside(server, 1u);
        require(server.unload_scene(1u));
        host.frames(3);
        require(host.entities_in(slot1) == 0u);
    }
    require(server.scene_generation(1u) == 21u);
    std::printf("20 load/unload rounds: slot 1 empty after each, generation %u\n", server.scene_generation(1u));

    // 4. Two levels at once; only slot 1 has a player.
    require(server.load_scene(1u, kLevel));
    require(server.load_scene(2u, kLevel));
    require(host.ready_within(1u, 30.0));
    require(host.ready_within(2u, 30.0));
    host.teleport_player(server.scene_config(1u)->player.spawn_position);
    host.frames(300);
    require(host.entities_in(slot1) > 0u);
    require(host.entities_in(slot2) == 0u);  // its mission waits for a player
    require(server.scene_manager(2u)->agent_count() == 0u);
    require_agents_inside(server, 1u);
    host.teleport_player(server.scene_config(2u)->player.spawn_position);
    host.frames(300);
    std::printf("two levels: slot 1 entities %u, slot 2 entities %u once the player is there\n",
        host.entities_in(slot1), host.entities_in(slot2));
    require(host.entities_in(slot2) > 0u);
    require_agents_inside(server, 1u);
    require_agents_inside(server, 2u);
    require(server.unload_scene(1u));
    require(server.unload_scene(2u));
    host.frames(3);
    require(host.entities_in(slot1) == 0u);
    require(host.entities_in(slot2) == 0u);

    // 5. A terrain level composes on the worker while the host keeps ticking.
    require(server.load_scene(3u, "level_terrain_seed2"));
    int frames_while_loading = 0;
    const auto load_start = std::chrono::steady_clock::now();
    while (server.scene_state(3u) == gs::SceneSlotState::kLoading &&
           std::chrono::steady_clock::now() - load_start < std::chrono::seconds(30)) {
        host.frame();
        ++frames_while_loading;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double terrain_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start).count();
    std::printf("terrain level ready in %.0f ms, %d host frames ran meanwhile\n", terrain_ms, frames_while_loading);
    if (server.scene_state(3u) != gs::SceneSlotState::kReady) {
        std::fprintf(stderr, "terrain slot: %s\n", server.scene_error(3u).c_str());
    }
    require(server.scene_state(3u) == gs::SceneSlotState::kReady);
    require(frames_while_loading > 1);
    require(server.scene_config(3u)->terrain.enabled);
    require(server.unload_scene(3u));

    // 6. A level with everything in its own file: its own game rule spawns
    // its nest, and the warband patrol comes for the player, inside the slot.
    const gs::SceneBounds slot4 = gs::scene_slot_bounds(4u);
    require(server.load_scene(4u, "level_composed_patrol"));
    require(host.ready_within(4u, 30.0));
    host.teleport_player(server.scene_config(4u)->player.spawn_position);
    host.frames(30);
    require(host.entities_in(slot4) > 0u);  // the rule's nest
    const gs::AgentRuntimeManager* patrolled = server.scene_manager(4u);
    for (int i = 0; i < 1500 && patrolled->patrol_groups().groups().empty(); ++i) host.frame();
    std::printf("patrol level: %zu patrol groups, %zu agents, slot entities %u\n",
        patrolled->patrol_groups().groups().size(), patrolled->agent_count(), host.entities_in(slot4));
    require(!patrolled->patrol_groups().groups().empty());
    host.frames(30);
    require_agents_inside(server, 4u);
    require(server.unload_scene(4u));

    host.server.reset();
    Kernel_Destroy(host.kernel);
    std::printf("scene_slot_runtime_test passed\n");
    return 0;
}
