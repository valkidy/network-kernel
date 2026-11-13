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
#include "physics/public/physics_world.h"

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
    scene_state_wire::Gate level_gate;
    scene_state_wire::Gate return_gate;
    std::uint32_t owner = 0;
    std::uint32_t defeated = 0;
    std::uint32_t target = 0;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> slots;
};

struct Follower {
    KernelHandle* kernel = nullptr;
    bool registers_collision = true;
    const std::vector<std::uint8_t>* bundle = nullptr;
    SceneState state;
    int states_received = 0;
    std::vector<std::uint8_t> ui_results;
    std::map<std::uint32_t, std::uint32_t> loaded;

    void decode(const KernelGameMessage& m) {
        const std::uint8_t* p = m.payload;
        scene_state_wire::Header h;
        require(scene_state_wire::read_header(p, m.payload_size, &h));
        SceneState s;
        s.received = true;
        s.flow = h.flow;
        s.generation = h.generation;
        s.outcome = h.outcome;
        s.level_gate = h.level_gate;
        s.return_gate = h.return_gate;
        // Where to stand to go through whichever gate is open.
        const scene_state_wire::Gate* open = scene_state_wire::open_gate(h);
        s.gate_open = open != nullptr;
        if (open != nullptr) s.gate = scene_state_wire::step_in(*open);
        s.owner = h.owner;
        s.defeated = h.defeated;
        s.target = h.target;
        std::uint32_t at = scene_state_wire::kSlotsAt;
        for (std::uint32_t k = 0; k < h.slot_count; ++k) {
            const std::uint32_t name_length = p[at + 16];
            s.slots[p[at]] = {u32_at(p + at + 1), std::string(reinterpret_cast<const char*>(p + at + 17), name_length)};
            at += 17 + name_length;
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
                if (messages[i].message_type == GAME_SERVER_MESSAGE_OPEN_UI) {
                    require(messages[i].payload_size == 9u);
                    require(u32_at(messages[i].payload) == GAME_SERVER_UI_LEVEL_SELECT);
                    ui_results.push_back(messages[i].payload[8]);
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
    const char* entry = kEntry;

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
            client, bundle->data(), static_cast<std::uint32_t>(bundle->size()), entry, &result));
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

void activate(KernelHandle* from, std::uint32_t peer, std::uint32_t player, std::uint32_t prop, std::uint64_t id) {
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = peer;
    request.request_id = id;
    request.instigator_net_id = player;
    request.domain_action = KernelDomainAction_Activate;
    request.target_net_id = prop;
    request.requested_quantity = 1;
    require(Kernel_SubmitGameplayRequest(from, &request));
}

// A server the way GameServer_CreateWithGameplayCatalogFromMemory makes one.
struct CApiServer {
    KernelHandle* kernel = nullptr;
    GameServerHandle* game = nullptr;
};

CApiServer start_c_api(
    World* world, bool listen, std::uint16_t port, const std::vector<std::uint8_t>& bundle, const char* entry = kEntry) {
    CApiServer server;
    KernelConfig config = config_for(listen ? KernelMode_ListenServer : KernelMode_DedicatedServer);
    server.kernel = Kernel_Create(&config);
    require(server.kernel != nullptr);
    KernelGameplayCatalogLoadResult result{};
    server.game = GameServer_CreateWithGameplayCatalogFromMemory(
        server.kernel, bundle.data(), static_cast<std::uint32_t>(bundle.size()), entry, &result);
    if (server.game == nullptr) std::fprintf(stderr, "load: %s\n", result.diagnostic);
    require(server.game != nullptr);
    KernelGameplayCatalogSyncServerConfig serve{};
    serve.struct_size = sizeof(serve);
    serve.bundle_bytes = bundle.data();
    serve.bundle_size = static_cast<std::uint32_t>(bundle.size());
    serve.entry_path = entry;
    serve.content_namespace = "scene_flow_startup_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(server.kernel, &serve, &manifest));
    world->entry = entry;
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
    const CApiServer server = start_c_api(&world, true, 8232, bundle);
    Follower host{server.kernel, false, &bundle};
    world.followers.push_back(&host);
    Follower* a = world.join();
    const std::uint32_t host_player = player_of(server.kernel);
    const std::uint32_t a_player = player_of(a->kernel);
    require(world.until([&] { return host.state.received && a->state.owner == host_player; }, 5.0));
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);

    // A client can list the levels from its own synced catalog.
    require(GameServer_GetSceneTemplateCount(a->kernel) == 7u);
    GameServerSceneTemplateInfo info{};
    info.struct_size = sizeof(info);
    require(GameServer_GetSceneTemplate(a->kernel, 1u, &info));
    require(info.template_id == index && std::string(info.name) == kLevel);
    require(std::string(info.display_name) == "Mission test");
    require(!GameServer_GetSceneTemplate(a->kernel, 7u, &info));

    // The camp (0) and a test map (100) are scene templates but not levels:
    // neither is listed above, and asking for one opens nothing.
    for (const std::uint16_t not_a_level : {std::uint16_t{0}, std::uint16_t{100}}) {
        select_level(server.kernel, not_a_level);
        world.settle(0.5);
        require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a->state.slots.empty());
    }

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
    const CApiServer server = start_c_api(&world, false, 8233, bundle);
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

// A dedicated server built natively, as the app does, for what a test must
// read or set on the GameServer itself.
std::unique_ptr<gs::GameServer> start_native(
    World* world, std::uint16_t port, const std::vector<std::uint8_t>& bundle_bytes) {
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(bundle_bytes);
    const auto size = static_cast<std::uint32_t>(bundle->size());
    gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(bundle->data(), size, kEntry);
    const std::vector<std::uint8_t> scene =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.static_collision_scene.entry_path);
    config.navigation_mesh.artifact =
        gs::load_gameplay_bundle_entry_bytes(bundle->data(), size, config.navigation_mesh.entry_path);
    KernelConfig kernel_config = config_for(KernelMode_DedicatedServer);
    world->server_kernel = Kernel_Create(&kernel_config);
    require(gs::load_kernel_gameplay_catalog(world->server_kernel, config));
    KernelStaticCollisionSceneConfig camp{};
    camp.struct_size = sizeof(camp);
    camp.artifact_bytes = scene.data();
    camp.artifact_size = static_cast<std::uint32_t>(scene.size());
    camp.scene_id = config.static_collision_scene.scene_id;
    camp.collider_id = config.static_collision_scene.collider_id;
    camp.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(world->server_kernel, &camp));
    KernelGameplayCatalogSyncServerConfig serve{};
    serve.struct_size = sizeof(serve);
    serve.bundle_bytes = bundle->data();
    serve.bundle_size = size;
    serve.entry_path = kEntry;
    serve.content_namespace = "scene_flow_startup_test";
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(world->server_kernel, &serve, &manifest));
    world->port = port;
    world->bundle = &bundle_bytes;
    require(Kernel_StartDedicatedServer(world->server_kernel, world->port));
    auto server = std::make_unique<gs::GameServer>(world->server_kernel, config);
    require(server->preload_directors());
    server->enable_scene_table(gs::SceneCatalogSource{bundle, kEntry, ""});
    gs::GameServer* raw = server.get();
    world->on_event = [raw](const KernelEvent& event) { raw->handle_event(event); };
    world->on_tick = [raw] { raw->tick(kDt); };
    return server;
}

