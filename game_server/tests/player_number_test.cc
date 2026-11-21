// Snapshot schema 33: the server numbers its players 1..N. Each session gets
// the lowest number no other session holds when it joins and frees it when it
// leaves, and every client sees the same number on the same player
// (RenderEntityState::player_number) -- what a client picks a player's skin
// by. A dedicated server with the shipped catalog and ground, four real
// clients over GNS; then one drops and a newcomer -- which is what a reconnect
// looks like to the server -- takes its number back.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
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

using network_example::game_server::GameServerGameplayConfig;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint16_t kPort = 8087;

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog" / "mesh_assets" /
        "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

KernelHandle* make_engine(KernelMode mode, const GameServerGameplayConfig& config) {
    KernelConfig kernel_config{};
    kernel_config.mode = mode;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 256;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(network_example::game_server::load_kernel_gameplay_catalog(kernel, config));
    return kernel;
}

void set_ground(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
}

// player net id -> number, for every player this kernel draws.
std::map<std::uint32_t, std::uint8_t> player_numbers(KernelHandle* kernel) {
    std::array<RenderEntityState, 256> states{};
    const std::uint32_t count = Kernel_GetRenderStates(
        kernel, states.data(), static_cast<std::uint32_t>(states.size()));
    std::map<std::uint32_t, std::uint8_t> numbers;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].entity_type == KernelEntityType_Actor &&
            states[index].actor_type == KernelActorType_Player) {
            numbers[states[index].net_id] = states[index].player_number;
        }
    }
    return numbers;
}

std::uint32_t own_player(KernelHandle* kernel) {
    KernelLocalPlayerInfo info{};
    return Kernel_GetLocalPlayerInfo(kernel, &info) && info.has_welcome != 0u
        ? info.player_net_id
        : 0u;
}

struct Session {
    const GameServerGameplayConfig* config = nullptr;
    const std::vector<std::uint8_t>* scene = nullptr;
    KernelHandle* server = nullptr;
    network_example::game_server::GameServer* game_server = nullptr;
    std::vector<KernelHandle*> clients;

    KernelHandle* join() {
        KernelHandle* client = make_engine(KernelMode_Client, *config);
        const std::string address = "127.0.0.1:" + std::to_string(kPort);
        require(Kernel_StartClient(client, address.c_str()));
        set_ground(client, *config, *scene);
        clients.push_back(client);
        return client;
    }

    void drop(KernelHandle* client) {
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (*it == client) {
                clients.erase(it);
                break;
            }
        }
        Kernel_Destroy(client);
    }

    void frame() {
        const KernelPlayerInput idle{};
        for (KernelHandle* client : clients) {
            Kernel_SubmitPlayerInput(client, 0, &idle);
            Kernel_Update(client, kTickSeconds);
            drain(client, false);
        }
        game_server->tick(kTickSeconds);
        Kernel_Update(server, kTickSeconds);
        drain(server, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void drain(KernelHandle* kernel, bool to_game_server) {
        std::array<KernelEvent, 256> events{};
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count && to_game_server; ++index) {
            game_server->handle_event(events[index]);
        }
    }

    // Steps until the server holds exactly `players` players, all numbered,
    // and every client draws exactly the server's numbers. Returns them.
    std::map<std::uint32_t, std::uint8_t> settle(std::size_t players) {
        for (int frame_index = 0; frame_index < 900; ++frame_index) {
            frame();
            const std::map<std::uint32_t, std::uint8_t> truth = player_numbers(server);
            bool agreed = truth.size() == players;
            for (const auto& entry : truth) {
                agreed = agreed && entry.second != 0u;
            }
            for (KernelHandle* client : clients) {
                agreed = agreed && own_player(client) != 0u &&
                         player_numbers(client) == truth;
            }
            if (agreed) {
                return truth;
            }
        }
        std::fprintf(stderr, "never settled on %zu numbered players\n", players);
        for (const auto& entry : player_numbers(server)) {
            std::fprintf(stderr, "  server: net %u number %u\n", entry.first, entry.second);
        }
        for (std::size_t index = 0; index < clients.size(); ++index) {
            std::fprintf(stderr, "  client %zu (own net %u):", index, own_player(clients[index]));
            for (const auto& entry : player_numbers(clients[index])) {
                std::fprintf(stderr, " net %u=%u", entry.first, entry.second);
            }
            std::fprintf(stderr, "\n");
        }
        require(false);
        return {};
    }
};

}  // namespace

int main() {
    const GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::vector<std::uint8_t> scene = read_ground_scene();

    Session session;
    session.config = &config;
    session.scene = &scene;
    session.server = make_engine(KernelMode_DedicatedServer, config);
    set_ground(session.server, config, scene);
    // No mission: nothing but players on the map.
    GameServerGameplayConfig quiet = config;
    quiet.game_rules.clear();
    network_example::game_server::GameServer game_server(session.server, quiet);
    session.game_server = &game_server;
    require(Kernel_StartDedicatedServer(session.server, kPort));

    // One at a time, so the order -- and so the numbers -- are known.
    std::vector<KernelHandle*> joined;
    for (std::size_t index = 0; index < 4u; ++index) {
        joined.push_back(session.join());
        session.settle(index + 1u);
    }
    std::map<std::uint32_t, std::uint8_t> numbers = session.settle(4u);
    for (std::size_t index = 0; index < 4u; ++index) {
        require(numbers[own_player(joined[index])] == index + 1u);
    }

    // The second player drops; the others keep their numbers.
    const std::uint32_t dropped_player = own_player(joined[1]);
    session.drop(joined[1]);
    numbers = session.settle(3u);
    require(numbers.count(dropped_player) == 0u);
    require(numbers[own_player(joined[0])] == 1u);
    require(numbers[own_player(joined[2])] == 3u);
    require(numbers[own_player(joined[3])] == 4u);

    // Whoever joins next takes the lowest free number, 2, not 5.
    KernelHandle* newcomer = session.join();
    numbers = session.settle(4u);
    require(own_player(newcomer) != dropped_player);
    require(numbers[own_player(newcomer)] == 2u);
    require(numbers[own_player(joined[0])] == 1u);
    require(numbers[own_player(joined[2])] == 3u);
    require(numbers[own_player(joined[3])] == 4u);

    for (KernelHandle* client : session.clients) {
        Kernel_Destroy(client);
    }
    Kernel_Destroy(session.server);
    std::puts("player_number_test passed");
    return 0;
}
