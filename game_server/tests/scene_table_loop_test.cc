// The playable loop over the scene table (docs/SCENE_TABLE_PLAN.md S4/S6),
// end to end over real connections in one process: a server built the way
// the apps build one (the shipping bundle, its camp in slot 0) and pure
// clients that sync the catalog, handshake, and follow SCENE_STATE the way
// Unity will -- load the listed slots with GameServer_ClientLoadSceneSlot,
// answer SCENE_READY, unload what is no longer listed.
//
// Run twice: as a listen host (whose own player also follows SCENE_STATE,
// without registering collision, and picks the level with SCENE_SELECT) and
// as a dedicated server (the operator picks, through request_level).
//
//   level 1: select -> everyone ready -> gate opens -> players walk in (a
//            remote client's prediction stands on the slot's ground) -> a
//            second client joins late and follows -> the mission is cleared
//            -> return gate -> everyone back -> the slot unloads everywhere.
//   level 2: same level, next generation -> the party runs out of revives
//            -> everyone back in the camp, alive -> unloaded.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/src/kernel/public/kernel_api.h"
#include "game_server/public/game_server_api.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_table.h"
#include "game_server/tests/scene_state_wire.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_table_loop_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr const char* kLevel = "level_composed_mission";
constexpr float kDt = 1.0f / 30.0f;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

KernelConfig config_for(KernelMode mode) {
    KernelConfig config{};
    config.mode = mode;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 1024;
    config.max_render_states = 2048;
    return config;
}

struct SceneState {
    bool received = false;
    std::uint8_t flow = 0;
    std::uint8_t outcome = 0;
    bool gate_open = false;
    KernelVec3 gate{};
    scene_state_wire::Gate level_gate;
    scene_state_wire::Gate return_gate;
    // slot -> (generation, template name)
    std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> slots;
};

SceneState decode_state(const KernelGameMessage& m) {
    SceneState s;
    const std::uint8_t* p = m.payload;
    const auto u32 = [](const std::uint8_t* q) {
        return static_cast<std::uint32_t>(q[0]) | (static_cast<std::uint32_t>(q[1]) << 8) |
            (static_cast<std::uint32_t>(q[2]) << 16) | (static_cast<std::uint32_t>(q[3]) << 24);
    };
    const auto f32 = [&](const std::uint8_t* q) {
        const std::uint32_t bits = u32(q);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    scene_state_wire::Header h;
    require(scene_state_wire::read_header(p, m.payload_size, &h));
    (void)f32;
    s.received = true;
    s.flow = h.flow;
    s.outcome = h.outcome;
    s.level_gate = h.level_gate;
    s.return_gate = h.return_gate;
    // Where to stand to go through whichever gate is open.
    const scene_state_wire::Gate* open = scene_state_wire::open_gate(h);
    s.gate_open = open != nullptr;
    if (open != nullptr) s.gate = scene_state_wire::step_in(*open);
    std::uint32_t at = scene_state_wire::kSlotsAt;
    const std::uint32_t count = h.slot_count;
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t slot = p[at];
        const std::uint32_t generation = u32(p + at + 1);
        const std::uint32_t name_length = p[at + 16];
        s.slots[slot] = {generation, std::string(reinterpret_cast<const char*>(p + at + 17), name_length)};
        at += 17 + name_length;
    }
    require(at == m.payload_size);
    return s;
}

// A player's side of the protocol: what Unity's client runner will do.
struct Follower {
    const char* name = "";
    KernelHandle* kernel = nullptr;
    bool registers_collision = true;  // false on a listen host's own player
    SceneState state;
    std::map<std::uint32_t, std::uint32_t> loaded;  // slot -> generation
    const std::vector<std::uint8_t>* bundle = nullptr;