void auto_level(const std::vector<std::uint8_t>& bundle_bytes) {
    World world;
    std::unique_ptr<gs::GameServer> server = start_native(&world, 8234, bundle_bytes);
    server->set_auto_level(kLevel);
    world.settle(0.3);
    require(server->scene_flow() == GAME_SERVER_SCENE_FLOW_CAMP);  // nobody here yet
    Follower* a = world.join();
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    abort_level(a->kernel);
    require(world.until([&] {
        return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && a->state.generation == 1u;
    }, 30.0));
    std::printf("--level: opened with the first player, and again after it ended\n");
    world.leave(a);
    server.reset();
    Kernel_Destroy(world.server_kernel);
}

// Enemy actors now in the level.
std::vector<std::uint32_t> enemies_in(World& world, const gs::SceneBounds& bounds) {
    std::vector<KernelServerEntityState> states(4096);
    for (auto& s : states) s.struct_size = sizeof(s);
    const std::uint32_t n = Kernel_ServerQueryEntities(
        world.server_kernel, KernelEntityType_Actor, states.data(), static_cast<std::uint32_t>(states.size()));
    std::vector<std::uint32_t> out;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (states[k].actor_type == KernelActorType_Agent && states[k].hp != 0u && bounds.contains(states[k].position.x)) {
            out.push_back(states[k].net_id);
        }
    }
    return out;
}

