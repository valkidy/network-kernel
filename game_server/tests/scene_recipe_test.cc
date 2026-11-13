// Opening a level by recipe (docs/SCENE_TABLE_PLAYABLE_LOOP_PLAN.md L3): the
// server generates the map from seed -1, clients fetch the recipe in chunks
// and compose it on a worker, over real connections through the C API, as a
// listen host and as a dedicated server.

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
        std::fprintf(stderr, "scene_recipe_test failed at line %d: %s\n", line, text);
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
    std::map<std::uint32_t, SlotEntry> slots;
};

struct Follower {
    KernelHandle* kernel = nullptr;
    bool registers_collision = true;
    const std::vector<std::uint8_t>* bundle = nullptr;
    SceneState state;
    bool drop_next_chunk = false;
    bool simulate_failure = false;
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
    serve.content_namespace = "scene_recipe_test";
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


constexpr std::uint16_t kRandomLevel = 4u;
constexpr std::uint16_t kBundleLevel = 1u;

// Chunks: split, shuffled, repeated, replaced by a newer transfer.
void assembler() {
    gs::terrain::StoredRecipe stored;
    stored.heights_hash = 0x1234u;
    stored.recipe.cells_x = stored.recipe.cells_z = 720;
    stored.recipe.placements.resize(140);
    const std::vector<std::uint8_t> bytes = gs::terrain::write_recipe(stored);
    require(bytes.size() == 96u + 24u * 140u);
    const auto chunks = gs::encode_scene_recipe_chunks(1u, 7u, 3u, bytes);
    require(chunks.size() == 8u);
    for (const auto& chunk : chunks) require(chunk.size() <= KERNEL_MAX_GAME_MESSAGE_BYTES);

    gs::SceneRecipeAssembler a;
    require(a.accept(chunks[5]) && a.accept(chunks[5]) && a.accept(chunks[0]));
    for (std::size_t k = chunks.size(); k-- > 0u;) {
        require(a.complete(1u, 7u) == nullptr || k == 0u);
        require(a.accept(chunks[k]));
    }
    require(a.complete(1u, 7u) != nullptr && *a.complete(1u, 7u) == bytes);
    require(a.complete(1u, 8u) == nullptr);

    // A newer transfer replaces a partial one; a late chunk of the old is ignored.
    gs::SceneRecipeAssembler b;
    const auto old_chunks = gs::encode_scene_recipe_chunks(1u, 7u, 3u, bytes);
    const auto new_chunks = gs::encode_scene_recipe_chunks(1u, 7u, 4u, bytes);
    require(b.accept(old_chunks[0]));
    for (const auto& chunk : new_chunks) require(b.accept(chunk));
    require(b.accept(old_chunks[1]));
    require(b.complete(1u, 7u) != nullptr && *b.complete(1u, 7u) == bytes);

    const std::vector<std::uint8_t> short_body(5, 0u);
    require(!b.accept(short_body));
    std::printf("assembler: %zu B in %zu chunks, shuffled, repeated, replaced\n", bytes.size(), chunks.size());
}

std::uint64_t run(bool listen, std::uint16_t port, const std::vector<std::uint8_t>& bundle) {
    std::printf("=== %s\n", listen ? "listen host" : "dedicated");
    World world;
    const CApiServer server = start_c_api(&world, listen, port, bundle);
    Follower host{server.kernel, false, &bundle};
    if (listen) world.followers.push_back(&host);
    Follower* a = world.join();
    a->drop_next_chunk = true;
    KernelHandle* owner = listen ? server.kernel : a->kernel;
    const std::uint32_t a_player = player_of(a->kernel);

    // A level with a cooked recipe still loads from the bundle.
    select_level(owner, kBundleLevel);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 30.0));
    require(a->state.slots.at(1u).source == GAME_SERVER_SCENE_SOURCE_BUNDLE && a->state.slots.at(1u).hash == 0u);
    require(a->chunks_seen == 0);
    abort_level(owner);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a->loads.empty(); }, 10.0));

    // Random level: the first chunk is lost, and fetching recovers it.
    select_level(owner, kRandomLevel);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 60.0));
    const SlotEntry first = a->state.slots.at(1u);
    require(first.source == GAME_SERVER_SCENE_SOURCE_RECIPE && first.hash != 0u);
    require(world.until([&] { return a->ready(1u); }, 30.0));  // after its re-request
    require(a->chunks_seen >= 2);  // the lost one and its replacement
    GameServerTerrainInfo info{};
    info.struct_size = sizeof(info);
    require(GameServer_GetSceneTerrainInfo(a->kernel, 1u, &info));
    require(info.heights_hash == first.hash);
    // In slot 1, not the camp's slot 0: the map's corner sits half a map west
    // of the slot centre, and slots are far wider than any map.
    require(info.origin_x > gs::kSceneSlotSpacing * 0.5f && info.origin_x < gs::kSceneSlotSpacing);
    if (listen) {
        GameServerTerrainInfo host_info{};
        host_info.struct_size = sizeof(host_info);
        require(GameServer_GetSceneTerrainInfo(server.kernel, 1u, &host_info));
        require(host_info.heights_hash == first.hash);
    }
    std::printf("random level: hash %016llx, recovered after a lost chunk (%d chunks seen)\n",
        static_cast<unsigned long long>(first.hash), a->chunks_seen);

    // Standing on the generated ground, on both sides.
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    const gs::SceneBounds level = gs::scene_slot_bounds(1u);
    require(Kernel_ServerSetEntityTransform(server.kernel, a_player, &a->state.gate, &facing));
    require(world.until([&] { return level.contains(world.state_of(a_player).position.x); }, 5.0));
    require(world.until([&] {
        const KernelServerEntityState now = world.state_of(a_player);
        return now.position.y < 30.0f && std::fabs(now.velocity.y) < 0.1f;
    }, 20.0));
    world.settle(1.0);
    const KernelServerEntityState on_server = world.state_of(a_player);
    std::vector<RenderEntityState> states(2048);
    const std::uint32_t n = Kernel_GetRenderStates(a->kernel, states.data(), 2048u);
    float seen = NAN;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (states[k].net_id == a_player) seen = states[k].position.y;
    }
    std::printf("on generated ground: server y %.2f, client view y %.2f\n", on_server.position.y, seen);
    require(on_server.position.y > -5.0f);
    require(!std::isnan(seen) && std::fabs(seen - on_server.position.y) < 0.5f);

    // A late joiner fetches the same recipe.
    Follower* b = world.join();
    require(world.until([&] { return b->ready(1u); }, 15.0));
    GameServerTerrainInfo b_info{};
    b_info.struct_size = sizeof(b_info);
    require(GameServer_GetSceneTerrainInfo(b->kernel, 1u, &b_info));
    require(b_info.heights_hash == first.hash);
    std::printf("late joiner composed the same map\n");
    world.leave(b);

    // Opened again: another map.
    abort_level(owner);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a->loads.empty(); }, 10.0));
    select_level(owner, kRandomLevel);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN; }, 60.0));
    const SlotEntry second = a->state.slots.at(1u);
    require(second.hash != first.hash);
    std::printf("reopened: hash %016llx\n", static_cast<unsigned long long>(second.hash));
    abort_level(owner);
    require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP && a->loads.empty(); }, 10.0));

    if (!listen && std::getenv("NETWORK_KERNEL_FAKE_LAG_MS") == nullptr) {
        // A whole generated level: nests go up on arrival, the objective is
        // met, and the return gate stands where a player can walk through it.
        select_level(owner, kRandomLevel);
        require(world.until([&] {
            return a->state.flow == GAME_SERVER_SCENE_FLOW_GATE_OPEN && a->ready(1u);
        }, 60.0));
        require(Kernel_ServerSetEntityTransform(server.kernel, a_player, &a->state.gate, &facing));
        require(world.until([&] { return level.contains(world.state_of(a_player).position.x); }, 5.0));
        const auto entities_of = [&](std::uint16_t type) {
            std::vector<KernelServerEntityState> all(4096);
            for (auto& one : all) one.struct_size = sizeof(one);
            all.resize(Kernel_ServerQueryEntities(server.kernel, type, all.data(), static_cast<std::uint32_t>(all.size())));
            std::vector<KernelServerEntityState> inside;
            for (const auto& one : all) {
                if (level.contains(one.position.x)) inside.push_back(one);
            }
            return inside;
        };
        require(world.until([&] { return !entities_of(KernelEntityType_Prop).empty(); }, 10.0));
        std::printf("generated level: %zu nests up\n", entities_of(KernelEntityType_Prop).size());
        require(world.until([&] {
            for (const auto& s : entities_of(KernelEntityType_Actor)) {
                if (s.actor_type == KernelActorType_Agent && s.hp != 0u && s.position.y > -5.0f) {
                    const KernelVec3 under{s.position.x, -30.0f, s.position.z};
                    Kernel_ServerSetEntityTransform(server.kernel, s.net_id, &under, &facing);
                }
            }
            return a->state.flow == GAME_SERVER_SCENE_FLOW_RETURN_OPEN;
        }, 60.0));
        require(level.contains(a->state.gate.x));
        require(Kernel_ServerSetEntityTransform(server.kernel, a_player, &a->state.gate, &facing));
        require(world.until([&] { return a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP; }, 10.0));
        require(a->state.outcome == GAME_SERVER_SCENE_OUTCOME_COMPLETED);
        require(world.until([&] { return a->loads.empty(); }, 5.0));
        std::printf("generated level: cleared and back in the camp\n");
    }

    if (!listen) {
        // The only player cannot build it: the party does not wait out the timeout.
        a->simulate_failure = true;
        select_level(owner, kRandomLevel);
        require(world.until([&] { return a->state.outcome == GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED; }, 60.0));
        require(a->state.flow == GAME_SERVER_SCENE_FLOW_CAMP);
        std::printf("a client that cannot build the level ended it without the wait\n");
    }
    if (listen) world.followers.erase(std::find(world.followers.begin(), world.followers.end(), &host));
    stop(&world, server);
    return first.hash;
}

}  // namespace

int main() {
    assembler();
    const std::vector<std::uint8_t> bundle = read_bundle();
    run(true, 8242, bundle);
    run(false, 8243, bundle);
    std::printf("scene_recipe_test passed\n");
    return 0;
}