    void follow() {
        KernelGameMessage messages[8];
        std::uint32_t count = 0;
        while ((count = Kernel_PollGameMessages(kernel, messages, 8u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (messages[i].message_type == GAME_SERVER_MESSAGE_SCENE_STATE) {
                    state = decode_state(messages[i]);
                }
            }
        }
        if (!state.received) return;
        const std::uint32_t flags = registers_collision ? GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION : 0u;
        for (auto it = loaded.begin(); it != loaded.end();) {
            const auto listed = state.slots.find(it->first);
            if (listed == state.slots.end() || listed->second.first != it->second) {
                require(GameServer_ClientUnloadSceneSlot(kernel, it->first, flags));
                it = loaded.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [slot, entry] : state.slots) {
            if (loaded.count(slot) != 0u) continue;
            require(GameServer_ClientLoadSceneSlot(kernel, bundle->data(),
                static_cast<std::uint32_t>(bundle->size()), kEntry, entry.second.c_str(), slot, flags));
            loaded[slot] = entry.first;
            std::uint8_t body[5] = {static_cast<std::uint8_t>(slot),
                static_cast<std::uint8_t>(entry.first), static_cast<std::uint8_t>(entry.first >> 8),
                static_cast<std::uint8_t>(entry.first >> 16), static_cast<std::uint8_t>(entry.first >> 24)};
            require(Kernel_SendGameMessage(kernel, GAME_SERVER_MESSAGE_SCENE_READY, body, 5u));
        }
    }
};

struct World {
    KernelHandle* server_kernel = nullptr;
    std::unique_ptr<gs::GameServer> server;
    std::vector<KernelHandle*> clients;
    std::vector<Follower*> followers;
    std::uint32_t host_sequence = 1;
    bool listen = false;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    void frame() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (listen) {
            KernelPlayerInput input{};
            input.input_seq = host_sequence++;
            input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
            Kernel_SubmitPlayerInput(server_kernel, 1, &input);
        }
        Kernel_Update(server_kernel, kDt);
        KernelEvent events[64];
        std::uint32_t count = 0;
        while ((count = Kernel_PollEvents(server_kernel, events, 64u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) server->handle_event(events[i]);
        }
        server->tick(kDt);
        for (KernelHandle* client : clients) {
            Kernel_Update(client, kDt);
            while (Kernel_PollEvents(client, events, 64u) != 0u) {
            }
        }
        for (Follower* follower : followers) follower->follow();
    }

    bool run_until(const std::function<bool()>& done, double seconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
            frame();
            if (done()) return true;
        }
        return false;
    }

    std::vector<KernelServerEntityState> actors() const {
        std::vector<KernelServerEntityState> states(4096);
        for (auto& s : states) s.struct_size = sizeof(s);
        const std::uint32_t count = Kernel_ServerQueryEntities(
            server_kernel, KernelEntityType_Actor, states.data(), static_cast<std::uint32_t>(states.size()));
        states.resize(count);
        return states;
    }

    std::vector<KernelServerEntityState> players() const {
        std::vector<KernelServerEntityState> out;
        for (const auto& s : actors()) {
            if (s.actor_type == KernelActorType_Player) out.push_back(s);
        }
        return out;
    }

    KernelServerEntityState state_of(std::uint32_t net_id) const {
        KernelServerEntityState s{};
        s.struct_size = sizeof(s);
        require(Kernel_ServerGetEntityState(server_kernel, net_id, &s));
        return s;
    }

    void teleport(std::uint32_t net_id, const KernelVec3& to) const {
        const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerSetEntityTransform(server_kernel, net_id, &to, &facing));
    }
};

KernelHandle* join(World* world, std::uint16_t port, const std::vector<std::uint8_t>& bundle) {
    KernelConfig config = config_for(KernelMode_Client);
    KernelHandle* client = Kernel_Create(&config);
    require(client != nullptr);
    world->clients.push_back(client);
    KernelGameplayCatalogSyncClientConfig sync{};
    sync.struct_size = sizeof(sync);
    sync.max_bundle_size = KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE;
    sync.timeout_ms = 30000u;
    require(Kernel_StartClientCatalogSync(client, ("127.0.0.1:" + std::to_string(port)).c_str(), &sync));
    const auto sync_state = [&] {
        KernelGameplayCatalogSyncStatus status{};
        status.struct_size = sizeof(status);
        require(Kernel_GetGameplayCatalogSyncStatus(client, &status));
        return status.state;
    };
    require(world->run_until([&] { return sync_state() == KernelGameplayCatalogSyncState_ManifestReady; }, 15.0));
    KernelGameplayCatalogLoadResult result{};
    require(Kernel_LoadGameplayCatalogFromMemory(
        client, bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry, &result));
    require(Kernel_ContinueClientHandshake(client));
    require(world->run_until([&] {
        KernelLocalPlayerInfo info{};
        return Kernel_GetLocalPlayerInfo(client, &info) && info.has_welcome != 0u;
    }, 15.0));
    return client;
}

