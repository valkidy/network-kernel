// A wave walking out of the thing that spawned it, driven end to end against
// the shipped catalog.
//
// The design this pins: a unit is created inside its carrier -- where the
// carrier's own collider is what a shot reaches first -- is driven to the door
// by the entry pass rather than by its AI, leaves through the door the catalog
// authored rather than in whatever direction it was facing, and is handed back
// with its template's own movement mask once it is clear.
//
// Every assertion is scoped to the net ids this nest put out. The shipped
// catalog also runs a patrol that spawns the same template, so "are there any
// gingerbread walking about" is answered yes by something else entirely.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};

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
    for (const network_example::game_server::EntityTemplateConfig& candidate :
         config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

KernelVec3 position_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state.position;
}

float horizontal_distance(const KernelVec3& from, const KernelVec3& to) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    return std::sqrt(dx * dx + dz * dz);
}

const network_example::game_server::AgentRuntimeState* find_agent(
    const network_example::game_server::AgentRuntimeManager& agents,
    std::uint32_t net_id) {
    for (const network_example::game_server::AgentRuntimeState& agent :
         agents.agents()) {
        if (agent.net_id == net_id) {
            return &agent;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::uint32_t nest_template = template_id_of(config, "gingerbread_nest");
    require(nest_template != 0u);

    // What the authoring says, read back off the loaded catalog: this nest has a
    // door, and that is what the rest of the test is about.
    const auto carrier = std::find_if(
        config.spawner_carriers.begin(),
        config.spawner_carriers.end(),
        [nest_template](
            const network_example::game_server::SpawnerCarrierConfig& candidate) {
            return candidate.entity_template_id == nest_template;
        });
    require(carrier != config.spawner_carriers.end());
    require(carrier->spawner.entry.authored);
    require(!carrier->spawner.entry.exits.empty());
    const network_example::game_server::SpawnerEntryConfig& entry =
        carrier->spawner.entry;
    const KernelVec3 authored_exit = entry.exits.front().exit;

    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 7951));

    network_example::game_server::GameServer game_server(kernel, config);
    require(game_server.preload_directors());

    // One nest, placed rather than missioned in: the mission's own placement is
    // the shipping flow test's subject, not this one's.
    const KernelVec3 nest_position{0.0f, 0.0f, 0.0f};
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = nest_template;
    create_info.position = nest_position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t nest_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &nest_net_id));

    const network_example::game_server::SpawnerDirector& spawner =
        game_server.agent_runtime_manager().spawner_director();
    std::array<KernelEvent, 256> events{};
    auto step = [&]() {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            game_server.handle_event(events[index]);
        }
    };

    for (int tick = 0; tick < 900 && spawner.spawned_unit_count() == 0u; ++tick) {
        step();
    }
    require(spawner.spawned_unit_count() > 0u);
    require(spawner.instances().size() == 1u);
    const std::vector<std::uint32_t> wave = spawner.instances().front().spawned_net_ids;
    require(wave.size() >= 2u);

    // 1. Put out inside the nest, not around it. Without this the rest proves
    //    nothing: a unit that starts in the yard has nowhere to walk out of.
    for (const std::uint32_t net_id : wave) {
        require(horizontal_distance(nest_position, position_of(kernel, net_id)) < 1.0f);
    }

    // Somebody to chase, off to one side and well within the gingerbread's
    // vision. This is what makes the walk provable: the chaser steers at a
    // player, which from here is -x, while the authored door is +z. A unit that
    // comes out of the door was driven by the entry pass and not by its AI.
    const std::uint32_t player_template = template_id_of(config, "player");
    require(player_template != 0u);
    KernelServerEntityCreateInfo player_info{};
    player_info.struct_size = sizeof(player_info);
    player_info.entity_template_id = player_template;
    player_info.position = KernelVec3{-8.0f, 0.0f, 0.0f};
    player_info.rotation = kIdentityRotation;
    std::uint32_t player_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player_info, &player_net_id));

    // 2. The runtime owns them, and it owns them one at a time: a wave leaves in
    //    single file, so the last unit out is still waiting while the first
    //    walks.
    std::uint32_t entering = 0;
    std::uint32_t holding = 0;
    for (const std::uint32_t net_id : wave) {
        const network_example::game_server::AgentRuntimeState* agent =
            find_agent(game_server.agent_runtime_manager(), net_id);
        require(agent != nullptr);
        if (agent->entry.active) {
            ++entering;
        }
        if (agent->entry.hold_ticks > 0u) {
            ++holding;
        }
    }
    require(entering == wave.size());
    require(entry.stagger_ticks == 0u || holding > 0u);
    for (const std::uint32_t net_id : wave) {
        const network_example::game_server::AgentRuntimeState* agent =
            find_agent(game_server.agent_runtime_manager(), net_id);
        const KernelVec3 at = position_of(kernel, net_id);
        std::fprintf(
            stderr,
            "  spawned net_id=%u at (%.2f, %.2f, %.2f) hold=%u remaining=%u\n",
            net_id,
            at.x, at.y, at.z,
            agent->entry.hold_ticks,
            agent->entry.remaining_ticks);
    }

    // 3. Everyone is out, within their own budget plus the queue in front of
    //    them, and a couple of ticks for the hand back. Where each one stood the
    //    moment it was released is what the rest of this asserts on: once it is
    //    the AI's it walks off towards the player, so a position read later
    //    would be a measurement of the chaser, not of the walk.
    const std::uint32_t walk_budget = entry.max_ticks +
        entry.stagger_ticks * static_cast<std::uint32_t>(wave.size()) + 10u;
    std::vector<KernelVec3> release_positions(wave.size(), KernelVec3{});
    std::vector<bool> released(wave.size(), false);
    for (std::uint32_t tick = 0; tick < walk_budget; ++tick) {
        step();
        for (std::size_t index = 0; index < wave.size(); ++index) {
            if (released[index]) {
                continue;
            }
            const network_example::game_server::AgentRuntimeState* agent =
                find_agent(game_server.agent_runtime_manager(), wave[index]);
            require(agent != nullptr);
            if (agent->entry.active) {
                continue;
            }
            released[index] = true;
            release_positions[index] = position_of(kernel, wave[index]);
        }
    }
    for (std::size_t index = 0; index < wave.size(); ++index) {
        // Handed back to the AI, which is also what says its own movement mask
        // has been put back.
        require(released[index]);
        const KernelVec3 position = release_positions[index];
        std::fprintf(
            stderr,
            "  released net_id=%u at (%.2f, %.2f, %.2f) distance=%.2f\n",
            wave[index],
            position.x, position.y, position.z,
            horizontal_distance(nest_position, position));
        // 4. At the authored door, within the tolerance the walk stops on, and
        //    nowhere along the line to the player: a chaser would have taken
        //    them towards the player, which is the opposite side of the nest.
        //    Asserted against the exit the catalog carries rather than a
        //    hardcoded axis, so re-siting the door to match a model moves this
        //    test with it.
        require(
            horizontal_distance(authored_exit, position) <
            network_example::game_server::kSpawnerEntryArrivalMeters + 0.2f);
        // 5. And therefore clear of the nest's own collider, which the catalog
        //    checks every exit against.
        require(
            horizontal_distance(nest_position, position) >
            horizontal_distance(nest_position, authored_exit) - 1.0f);
    }

    Kernel_Destroy(kernel);
    std::printf("spawner_entry_test: PASS\n");
    return 0;
}
