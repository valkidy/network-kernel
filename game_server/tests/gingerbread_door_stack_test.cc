// Gingerbread standing at their nest's door, stacked, sank without moving.
//
// Seen in play (G0, docs/REMOTE_PRESENTATION_NEXT_IMPLEMENTATION_PLAN.md §3.1):
// several gingerbread on the same point at y = 0, flagged grounded, their
// vertical velocity growing by 9.81 m/s for every second they stood there --
// -128 m/s on the client, where it is quantised; -327 m/s on the server.
//
// A nest walks each unit out of its door on a terrain-only mask, so a wave
// arrives on the exit point together. Released there with actors blocking each
// other, the stacked capsules report one another as ground with a horizontal
// normal. Off walkable ground the movement solver adds gravity to the vertical
// velocity it had, Jolt hands that velocity back unchanged although nothing
// moved, and so it grew without bound. The solver now keeps only the vertical
// speed the move made good.
//
// The whole shipped game, headless, as the dedicated server runs it: a listen
// server whose host joins so the mission puts its nests out, actor blocking on,
// and the host standing out of every gingerbread's sight so they stay at their
// doors.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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
// Long enough for four waves out of every nest, and for a stuck one to have
// sunk well past anything a knockback or a fall could produce.
constexpr int kRunTicks = 30 * 60;

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

std::uint32_t template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::uint32_t gingerbread = template_id_of(config, "gingerbread");
    require(gingerbread != 0u);
    const std::vector<std::uint8_t> scene = read_ground_scene();

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
    kernel_config.max_render_states = 512;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    // As the dedicated server runs: actors block each other. Without it the
    // stacked units pass through one another and nothing sinks.
    KernelSessionRulesConfig session_rules{};
    session_rules.struct_size = sizeof(session_rules);
    session_rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(kernel, &session_rules));
    require(network_example::game_server::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartListenServer(kernel, 7952));
    network_example::game_server::GameServer game_server(kernel, config);
    // What the host apps do before their first frame: without it no director
    // runs, so no mission and no nests.
    require(game_server.preload_directors());

    std::array<KernelEvent, 4096> events{};
    std::vector<KernelServerEntityState> states(1024);
    std::map<std::uint32_t, float> lowest_vertical_velocity;
    std::size_t most_on_one_spot = 0;

    for (int tick = 0; tick < kRunTicks; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t event_count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            game_server.handle_event(events[index]);
        }
        game_server.tick(kTickSeconds);

        // The query writes nothing into an element whose struct_size is unset.
        for (KernelServerEntityState& state : states) {
            state.struct_size = sizeof(KernelServerEntityState);
        }
        const std::uint32_t count = std::min<std::uint32_t>(
            Kernel_ServerQueryEntities(
                kernel, 0u, states.data(), static_cast<std::uint32_t>(states.size())),
            static_cast<std::uint32_t>(states.size()));
        std::vector<KernelVec3> standing;
        for (std::uint32_t index = 0; index < count; ++index) {
            const KernelServerEntityState& state = states[index];
            if (state.actor_template_id != gingerbread) continue;
            auto [it, fresh] = lowest_vertical_velocity.try_emplace(state.net_id, 0.0f);
            it->second = std::min(it->second, state.velocity.y);
            standing.push_back(state.position);
        }
        // How many stand on one spot: the stack under test.
        for (const KernelVec3& spot : standing) {
            const std::size_t together = static_cast<std::size_t>(std::count_if(
                standing.begin(), standing.end(), [&spot](const KernelVec3& other) {
                    return std::abs(other.x - spot.x) < 0.01f &&
                        std::abs(other.z - spot.z) < 0.01f;
                }));
            most_on_one_spot = std::max(most_on_one_spot, together);
        }
    }
    Kernel_Destroy(kernel);

    float lowest = 0.0f;
    for (const auto& [net_id, velocity_y] : lowest_vertical_velocity) {
        lowest = std::min(lowest, velocity_y);
    }
    std::printf(
        "%zu gingerbread; at most %zu on one spot; lowest vertical velocity %.2f m/s\n",
        lowest_vertical_velocity.size(),
        most_on_one_spot,
        lowest);
    // The case is there: waves of several, and some of them stacked.
    require(lowest_vertical_velocity.size() >= 12u);
    require(most_on_one_spot >= 2u);
    // Nothing knocks them about or drops them here, so nothing falls: a unit
    // settling onto the floor is a fraction of a metre per second at most.
    for (const auto& [net_id, velocity_y] : lowest_vertical_velocity) {
        if (velocity_y < -2.0f) {
            std::fprintf(stderr, "gingerbread #%u reached %.2f m/s\n", net_id, velocity_y);
        }
        require(velocity_y >= -2.0f);
    }
    return 0;
}