void objective_and_revives(const std::vector<std::uint8_t>& bundle_bytes) {
    World world;
    std::unique_ptr<gs::GameServer> server = start_native(&world, 8236, bundle_bytes);
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    const auto walk_in = [&](Follower* who) {
        const std::uint32_t player = player_of(who->kernel);
        require(world.until([&] { return who->loaded.count(1u) == 1u; }, 15.0));
        world.settle(0.3);  // its SCENE_READY reaches the server
        require(Kernel_ServerSetEntityTransform(world.server_kernel, player, &who->state.gate, &facing));
        require(world.until([&] { return level.contains(world.state_of(player).position.x); }, 5.0));
    };

    Follower* a = world.join();
    Follower* b = world.join();
    require(server->request_level("level_composed_patrol"));
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(world.until([&] { return b->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 5.0));
    require(a->state.target == 20u && a->state.defeated == 0u);
    walk_in(a);
    walk_in(b);
    require(server->remaining_revives() == 10);  // 2 players x 5

    // Late joiners add 5 each, up to 20 granted in all.
    Follower* c = world.join();
    require(world.until([&] { return server->remaining_revives() == 15; }, 2.0));
    Follower* d = world.join();
    require(world.until([&] { return server->remaining_revives() == 20; }, 2.0));
    Follower* e = world.join();
    world.settle(0.5);
    require(server->remaining_revives() == 20);
    std::printf("revives: 2 players 10, late joiners 15 then 20, a fifth joiner still 20\n");

    // The cap is on what was granted: spending some and re-joining adds nothing.
    const std::uint32_t a_player = player_of(a->kernel);
    const KernelVec3 at = world.state_of(a_player).position;
    const KernelVec3 below{at.x, -30.0f, at.z};
    require(Kernel_ServerSetEntityTransform(world.server_kernel, a_player, &below, &facing));
    require(world.until([&] { return server->remaining_revives() == 19; }, 5.0));
    world.leave(e);
    world.settle(0.5);
    e = world.join();
    world.settle(0.5);
    require(server->remaining_revives() == 19);
    std::printf("revives: spent one, left and re-joined, still 19\n");

    // Retired enemies do not count; deaths do.
    require(world.until([&] { return !enemies_in(world, level).empty(); }, 15.0));
    const std::vector<std::uint32_t> first = enemies_in(world, level);
    require(Kernel_ServerDestroyEntity(world.server_kernel, first[0], KernelDespawnReason_Retired));
    world.settle(0.3);
    require(server->level_defeated() == 0u);
    require(world.until([&] {
        for (const std::uint32_t net : enemies_in(world, level)) {
            const KernelServerEntityState s = world.state_of(net);
            if (s.position.y < -5.0f) continue;
            const KernelVec3 under{s.position.x, -30.0f, s.position.z};
            Kernel_ServerSetEntityTransform(world.server_kernel, net, &under, &facing);
        }
        return server->scene_flow() == GAME_SERVER_SCENE_FLOW_RETURN_OPEN;
    }, 60.0));
    require(server->scene_outcome() == GAME_SERVER_SCENE_OUTCOME_COMPLETED);
    require(server->level_defeated() >= 20u);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_RETURN_OPEN; }, 5.0));
    require(a->state.defeated >= 20u);
    std::printf("objective: %u enemies defeated, return gate open\n", a->state.defeated);
    (void)c;
    (void)d;
    while (!world.followers.empty()) world.leave(world.followers.back());
    server.reset();
    Kernel_Destroy(world.server_kernel);
}

