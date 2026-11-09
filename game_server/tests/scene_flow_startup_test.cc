// The scene table through the paths real servers start on
// (docs/SCENE_TABLE_PLAYABLE_LOOP_PLAN.md L0): GameServer_CreateWith-
// GameplayCatalogFromMemory, which is how a Unity host makes its game server,
// and real clients over real connections following SCENE_STATE.
//
//   listen host: the host owns the room, a client's select and abort are
//     refused; SCENE_STATE keeps arriving with nothing changing; aborting
//     empties the level and brings a player inside back alive.
//   dedicated: the first player owns it; when they leave the next one does;
//     when everyone leaves the level ends; a gate nobody uses closes.
//   --level (GameServer::set_auto_level): the level opens by itself once
//     someone is connected, and again after it ends.

#include <algorithm>
#include <chrono>
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

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_flow_startup_test failed at line %d: %s\n", line, text);
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

std::uint32_t u32_at(const std::uint8_t* q) {
    return static_cast<std::uint32_t>(q[0]) | (static_cast<std::uint32_t>(q[1]) << 8) |
        (static_cast<std::uint32_t>(q[2]) << 16) | (static_cast<std::uint32_t>(q[3]) << 24);
}

struct SceneState {
    bool received = false;
    std::uint8_t flow = 0;
    std::uint32_t generation = 0;
    std::uint8_t outcome = 0;
    bool gate_open = false;
    KernelVec3 gate{};
    std::uint32_t owner = 0;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> slots;
};

struct Follower {
    KernelHandle* kernel = nullptr;
    bool registers_collision = true;
    const std::vector<std::uint8_t>* bundle = nullptr;
    SceneState state;
    int states_received = 0;
    std::map<std::uint32_t, std::uint32_t> loaded;

    void decode(const KernelGameMessage& m) {
        require(m.payload_size >= 29u);
        const std::uint8_t* p = m.payload;
        SceneState s;
        s.received = true;
        s.flow = p[0];
        s.generation = u32_at(p + 2);
        s.outcome = p[6];
        s.gate_open = p[7] != 0u;
        const auto f = [&](std::uint32_t at) {
            const std::uint32_t bits = u32_at(p + at);
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        };
        s.gate = KernelVec3{f(8), f(12), f(16)};
        s.owner = u32_at(p + 24);
        std::uint32_t at = 29;
        for (std::uint32_t k = 0; k < p[28]; ++k) {
            const std::uint32_t name_length = p[at + 7];
            s.slots[p[at]] = {u32_at(p + at + 1), std::string(reinterpret_cast<const char*>(p + at + 8), name_length)};
            at += 8 + name_length;
        }
        require(at == m.payload_size);
        state = s;
        ++states_received;
    }