std::uint32_t player_of(KernelHandle* kernel) {
    KernelLocalPlayerInfo info{};
    require(Kernel_GetLocalPlayerInfo(kernel, &info));
    return info.player_net_id;
}

void run(bool listen, std::uint16_t port) {
    std::printf("=== %s\n", listen ? "listen host" : "dedicated server");
    const std::vector<std::uint8_t> bundle_bytes = read_bundle();
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(bundle_bytes);
    const auto size = static_cast<std::uint32_t>(bundle->size());
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(bundle->data(), size, kEntry);
    const std::vector<std::uint8_t> scene =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.static_collision_scene.entry_path);
    config.navigation_mesh.artifact =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.navigation_mesh.entry_path);
    const auto level_found = std::find_if(config.scene_templates.begin(), config.scene_templates.end(),
        [](const gs::SceneTemplateRef& r) { return r.name == kLevel; });
    require(level_found != config.scene_templates.end());
    const std::uint16_t level_index = level_found->id;

    World world;
    world.listen = listen;
    KernelConfig server_config = config_for(listen ? KernelMode_ListenServer : KernelMode_DedicatedServer);
    world.server_kernel = Kernel_Create(&server_config);
    require(gs::load_kernel_gameplay_catalog(world.server_kernel, config));
    KernelStaticCollisionSceneConfig camp{};
    camp.struct_size = sizeof(camp);
    camp.artifact_bytes = scene.data();
    camp.artifact_size = static_cast<std::uint32_t>(scene.size());
    camp.scene_id = config.static_collision_scene.scene_id;
    camp.collider_id = config.static_collision_scene.collider_id;
    camp.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(world.server_kernel, &camp));
    KernelGameplayCatalogSyncServerConfig serve{};
    serve.struct_size = sizeof(serve);
    serve.bundle_bytes = bundle->data();
    serve.bundle_size = size;
    serve.entry_path = kEntry;
    serve.content_namespace = "scene_table_loop_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(world.server_kernel, &serve, &manifest));
    require(listen ? Kernel_StartListenServer(world.server_kernel, port)
                   : Kernel_StartDedicatedServer(world.server_kernel, port));
    world.server = std::make_unique<gs::GameServer>(world.server_kernel, config);
    require(world.server->preload_directors());
    world.server->enable_scene_table(gs::SceneCatalogSource{bundle, kEntry, ""});
    gs::GameServer& server = *world.server;

    Follower host{"host", world.server_kernel, false};
    host.bundle = &bundle_bytes;
    if (listen) world.followers.push_back(&host);
    Follower a{"client A", join(&world, port, bundle_bytes), true};
    a.bundle = &bundle_bytes;
    world.followers.push_back(&a);
    require(world.run_until([&] { return a.state.received; }, 5.0));
    require(a.state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a.state.slots.empty());
    std::vector<std::uint32_t> party = {player_of(a.kernel)};
    if (listen) party.push_back(player_of(world.server_kernel));
    const gs::SceneBounds camp_bounds = gs::scene_slot_bounds(0);
    const gs::SceneBounds level_bounds = gs::scene_slot_bounds(gs::GameServer::kPartyLevelSlot);

    // ---- Level 1: select.
    if (listen) {
        // Only the host may choose: a client's request is ignored.
        const std::uint8_t pick[2] = {static_cast<std::uint8_t>(level_index), static_cast<std::uint8_t>(level_index >> 8)};
        require(Kernel_SendGameMessage(a.kernel, GAME_SERVER_MESSAGE_SCENE_SELECT, pick, 2u));
        world.run_until([&] { return false; }, 0.3);
        require(server.scene_flow() == GAME_SERVER_SCENE_FLOW_CAMP);
        require(Kernel_SendGameMessage(world.server_kernel, GAME_SERVER_MESSAGE_SCENE_SELECT, pick, 2u));
    } else {
        require(server.request_level(kLevel));
    }
    require(world.run_until([&] { return server.scene_flow() == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(server.scene_ready_count() == party.size());
    require(world.run_until([&] { return a.state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 5.0));
    require(a.state.gate_open && a.loaded.count(1u) == 1u);
    std::printf("gate open at (%.1f, %.1f, %.1f); %zu ready\n", a.state.gate.x, a.state.gate.y, a.state.gate.z,
        server.scene_ready_count());
    require(camp_bounds.contains(a.state.gate.x));

    // Walk in (put each player on the gate; the server moves them on).
    for (const std::uint32_t player : party) world.teleport(player, a.state.gate);
    require(world.run_until([&] {
        return std::all_of(party.begin(), party.end(),
            [&](std::uint32_t p) { return level_bounds.contains(world.state_of(p).position.x); });
    }, 5.0));
    // Standing on the slot's ground -- on the server and in A's prediction.
    world.run_until([&] { return false; }, 2.0);
    const std::uint32_t a_player = player_of(a.kernel);
    const KernelServerEntityState on_server = world.state_of(a_player);
    RenderEntityState states[2048];
    const std::uint32_t render_count = Kernel_GetRenderStates(a.kernel, states, 2048u);
    float predicted_y = NAN;
    for (std::uint32_t k = 0; k < render_count; ++k) {
        if (states[k].net_id == a_player) predicted_y = states[k].position.y;
    }
    std::printf("A in level: server y %.2f, A's own view y %.2f (spawn y %.2f)\n", on_server.position.y,
        predicted_y, server.scene_config(1u)->player.spawn_position.y);
    require(on_server.position.y > server.scene_config(1u)->player.spawn_position.y - 4.0f);
    require(!std::isnan(predicted_y) && std::fabs(predicted_y - on_server.position.y) < 0.5f);

    // A late joiner gets the level in its first SCENE_STATE and follows.
    Follower b{"client B", join(&world, port, bundle_bytes), true};
    b.bundle = &bundle_bytes;
    world.followers.push_back(&b);
    require(world.run_until([&] { return b.loaded.count(1u) == 1u; }, 10.0));
    world.run_until([&] { return server.scene_ready_count() == party.size() + 1u; }, 5.0);
    require(server.scene_ready_count() == party.size() + 1u);
    party.push_back(player_of(b.kernel));
    world.teleport(party.back(), a.state.gate);
    require(world.run_until([&] { return level_bounds.contains(world.state_of(party.back()).position.x); }, 5.0));
    std::printf("late joiner B followed into the level\n");

    // Clear the mission: every actor and prop in the level that is not a
    // player (nests are props) or a level gate's portal or exit, until the
    // mission is done.
    const auto is_gate = [&](std::uint32_t net_id) {
        for (const auto* gate : {&server.level_gate(), &server.return_gate()}) {
            if (gate->entrance_net_id != 0u &&
                (net_id == gate->entrance_net_id || net_id == gate->entrance_net_id + 1u)) {
                return true;
            }
        }
        return false;
    };
    const auto clear_level = [&] {
        for (const std::uint16_t type : {static_cast<std::uint16_t>(KernelEntityType_Actor),
                 static_cast<std::uint16_t>(KernelEntityType_Prop)}) {
            std::vector<KernelServerEntityState> states(4096);
            for (auto& st : states) st.struct_size = sizeof(st);
            const std::uint32_t n = Kernel_ServerQueryEntities(
                world.server_kernel, type, states.data(), static_cast<std::uint32_t>(states.size()));
            for (std::uint32_t k = 0; k < n; ++k) {
                if (states[k].actor_type == KernelActorType_Player) continue;
                if (is_gate(states[k].net_id)) continue;
                if (level_bounds.contains(states[k].position.x)) {
                    Kernel_ServerDestroyEntity(world.server_kernel, states[k].net_id, KernelDespawnReason_Destroyed);
                }
            }
        }
        return server.scene_flow() == GAME_SERVER_SCENE_FLOW_RETURN_OPEN;
    };
    const bool cleared = world.run_until(clear_level, 60.0);
    if (!cleared) {
        for (const auto& rule : server.scene_manager(1u)->game_rule_director().runtimes()) {
            std::fprintf(stderr, "rule status %d nodes:", static_cast<int>(rule.status));
            for (const auto node : rule.node_states) std::fprintf(stderr, " %d", static_cast<int>(node));
            std::fprintf(stderr, "\n");
        }
    }
    require(cleared);
    require(server.scene_outcome() == GAME_SERVER_SCENE_OUTCOME_COMPLETED);
    require(world.run_until([&] { return a.state.flow == GAME_SERVER_SCENE_FLOW_RETURN_OPEN; }, 5.0));
    require(level_bounds.contains(a.state.gate.x));
    std::printf("mission cleared; return gate at x %.1f\n", a.state.gate.x);
    for (const std::uint32_t player : party) world.teleport(player, a.state.gate);
    require(world.run_until([&] { return server.scene_flow() == GAME_SERVER_SCENE_FLOW_CAMP; }, 10.0));
    require(server.scene_state(1u) == gs::SceneSlotState::kEmpty);
    for (const std::uint32_t player : party) require(camp_bounds.contains(world.state_of(player).position.x));
    require(world.run_until([&] { return a.loaded.empty() && b.loaded.empty() && host.loaded.empty(); }, 5.0));
    require(a.state.outcome == GAME_SERVER_SCENE_OUTCOME_COMPLETED);
    std::printf("level 1 complete: everyone back, slot unloaded on server and clients\n");

    // ---- Level 2: run out of revives.
    require(server.request_level(kLevel));
    require(world.run_until([&] { return server.scene_flow() == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(server.scene_generation(1u) == 1u);
    require(world.run_until([&] {
        return a.state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && camp_bounds.contains(a.state.gate.x);
    }, 5.0));
    for (const std::uint32_t player : party) world.teleport(player, a.state.gate);
    require(world.run_until([&] {
        return std::all_of(party.begin(), party.end(),
            [&](std::uint32_t p) { return level_bounds.contains(world.state_of(p).position.x); });
    }, 5.0));
    std::printf("level 2 entered: revives %d (level config %d)\n", server.remaining_revives(),
        server.scene_config(1u)->player.respawn.team_revive_times);
    require(server.remaining_revives() == 1);
    // Everyone dies; the one revive goes to one of them, who then dies too.
    require(world.run_until([&] {
        for (const std::uint32_t player : party) {
            const KernelServerEntityState s = world.state_of(player);
            // A real death: below the kill floor. (Setting health to zero
            // raises no death, so it would never spend a revive.)
            if (s.hp != 0u && level_bounds.contains(s.position.x) && s.position.y > -5.0f) {
                world.teleport(player, KernelVec3{s.position.x, -30.0f, s.position.z});
            }
        }
        return server.scene_flow() == GAME_SERVER_SCENE_FLOW_CAMP;
    }, 30.0));
    require(server.scene_outcome() == GAME_SERVER_SCENE_OUTCOME_FAILED);
    world.run_until([&] { return false; }, 0.5);
    for (const std::uint32_t player : party) {
        const KernelServerEntityState s = world.state_of(player);
        require(camp_bounds.contains(s.position.x));
        require(s.hp != 0u);
    }
    require(server.remaining_revives() == config.player.respawn.team_revive_times);
    require(world.run_until([&] { return a.loaded.empty() && b.loaded.empty(); }, 5.0));
    require(a.state.outcome == GAME_SERVER_SCENE_OUTCOME_FAILED);
    std::printf("level 2 lost: everyone back in the camp alive, revives back to the camp's (%d)\n",
        server.remaining_revives());

    for (KernelHandle* client : world.clients) {
        GameServer_ReleaseTerrain(client);
        Kernel_Destroy(client);
    }
    world.server.reset();
    GameServer_ReleaseTerrain(world.server_kernel);
    Kernel_Destroy(world.server_kernel);
}

}  // namespace

int main() {
    // Kernel tests bind real ports; 8240/8241 are unused elsewhere (test-port-collisions).
    run(true, 8240);
    run(false, 8241);
    std::printf("scene_table_loop_test passed\n");
    return 0;
}