// A level out of revives says nothing about the camp a newcomer arrives in.
void joiner_with_level_out_of_revives(const std::vector<std::uint8_t>& bundle_bytes) {
    World world;
    std::unique_ptr<gs::GameServer> server = start_native(&world, 8237, bundle_bytes);
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    Follower* a = world.join();
    Follower* b = world.join();
    require(server->request_level(kLevel));  // one fixed revive
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(world.until([&] { return b->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 5.0));
    const std::uint32_t a_player = player_of(a->kernel);
    require(Kernel_ServerSetEntityTransform(world.server_kernel, a_player, &a->state.gate, &facing));
    require(world.until([&] { return level.contains(world.state_of(a_player).position.x); }, 5.0));
    require(server->remaining_revives() == 1);
    const KernelVec3 at = world.state_of(a_player).position;
    const KernelVec3 below{at.x, -30.0f, at.z};
    require(Kernel_ServerSetEntityTransform(world.server_kernel, a_player, &below, &facing));
    require(world.until([&] { return server->remaining_revives() == 0; }, 5.0));
    Follower* c = world.join();
    world.settle(0.3);
    require(world.state_of(player_of(c->kernel)).hp != 0u);
    std::printf("revives: a newcomer arrives alive though the level is out of revives\n");
    while (!world.followers.empty()) world.leave(world.followers.back());
    server.reset();
    Kernel_Destroy(world.server_kernel);
}

// The real camp: its level terminal, activated the way a player would.
void camp_level_terminal(const std::vector<std::uint8_t>& bundle, std::uint16_t index) {
    World world;
    const CApiServer server = start_c_api(&world, true, 8244, bundle, "gameplay_catalog.yaml");
    Follower host{server.kernel, false, &bundle};
    world.followers.push_back(&host);
    Follower* a = world.join();
    const std::uint32_t host_player = player_of(server.kernel);
    const std::uint32_t a_player = player_of(a->kernel);
    world.settle(0.5);

    // The terminal is there, standing on the ground; nothing hostile is.
    std::vector<KernelServerEntityState> props(512);
    for (auto& p : props) p.struct_size = sizeof(p);
    props.resize(Kernel_ServerQueryEntities(server.kernel, KernelEntityType_Prop, props.data(), 512u));
    const auto terminal = std::find_if(props.begin(), props.end(),
        [](const KernelServerEntityState& p) { return p.entity_template_id == 231u; });
    require(terminal != props.end());
    // The camp's own ground, whatever its exported name.
    const gs::GameServerGameplayConfig camp = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), "gameplay_catalog.yaml");
    const std::vector<std::uint8_t> artifact = gs::load_gameplay_bundle_entry_bytes(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), camp.static_collision_scene.entry_path);
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::string error;
    require(network_example::physics::static_scene_triangles(artifact, &positions, &indices, &error));
    float ground = -1000.0f;
    const float px = terminal->position.x;
    const float pz = terminal->position.z;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const float* a0 = &positions[indices[t] * 3u];
        const float* b0 = &positions[indices[t + 1] * 3u];
        const float* c0 = &positions[indices[t + 2] * 3u];
        const float d = (b0[2] - c0[2]) * (a0[0] - c0[0]) + (c0[0] - b0[0]) * (a0[2] - c0[2]);
        if (std::fabs(d) < 1e-6f) continue;
        const float u = ((b0[2] - c0[2]) * (px - c0[0]) + (c0[0] - b0[0]) * (pz - c0[2])) / d;
        const float v = ((c0[2] - a0[2]) * (px - c0[0]) + (a0[0] - c0[0]) * (pz - c0[2])) / d;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
        ground = std::max(ground, u * a0[1] + v * b0[1] + (1.0f - u - v) * c0[1]);
    }
    std::printf("terminal at (%.1f, %.2f, %.1f), ground %.2f\n", px, terminal->position.y, pz, ground);
    require(std::fabs(terminal->position.y - ground) < 0.3f);
    std::vector<KernelServerEntityState> actors(512);
    for (auto& p : actors) p.struct_size = sizeof(p);
    actors.resize(Kernel_ServerQueryEntities(server.kernel, KernelEntityType_Actor, actors.data(), 512u));
    for (const auto& actor : actors) require(actor.actor_type != KernelActorType_Agent);

    const std::uint32_t prop = terminal->net_id;
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    const KernelVec3 beside{px + 1.5f, terminal->position.y + 1.0f, pz};
    require(Kernel_ServerSetEntityTransform(server.kernel, a_player, &beside, &facing));
    require(Kernel_ServerSetEntityTransform(server.kernel, host_player, &beside, &facing));
    world.settle(0.5);

    // A guest asks and is told no; the host asks and may choose. Nobody went inside.
    activate(a->kernel, 0u, a_player, prop, 1u);
    require(world.until([&] { return !a->ui_results.empty(); }, 5.0));
    require(a->ui_results.back() == GAME_SERVER_OPEN_UI_NOT_OWNER);
    require(host.ui_results.empty());
    activate(server.kernel, 1u, host_player, prop, 2u);
    require(world.until([&] { return !host.ui_results.empty(); }, 5.0));
    require(host.ui_results.back() == GAME_SERVER_OPEN_UI_OK);
    world.settle(0.5);  // A shelter request lands a tick after the event.
    KernelLocalShelterState shelter{};
    shelter.struct_size = sizeof(shelter);
    require(Kernel_GetLocalShelterState(server.kernel, &shelter));
    require(shelter.shelter_net_id == 0u);

    // Choosing from it opens the level; while it runs the list is unavailable.
    select_level(server.kernel, index);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    activate(server.kernel, 1u, host_player, prop, 3u);
    require(world.until([&] { return host.ui_results.size() == 2u; }, 5.0));
    require(host.ui_results.back() == GAME_SERVER_OPEN_UI_BUSY);
    std::printf("camp: the terminal answered guest, host and busy; nobody went inside\n");
    world.followers.erase(std::find(world.followers.begin(), world.followers.end(), &host));
    stop(&world, server);
}