    void follow() {
        KernelGameMessage messages[8];
        std::uint32_t count = 0;
        while ((count = Kernel_PollGameMessages(kernel, messages, 8u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (messages[i].message_type == GAME_SERVER_MESSAGE_SCENE_STATE) decode(messages[i]);
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
            const std::uint8_t body[5] = {static_cast<std::uint8_t>(slot),
                static_cast<std::uint8_t>(entry.first), static_cast<std::uint8_t>(entry.first >> 8),
                static_cast<std::uint8_t>(entry.first >> 16), static_cast<std::uint8_t>(entry.first >> 24)};
            require(Kernel_SendGameMessage(kernel, GAME_SERVER_MESSAGE_SCENE_READY, body, 5u));
        }
    }
};

struct World {
    KernelHandle* server_kernel = nullptr;
    std::function<void(const KernelEvent&)> on_event;
    std::function<void()> on_tick;
    bool listen = false;
    std::uint32_t sequence = 1;
    std::vector<KernelHandle*> clients;
    std::vector<Follower*> followers;
    const std::vector<std::uint8_t>* bundle = nullptr;
    std::uint16_t port = 0;

    void frame(bool sleep = true) {
        if (sleep) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (listen) {
            KernelPlayerInput input{};
            input.input_seq = sequence++;
            input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
            Kernel_SubmitPlayerInput(server_kernel, 1, &input);
        }
        Kernel_Update(server_kernel, kDt);
        KernelEvent events[64];
        std::uint32_t count = 0;
        while ((count = Kernel_PollEvents(server_kernel, events, 64u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) on_event(events[i]);
        }
        on_tick();
        for (KernelHandle* client : clients) {
            Kernel_Update(client, kDt);
            while (Kernel_PollEvents(client, events, 64u) != 0u) {
            }
        }
        for (Follower* follower : followers) follower->follow();
    }

    bool until(const std::function<bool()>& done, double seconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
            frame();
            if (done()) return true;
        }
        return false;
    }

    void settle(double seconds) {
        until([] { return false; }, seconds);
    }

    Follower* join() {
        KernelConfig config = config_for(KernelMode_Client);
        KernelHandle* client = Kernel_Create(&config);
        require(client != nullptr);
        clients.push_back(client);
        KernelGameplayCatalogSyncClientConfig sync{};
        sync.struct_size = sizeof(sync);
        sync.max_bundle_size = KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE;
        sync.timeout_ms = 3000000u;  // counted in simulated time, which runs fast here
        require(Kernel_StartClientCatalogSync(client, ("127.0.0.1:" + std::to_string(port)).c_str(), &sync));
        require(until([&] {
            KernelGameplayCatalogSyncStatus status{};
            status.struct_size = sizeof(status);
            require(Kernel_GetGameplayCatalogSyncStatus(client, &status));
            return status.state == KernelGameplayCatalogSyncState_ManifestReady;
        }, 15.0));
        KernelGameplayCatalogLoadResult result{};
        require(Kernel_LoadGameplayCatalogFromMemory(
            client, bundle->data(), static_cast<std::uint32_t>(bundle->size()), kEntry, &result));
        require(Kernel_ContinueClientHandshake(client));
        auto* follower = new Follower{client, true, bundle};
        followers.push_back(follower);
        require(until([&] { return follower->state.received; }, 15.0));
        return follower;
    }

    void leave(Follower* follower) {
        followers.erase(std::find(followers.begin(), followers.end(), follower));
        clients.erase(std::find(clients.begin(), clients.end(), follower->kernel));
        GameServer_ReleaseTerrain(follower->kernel);
        Kernel_Destroy(follower->kernel);
        delete follower;
    }

    KernelServerEntityState state_of(std::uint32_t net_id) const {
        KernelServerEntityState s{};
        s.struct_size = sizeof(s);
        require(Kernel_ServerGetEntityState(server_kernel, net_id, &s));
        return s;
    }
};

std::uint32_t player_of(KernelHandle* kernel) {
    KernelLocalPlayerInfo info{};
    require(Kernel_GetLocalPlayerInfo(kernel, &info));
    return info.player_net_id;
}

void select_level(KernelHandle* from, std::uint16_t index) {
    const std::uint8_t pick[2] = {static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8)};
    require(Kernel_SendGameMessage(from, GAME_SERVER_MESSAGE_SCENE_SELECT, pick, 2u));
}

void abort_level(KernelHandle* from) {
    require(Kernel_SendGameMessage(from, GAME_SERVER_MESSAGE_SCENE_ABORT, nullptr, 0u));
}

// A server the way GameServer_CreateWithGameplayCatalogFromMemory makes one.
struct CApiServer {
    KernelHandle* kernel = nullptr;
    GameServerHandle* game = nullptr;
};

CApiServer start_c_api(World* world, bool listen, std::uint16_t port, const std::vector<std::uint8_t>& bundle) {
    CApiServer server;
    KernelConfig config = config_for(listen ? KernelMode_ListenServer : KernelMode_DedicatedServer);
    server.kernel = Kernel_Create(&config);
    require(server.kernel != nullptr);
    KernelGameplayCatalogLoadResult result{};
    server.game = GameServer_CreateWithGameplayCatalogFromMemory(
        server.kernel, bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry, &result);
    if (server.game == nullptr) std::fprintf(stderr, "load: %s\n", result.diagnostic);
    require(server.game != nullptr);
    KernelGameplayCatalogSyncServerConfig serve{};
    serve.struct_size = sizeof(serve);
    serve.bundle_bytes = bundle.data();
    serve.bundle_size = static_cast<std::uint32_t>(bundle.size());
    serve.entry_path = kEntry;
    serve.content_namespace = "scene_flow_startup_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(server.kernel, &serve, &manifest));
    require(listen ? Kernel_StartListenServer(server.kernel, port) : Kernel_StartDedicatedServer(server.kernel, port));
    world->server_kernel = server.kernel;
    world->listen = listen;
    world->port = port;
    world->bundle = &bundle;
    GameServerHandle* game = server.game;
    world->on_event = [game](const KernelEvent& event) { GameServer_HandleEvent(game, &event); };
    world->on_tick = [game] { GameServer_Tick(game, kDt); };
    return server;
}

void stop(World* world, CApiServer server) {
    while (!world->followers.empty()) {
        Follower* f = world->followers.back();
        if (f->kernel == server.kernel) {
            world->followers.pop_back();
            continue;
        }
        world->leave(f);
    }
    GameServer_Destroy(server.game);
    GameServer_ReleaseTerrain(server.kernel);
    Kernel_Destroy(server.kernel);
}

std::uint16_t level_index(const std::vector<std::uint8_t>& bundle) {
    const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), kEntry);
    const auto found = std::find_if(config.scene_templates.begin(), config.scene_templates.end(),
        [](const gs::SceneTemplateRef& r) { return r.name == kLevel; });
    require(found != config.scene_templates.end());
    return found->id;
}

