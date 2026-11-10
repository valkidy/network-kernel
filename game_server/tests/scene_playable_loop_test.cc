// The whole game loop, the way a player plays it
// (docs/SCENE_TABLE_PLAYABLE_LOOP_PLAN.md L6): from the real camp, walk to the
// terminal, open a generated level, walk through the gate, clear it, walk out,
// open another, lose it, and open a third. The server is made through the C API
// and players move by input only; enemies and players die to the kill floor.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
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
#include "game_server/src/terrain/terrain_recipe.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "scene_playable_loop_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr std::uint16_t kRandomLevel = 4u;
constexpr std::uint32_t kTerminalTemplate = 231u;
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

struct SlotEntry {
    std::uint32_t generation = 0;
    std::uint32_t source = 0;
    std::uint64_t hash = 0;
    std::string name;
};

struct SceneState {
    bool received = false;
    std::uint8_t flow = 0;
    std::uint32_t generation = 0;
    std::uint8_t outcome = 0;
    KernelVec3 gate{};
    std::uint32_t defeated = 0;
    std::uint32_t target = 0;
    std::map<std::uint32_t, SlotEntry> slots;
};

struct Follower {
    KernelHandle* kernel = nullptr;
    bool registers_collision = true;
    const std::vector<std::uint8_t>* bundle = nullptr;
    SceneState state;
    bool drop_next_chunk = false;
    bool simulate_failure = false;
    std::uint32_t player = 0;
    bool walking = false;
    KernelVec3 walk_to{};
    std::vector<std::uint8_t> ui_results;
    int chunks_seen = 0;
    std::chrono::steady_clock::time_point last_pump = std::chrono::steady_clock::now();
    struct Load { std::uint32_t generation = 0; bool begun = false; bool done = false; };
    std::map<std::uint32_t, Load> loads;

    void decode(const KernelGameMessage& m) {
        require(m.payload_size >= 33u);
        const std::uint8_t* p = m.payload;
        SceneState s;
        s.received = true;
        s.flow = p[0];
        s.generation = u32_at(p + 2);
        s.outcome = p[6];
        const auto f = [&](std::uint32_t at) {
            const std::uint32_t bits = u32_at(p + at);
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        };
        s.gate = KernelVec3{f(8), f(12), f(16)};
        s.defeated = static_cast<std::uint32_t>(p[28]) | (static_cast<std::uint32_t>(p[29]) << 8);
        s.target = static_cast<std::uint32_t>(p[30]) | (static_cast<std::uint32_t>(p[31]) << 8);
        std::uint32_t at = 33;
        for (std::uint32_t k = 0; k < p[32]; ++k) {
            SlotEntry e;
            e.generation = u32_at(p + at + 1);
            e.source = p[at + 7];
            e.hash = static_cast<std::uint64_t>(u32_at(p + at + 8)) |
                (static_cast<std::uint64_t>(u32_at(p + at + 12)) << 32);
            const std::uint32_t n = p[at + 16];
            e.name.assign(reinterpret_cast<const char*>(p + at + 17), n);
            s.slots[p[at]] = e;
            at += 17 + n;
        }
        require(at == m.payload_size);
        state = s;
    }

    void send(std::uint32_t type, std::uint32_t slot, std::uint32_t generation, std::uint32_t extra = 0xffu) {
        const std::uint8_t body[6] = {static_cast<std::uint8_t>(slot),
            static_cast<std::uint8_t>(generation), static_cast<std::uint8_t>(generation >> 8),
            static_cast<std::uint8_t>(generation >> 16), static_cast<std::uint8_t>(generation >> 24),
            static_cast<std::uint8_t>(extra)};
        require(Kernel_SendGameMessage(kernel, type, body, extra == 0xffu ? 5u : 6u));
    }