// A level template as the start scene (docs/START_SCENE_PLAN.md Q1): its map,
// game_rule and props run, but none of the level flow -- no objective, no
// return gate, nothing to come back to.
void level_as_start_scene(const std::vector<std::uint8_t>& bundle) {
    World world;
    const CApiServer server = start_c_api(&world, true, 8250, bundle, "gameplay_catalog.scene_3.yaml");
    Follower host{server.kernel, false, &bundle};
    world.followers.push_back(&host);
    Follower* a = world.join();
    const auto agents = [&] {
        std::vector<KernelServerEntityState> all(512);
        for (auto& one : all) one.struct_size = sizeof(one);
        all.resize(Kernel_ServerQueryEntities(server.kernel, KernelEntityType_Actor, all.data(), 512u));
        std::vector<KernelServerEntityState> out;
        for (const auto& one : all) {
            if (one.actor_type == KernelActorType_Agent && one.hp != 0u && one.position.y > -5.0f) out.push_back(one);
        }
        return out;
    };
    // Its game_rule puts enemies out, in slot 0.
    require(world.until([&] { return !agents().empty(); }, 60.0));
    for (const auto& one : agents()) require(gs::scene_slot_bounds(0u).contains(one.position.x));
    // Kill well past its objective: the flow stays in the camp state with no target and no gate.
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    int killed = 0;
    world.until([&] {
        for (const auto& one : agents()) {
            const KernelVec3 under{one.position.x, -30.0f, one.position.z};
            if (Kernel_ServerSetEntityTransform(server.kernel, one.net_id, &under, &facing)) ++killed;
        }
        return killed >= 25;
    }, 60.0);
    world.settle(1.0);
    std::printf("level as start scene: %d enemies killed, flow %u, target %u, gate %d\n",
        killed, a->state.flow, a->state.target, a->state.gate_open ? 1 : 0);
    require(killed >= 1);
    require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);
    require(a->state.target == 0u && !a->state.gate_open && a->state.slots.empty());
    world.followers.erase(std::find(world.followers.begin(), world.followers.end(), &host));
    stop(&world, server);
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    const std::uint16_t index = level_index(bundle);
    listen_host(bundle, index);
    dedicated(bundle, index);
    auto_level(bundle);
    objective_and_revives(bundle);
    joiner_with_level_out_of_revives(bundle);
    camp_level_terminal(bundle, index);
    level_as_start_scene(bundle);
    std::printf("scene_flow_startup_test passed\n");
    return 0;
}