void listen_host(const std::vector<std::uint8_t>& bundle, std::uint16_t index) {
    World world;
    const CApiServer server = start_c_api(&world, true, 8072, bundle);
    Follower host{server.kernel, false, &bundle};
    world.followers.push_back(&host);
    Follower* a = world.join();
    const std::uint32_t host_player = player_of(server.kernel);
    const std::uint32_t a_player = player_of(a->kernel);
    require(world.until([&] { return host.state.received && a->state.owner == host_player; }, 5.0));
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);

    // A client can list the levels from its own synced catalog.
    require(GameServer_GetSceneTemplateCount(a->kernel) == 3u);
    GameServerSceneTemplateInfo info{};
    info.struct_size = sizeof(info);
    require(GameServer_GetSceneTemplate(a->kernel, 1u, &info));
    require(info.template_id == index && std::string(info.name) == kLevel);
    require(std::string(info.display_name) == "Mission test");
    require(!GameServer_GetSceneTemplate(a->kernel, 3u, &info));

    // The state is re-sent with nothing changing.
    const int before = a->states_received;
    for (int i = 0; i < 400; ++i) world.frame();
    require(a->states_received > before);

    // A client may not choose; the host may, through the real creation path.
    select_level(a->kernel, index);
    world.settle(0.3);
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);
    select_level(server.kernel, index);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(a->loaded.count(1u) == 1u && host.loaded.count(1u) == 1u);
    std::printf("listen: host opened the level through the C API server\n");

    // A client may not abort; the host may.
    abort_level(a->kernel);
    world.settle(0.3);
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN);
    abort_level(server.kernel);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a->loaded.empty(); }, 10.0));
    require(a->state.outcome == GAME_SERVER_SCENE_OUTCOME_ABORTED);

    // Abort with someone inside: back at the camp, alive, and the next level opens.
    select_level(server.kernel, index);
    require(world.until([&] {
        return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && a->state.generation == 1u;
    }, 30.0));
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(server.kernel, a_player, &a->state.gate, &facing));
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    require(world.until([&] { return level.contains(world.state_of(a_player).position.x); }, 5.0));
    abort_level(server.kernel);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP; }, 10.0));
    world.settle(0.3);
    const KernelServerEntityState back = world.state_of(a_player);
    require(gs::scene_slot_bounds(0u).contains(back.position.x) && back.hp != 0u);
    std::printf("listen: abort brought the player inside back alive\n");
    world.followers.erase(std::find(world.followers.begin(), world.followers.end(), &host));
    stop(&world, server);
}