    void follow() {
        KernelGameMessage messages[8];
        std::uint32_t count = 0;
        while ((count = Kernel_PollGameMessages(kernel, messages, 8u)) != 0u) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (messages[i].message_type == GAME_SERVER_MESSAGE_SCENE_RECIPE_CHUNK) {
                    ++chunks_seen;
                    if (drop_next_chunk) {
                        drop_next_chunk = false;  // lost: only a re-request brings it back
                        continue;
                    }
                    require(GameServer_ClientAcceptSceneMessage(kernel, &messages[i]));
                } else if (messages[i].message_type == GAME_SERVER_MESSAGE_SCENE_STATE) {
                    decode(messages[i]);
                } else if (messages[i].message_type == GAME_SERVER_MESSAGE_OPEN_UI) {
                    require(messages[i].payload_size == 9u);
                    ui_results.push_back(messages[i].payload[8]);
                }
            }
        }
        if (!state.received) return;
        const std::uint32_t collision = registers_collision ? GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION : 0u;
        for (auto it = loads.begin(); it != loads.end();) {
            const auto listed = state.slots.find(it->first);
            if (listed == state.slots.end() || listed->second.generation != it->second.generation) {
                require(GameServer_ClientUnloadSceneSlot(kernel, it->first, it->second.begun ? collision : 0u));
                it = loads.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [slot, entry] : state.slots) {
            Load& load = loads[slot];
            load.generation = entry.generation;
            if (load.done) continue;
            if (!load.begun) {
                std::uint32_t flags = collision;
                if (entry.source == GAME_SERVER_SCENE_SOURCE_RECIPE) {
                    // Real time: frames here run far faster than the game's clock.
                    const auto now = std::chrono::steady_clock::now();
                    const float waited = std::chrono::duration<float>(now - last_pump).count();
                    last_pump = now;
                    const std::uint32_t got = GameServer_ClientPumpSceneRecipe(
                        kernel, slot, entry.generation, entry.hash, std::min(waited, 1.0f));
                    if (got == GAME_SERVER_SCENE_LOAD_PENDING) continue;
                    if (got == GAME_SERVER_SCENE_LOAD_FAILED || simulate_failure) {
                        send(GAME_SERVER_MESSAGE_SCENE_LOAD_FAILED, slot, entry.generation, 1u);
                        load.done = true;
                        continue;
                    }
                    flags |= GAME_SERVER_SCENE_LOAD_FROM_RECIPE;
                }
                require(GameServer_ClientBeginLoadSceneSlot(kernel, bundle->data(),
                    static_cast<std::uint32_t>(bundle->size()), kEntry, entry.name.c_str(), slot, entry.generation, flags));
                load.begun = true;
            }
            const std::uint32_t status = GameServer_ClientPollSceneSlotLoad(kernel, slot, entry.generation);
            if (status == GAME_SERVER_SCENE_LOAD_PENDING) continue;
            require(status == GAME_SERVER_SCENE_LOAD_READY);
            load.done = true;
            send(GAME_SERVER_MESSAGE_SCENE_READY, slot, entry.generation);
        }
    }

    bool ready(std::uint32_t slot) const {
        const auto it = loads.find(slot);
        return it != loads.end() && it->second.done && it->second.begun;
    }
};

