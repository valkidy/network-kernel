// A gingerbread knocked down by a shockwave stays down before it walks again.
//
// Seen in play: a unit knocked flat slid along the ground. The client plays its
// impact-falling-flat clip from the landing, but the server handed it back to
// its controller the tick it came down, and it walked off under the clip -- the
// probe measured it walking at 4.61 m/s from the tick after touchdown. Its
// template now authors `knockdown: recovery_ticks`, and a landed knockback
// holds it rooted for that long.
//
// The shipped game, headless, as the dedicated server runs it; the unit is one
// its nest put out, driven by its own AI, knocked off its feet by a shockwave
// bottle thrown at the ground in front of it.

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
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
// 28_gingerbread.yaml's knockdown recovery_ticks.
constexpr std::size_t kRecoveryTicks = 21u;

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
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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
    const auto config = network_example::game_server::default_game_server_gameplay_config();
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
    require(Kernel_StartListenServer(kernel, 7961));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());
    std::array<KernelEvent, 4096> events{};
    const auto step = [&]() {
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t n = Kernel_PollEvents(kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t i = 0; i < n; ++i) game_server.handle_event(events[i]);
        game_server.tick(kTickSeconds);
    };
    std::vector<KernelServerEntityState> states(1024);
    for (int tick = 0; tick < 30 * 20; ++tick) step();
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    const std::uint32_t player = local.player_net_id;
    // A gingerbread standing at a door: the last one found.
    for (KernelServerEntityState& st : states) st.struct_size = sizeof(KernelServerEntityState);
    const std::uint32_t count = Kernel_ServerQueryEntities(kernel, 0u, states.data(), 1024u);
    std::uint32_t unit = 0;
    KernelVec3 unit_at{};
    for (std::uint32_t i = 0; i < count; ++i) {
        if (states[i].actor_template_id == gingerbread) { unit = states[i].net_id; unit_at = states[i].position; }
    }
    require(unit != 0u);
    // The player 8 m out on -x from it, throwing at the ground 2 m in front of it.
    const KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    const KernelVec3 player_at{unit_at.x - 8.0f, 0.0f, unit_at.z};
    require(Kernel_ServerSetEntityTransform(kernel, player, &player_at, &identity));
    step();
    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, player, 4, &container));
    KernelItemInstanceId bottle = 0;
    require(Kernel_ServerCreateInventoryItem(kernel, 3008u, 1u, container, &bottle));
    KernelGameplayRequest request{};
    request.struct_size = sizeof(request);
    request.requester_peer = local.peer_id != 0u ? local.peer_id : 1u;
    request.request_id = 1;
    request.instigator_net_id = player;
    request.domain_action = KernelDomainAction_Throw;
    request.requested_quantity = 1;
    request.selected_item_instance_id = bottle;
    request.throw_direction = KernelVec3{0.97f, -0.24f, 0.0f};
    require(Kernel_ServerSubmitGameplayRequest(kernel, &request));
    std::printf("unit #%u at (%.2f, %.2f)\n", unit, unit_at.x, unit_at.z);

    struct Sample {
        KernelVec3 position;
        std::uint32_t flags;
    };
    std::vector<Sample> samples;
    for (int tick = 0; tick < 100; ++tick) {
        step();
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        if (!Kernel_ServerGetEntityState(kernel, unit, &state)) break;
        samples.push_back(Sample{state.position, state.visual_flags});
    }
    Kernel_Destroy(kernel);

    // It flew: this is a knockback that lands, not a stand.
    std::size_t landed = samples.size();
    bool airborne = false;
    for (std::size_t tick = 0; tick < samples.size(); ++tick) {
        airborne = airborne || samples[tick].position.y > 0.5f;
        if (airborne && samples[tick].position.y < 0.05f &&
            (samples[tick].flags & KERNEL_VISUAL_FLAG_GROUNDED) != 0u) {
            landed = tick;
            break;
        }
    }
    require(airborne);
    require(landed + 60u < samples.size());
    std::printf("landed at %zu\n", landed);
    const auto moved = [&](std::size_t from, std::size_t to) {
        const float dx = samples[to].position.x - samples[from].position.x;
        const float dz = samples[to].position.z - samples[from].position.z;
        return std::sqrt(dx * dx + dz * dz);
    };
    // Down for the authored recovery: not a centimetre, though its AI is still
    // steering it at the player.
    const std::size_t down = landed + 1u;
    for (std::size_t tick = down; tick < landed + kRecoveryTicks; ++tick) {
        if (moved(down, tick) >= 0.01f) {
            std::fprintf(stderr, "moved %.3f m at %zu ticks after landing\n",
                moved(down, tick), tick - landed);
        }
        require(moved(down, tick) < 0.01f);
    }
    // Then up, and walking again.
    require(moved(down, landed + 60u) > 0.5f);
    return 0;
}