void dedicated(const std::vector<std::uint8_t>& bundle, std::uint16_t index) {
    World world;
    const CApiServer server = start_c_api(&world, false, 8073, bundle);
    Follower* a = world.join();
    Follower* b = world.join();
    const std::uint32_t a_player = player_of(a->kernel);
    const std::uint32_t b_player = player_of(b->kernel);
    require(world.until([&] { return a->state.owner == a_player && b->state.owner == a_player; }, 5.0));

    select_level(b->kernel, index);
    world.settle(0.3);
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);
    select_level(a->kernel, index);
    require(world.until([&] { return b->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    std::printf("dedicated: the first player opened the level; the second was refused\n");

    // The owner leaves mid-level: the next player owns it and can end it.
    world.leave(a);
    require(world.until([&] { return b->state.owner == b_player; }, 15.0));
    require(b->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN);
    abort_level(b->kernel);
    require(world.until([&] { return b->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && b->loaded.empty(); }, 10.0));
    std::printf("dedicated: ownership passed on and the new owner ended the level\n");

    // Everyone leaves mid-level: it ends with nobody there.
    select_level(b->kernel, index);
    require(world.until([&] { return b->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    world.leave(b);
    world.settle(1.0);
    Follower* c = world.join();
    require(world.until([&] { return c->state.flow == GAME_SERVER_SCENE_FLOW_CAMP; }, 5.0));
    require(c->state.slots.empty() && c->state.outcome == GAME_SERVER_SCENE_OUTCOME_ABORTED);
    require(c->state.generation == 2u);
    require(c->state.owner == player_of(c->kernel));
    std::printf("dedicated: an empty server ended the level\n");

    // A gate nobody uses closes (5 minutes of simulated time).
    select_level(c->kernel, index);
    require(world.until([&] { return c->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    for (int i = 0; i < 9200 && c->state.flow != GAME_SERVER_SCENE_FLOW_CAMP; ++i) world.frame(false);
    require(world.until([&] { return c->state.flow == GAME_SERVER_SCENE_FLOW_CAMP; }, 5.0));
    require(c->state.outcome == GAME_SERVER_SCENE_OUTCOME_ABORTED);
    std::printf("dedicated: an unused gate closed by itself\n");
    stop(&world, server);
}

void auto_level(const std::vector<std::uint8_t>& bundle_bytes) {
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(bundle_bytes);
    const auto size = static_cast<std::uint32_t>(bundle->size());
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(bundle->data(), size, kEntry);
    const std::vector<std::uint8_t> scene =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.static_collision_scene.entry_path);
    config.navigation_mesh.artifact =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.navigation_mesh.entry_path);
    World world;
    KernelConfig kernel_config = config_for(KernelMode_DedicatedServer);
    world.server_kernel = Kernel_Create(&kernel_config);
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
    serve.content_namespace = "scene_flow_startup_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(world.server_kernel, &serve, &manifest));
    world.port = 8074;
    world.bundle = &bundle_bytes;
    require(Kernel_StartDedicatedServer(world.server_kernel, world.port));
    gs::GameServer server(world.server_kernel, config);
    require(server.preload_directors());
    server.enable_scene_table(gs::SceneCatalogSource{bundle, kEntry, ""});
    server.set_auto_level(kLevel);
    world.on_event = [&](const KernelEvent& event) { server.handle_event(event); };
    world.on_tick = [&] { server.tick(kDt); };

    world.settle(0.3);
    require(server.scene_flow() == GAME_SERVER_SCENE_FLOW_CAMP);  // nobody here yet
    Follower* a = world.join();
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    abort_level(a->kernel);
    require(world.until([&] {
        return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && a->state.generation == 1u;
    }, 30.0));
    std::printf("--level: opened with the first player, and again after it ended\n");
    world.leave(a);
    Kernel_Destroy(world.server_kernel);
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const std::uint16_t index = level_index(bundle);
    listen_host(bundle, index);
    dedicated(bundle, index);
    auto_level(bundle);
    std::printf("scene_flow_startup_test passed\n");
    return 0;
}