std::uint32_t player_of(KernelHandle* kernel);

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
        // With a simulated bad network every timer must mean real time;
        // otherwise frames run far faster than the clock they report.
        static const bool real_time = std::getenv("NETWORK_KERNEL_FAKE_LAG_MS") != nullptr;
        if (sleep) std::this_thread::sleep_for(std::chrono::milliseconds(real_time ? 33 : 1));
        ++sequence;
        for (Follower* follower : followers) {
            const bool host = follower->kernel == server_kernel;
            if (!host && !follower->walking) continue;
            KernelPlayerInput input{};
            input.input_seq = sequence;
            input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
            if (follower->walking && follower->player != 0u) {
                const KernelServerEntityState at = state_of(follower->player);
                const float dx = follower->walk_to.x - at.position.x;
                const float dz = follower->walk_to.z - at.position.z;
                const float length = std::sqrt(dx * dx + dz * dz);
                if (length > 0.01f) input.move = KernelVec2{dx / length, dz / length};
            }
            Kernel_SubmitPlayerInput(follower->kernel, 1, &input);
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
        require(until([&] { return (follower->player = player_of(client)) != 0u; }, 15.0));
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
    serve.content_namespace = "scene_playable_loop_test";
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



// Under fake lag every frame is a real 33 ms, so the same game takes longer.
double wait(double seconds) {
    return std::getenv("NETWORK_KERNEL_FAKE_LAG_MS") != nullptr ? seconds * 6.0 : seconds;
}

float distance_xz(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

std::vector<KernelServerEntityState> entities(KernelHandle* kernel, std::uint16_t type) {
    std::vector<KernelServerEntityState> all(4096);
    for (auto& one : all) one.struct_size = sizeof(one);
    all.resize(Kernel_ServerQueryEntities(kernel, type, all.data(), static_cast<std::uint32_t>(all.size())));
    return all;
}

// Walks with real input until `done`, or fails the test.
void walk(World* world, Follower* who, const KernelVec3& to, const std::function<bool()>& done) {
    who->walk_to = to;
    who->walking = true;
    const bool arrived = world->until(done, wait(30.0));
    who->walking = false;
    if (!arrived) {
        const KernelServerEntityState at = world->state_of(who->player);
        std::fprintf(stderr, "stuck at (%.1f, %.1f, %.1f) walking to (%.1f, %.1f, %.1f)\n",
            at.position.x, at.position.y, at.position.z, to.x, to.y, to.z);
    }
    require(arrived);
}

void activate(KernelHandle* from, std::uint32_t player, std::uint32_t prop, std::uint64_t id) {
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.request_id = id;
    request.instigator_net_id = player;
    request.requester_peer = 1u;  // the listen host; a client fills in its own
    request.domain_action = KernelDomainAction_Activate;
    request.target_net_id = prop;
    request.requested_quantity = 1;
    require(Kernel_SubmitGameplayRequest(from, &request));
}

// Everything the level left behind is gone, for the server and for this client.
void require_level_gone(World* world, Follower* client, std::size_t actors_in_camp, std::size_t props_in_camp) {
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    require(world->until([&] { return client->loads.empty(); }, wait(10.0)));
    const auto actors = entities(world->server_kernel, KernelEntityType_Actor);
    const auto props = entities(world->server_kernel, KernelEntityType_Prop);
    for (const auto& one : actors) require(!level.contains(one.position.x));
    for (const auto& one : props) require(!level.contains(one.position.x));
    require(actors.size() == actors_in_camp);
    require(props.size() == props_in_camp);
    world->settle(0.5);
    std::vector<RenderEntityState> seen(2048);
    const std::uint32_t n = Kernel_GetRenderStates(client->kernel, seen.data(), 2048u);
    for (std::uint32_t k = 0; k < n; ++k) require(!level.contains(seen[k].position.x));
}

void run(bool listen, std::uint16_t port, const std::vector<std::uint8_t>& bundle) {
    std::printf("=== %s\n", listen ? "listen host" : "dedicated");
    World world;
    const CApiServer server = start_c_api(&world, listen, port, bundle);
    const gs::SceneBounds camp = gs::scene_slot_bounds(0u);
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};

    Follower host{server.kernel, false, &bundle};
    if (listen) {
        world.followers.push_back(&host);
        require(world.until([&] { return host.state.received; }, wait(15.0)));
        host.player = player_of(server.kernel);
    }
    Follower* owner = listen ? &host : world.join();
    Follower* guest = world.join();
    std::vector<Follower*> party{owner, guest};
    world.settle(1.0);

    const auto props = entities(server.kernel, KernelEntityType_Prop);
    const auto terminal = std::find_if(props.begin(), props.end(),
        [](const KernelServerEntityState& p) { return p.entity_template_id == kTerminalTemplate; });
    require(terminal != props.end());
    const KernelVec3 terminal_at = terminal->position;
    const std::uint32_t terminal_id = terminal->net_id;
    const std::size_t actors_in_camp = entities(server.kernel, KernelEntityType_Actor).size();
    const std::size_t props_in_camp = props.size();

    std::uint64_t request = 1u;
    const auto open_level = [&]() -> SlotEntry {
        // Over to the terminal, ask for the list, choose: the guest is not asked.
        walk(&world, owner, terminal_at, [&] {
            return distance_xz(world.state_of(owner->player).position, terminal_at) < 2.5f;
        });
        const std::size_t answers = owner->ui_results.size();
        activate(owner->kernel, owner->player, terminal_id, request++);
        require(world.until([&] { return owner->ui_results.size() == answers + 1u; }, wait(5.0)));
        require(owner->ui_results.back() == GAME_SERVER_OPEN_UI_OK);
        select_level(owner->kernel, kRandomLevel);
        require(world.until([&] {
            return guest->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && guest->ready(1u) &&
                owner->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && (listen || owner->ready(1u));
        }, wait(60.0)));
        const SlotEntry entry = guest->state.slots.at(1u);
        require(entry.source == GAME_SERVER_SCENE_SOURCE_RECIPE && entry.hash != 0u);
        // Everybody walks through the gate.
        const KernelVec3 gate = guest->state.gate;
        for (Follower* one : party) {
            walk(&world, one, gate, [&] { return level.contains(world.state_of(one->player).position.x); });
        }
        return entry;
    };

    // ---- first level: clear it and walk out.
    const SlotEntry first = open_level();
    std::printf("level 1 open, hash %016llx, party inside\n", static_cast<unsigned long long>(first.hash));
    require(guest->state.target == 20u);
    require(world.until([&] {
        for (const auto& s : entities(server.kernel, KernelEntityType_Actor)) {
            if (s.actor_type == KernelActorType_Agent && s.hp != 0u && s.position.y > -5.0f) {
                const KernelVec3 under{s.position.x, -30.0f, s.position.z};
                Kernel_ServerSetEntityTransform(server.kernel, s.net_id, &under, &facing);
            }
        }
        return guest->state.flow == GAME_SERVER_SCENE_FLOW_RETURN_OPEN;
    }, wait(90.0)));
    require(guest->state.defeated >= 20u);
    const KernelVec3 back = guest->state.gate;
    require(level.contains(back.x));
    std::printf("level 1 cleared (%u defeated), return gate open\n", guest->state.defeated);
    // Anyone the fight left dead is back up before heading out.
    require(world.until([&] {
        for (Follower* one : party) {
            if (world.state_of(one->player).hp == 0u) return false;
        }
        return true;
    }, wait(30.0)));
    for (Follower* one : party) {
        walk(&world, one, back, [&] { return camp.contains(world.state_of(one->player).position.x); });
    }
    require(world.until([&] { return guest->state.flow == GAME_SERVER_SCENE_FLOW_CAMP; }, wait(10.0)));
    require(guest->state.outcome == GAME_SERVER_SCENE_OUTCOME_COMPLETED);
    require_level_gone(&world, guest, actors_in_camp, props_in_camp);
    std::printf("level 1 done: party back in the camp, nothing left behind\n");

    // ---- second level, another map: the whole party dies out of it.
    const SlotEntry second = open_level();
    require(second.hash != first.hash);
    require(second.generation != first.generation);
    int deaths = 0;
    require(world.until([&] {
        for (Follower* one : party) {
            const KernelServerEntityState s = world.state_of(one->player);
            if (s.hp != 0u && s.position.y > -5.0f && level.contains(s.position.x)) {
                const KernelVec3 under{s.position.x, -30.0f, s.position.z};
                Kernel_ServerSetEntityTransform(server.kernel, one->player, &under, &facing);
                ++deaths;
            }
        }
        return guest->state.flow == GAME_SERVER_SCENE_FLOW_CAMP;
    }, wait(120.0)));
    std::printf("level 2 lost after %d deaths\n", deaths);
    require(guest->state.outcome == GAME_SERVER_SCENE_OUTCOME_FAILED);
    require(deaths == 12);  // 2 players x 5 revives, then both down
    world.settle(1.0);
    for (Follower* one : party) {
        const KernelServerEntityState s = world.state_of(one->player);
        require(s.hp != 0u);
        require(camp.contains(s.position.x));
    }
    require_level_gone(&world, guest, actors_in_camp, props_in_camp);

    // ---- and the camp still works: the terminal answers, another level opens.
    const SlotEntry third = open_level();
    require(third.hash != second.hash);
    require(guest->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN);
    std::printf("level 3 opens after a lost one\n");

    if (listen) world.followers.erase(std::find(world.followers.begin(), world.followers.end(), &host));
    stop(&world, server);
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_bundle();
    run(true, 8245, bundle);
    run(false, 8246, bundle);
    std::printf("scene_playable_loop_test passed\n");
    return 0;
}
