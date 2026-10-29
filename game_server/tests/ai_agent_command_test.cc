// C5 of docs/AI_BEHAVIOR_PLAN.md: a player's agent command (§8).
//
// AgentRuntimeManager::handle_agent_command, every way it refuses -- a peer
// flooding the server or still cooling down from its last obeyed order
// (§12.9), a dead requester, a missing or enemy agent, a behavior a
// command may not ask for, data the behavior cannot use, an agent switched by
// command too recently -- and what it applies when it obeys. Then the whole
// trip on a listen host: Kernel_SubmitAgentCommand, GameServer, and the
// outcome back through Kernel_PollAgentCommandOutcomes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};

std::vector<std::uint8_t> read_ground_scene() {
    const fs::path path = fs::path(std::getenv("TEST_SRCDIR")) / std::getenv("TEST_WORKSPACE") /
        "game_server" / "gameplay_catalog" / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void set_ground(KernelHandle* kernel, const gs::GameServerGameplayConfig& config,
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

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) return candidate.actor_template_id;
    }
    return 0;
}

std::uint32_t create(KernelHandle* kernel, std::uint32_t template_id, const KernelVec3& position) {
    require(template_id != 0u);
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_template_id = template_id;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0u);
    return net_id;
}

std::uint32_t create_player(
    KernelHandle* kernel,
    const gs::GameServerGameplayConfig& config,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = gs::kEntityTypeActor;
    create_info.actor_type = gs::kActorTypePlayer;
    create_info.entity_template_id = config.player.actor_template_id;
    create_info.actor_template_id = config.player.actor_template_id;
    create_info.position = position;
    create_info.rotation = kIdentityRotation;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    return net_id;
}

const gs::AgentRuntimeState& agent_of(const gs::AgentRuntimeManager& manager, std::uint32_t net_id) {
    for (const gs::AgentRuntimeState& agent : manager.agents()) {
        if (agent.net_id == net_id) return agent;
    }
    require(false);
    return manager.agents().front();
}

gs::GameServerGameplayConfig quiet_config() {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.preload_director_template_ids.clear();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    config.scene_props.clear();
    config.scene_items.clear();
    return config;
}

KernelAgentCommand order(
    std::uint32_t peer,
    std::uint32_t requester,
    std::uint32_t agent,
    std::uint8_t behavior,
    std::uint64_t request_id) {
    KernelAgentCommand command{};
    command.struct_size = sizeof(command);
    command.requester_peer = peer;
    command.requester_net_id = requester;
    command.agent_net_id = agent;
    command.behavior = behavior;
    command.request_id = request_id;
    return command;
}

// What the manager answers, and that the answer goes back where it came from.
std::uint8_t answer(gs::AgentRuntimeManager& manager, const KernelAgentCommand& command) {
    const KernelAgentCommandOutcome outcome = manager.handle_agent_command(command);
    require(outcome.struct_size == sizeof(KernelAgentCommandOutcome));
    require(outcome.requester_peer == command.requester_peer);
    require(outcome.request_id == command.request_id);
    require(outcome.agent_net_id == command.agent_net_id);
    require((outcome.status == KernelAgentCommandStatus_Accepted) ==
            (outcome.rejection_reason == KernelAgentCommandRejection_None));
    return outcome.rejection_reason;
}

void the_manager_decides() {
    gs::GameServerGameplayConfig config = quiet_config();
    // The shipped route, and the allied giant that may be sent along it.
    const gs::AgentRouteConfig* route = gs::find_agent_route(config, 1u);
    require(route != nullptr && route->name == "camp_perimeter");
    require(route->waypoints.size() == 4u);
    const std::uint32_t allied_id = template_id_of(config, "allied_gingerbread_giant");
    require(gs::find_actor_template(config, allied_id)
                ->remote_command_min_switch_interval_ticks == 10u);

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    set_ground(kernel, config, read_ground_scene());
    require(Kernel_StartDedicatedServer(kernel, 8025));
    gs::AgentRuntimeManager manager(kernel, config);
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            manager.tick(kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    };

    const std::uint32_t giant = create(kernel, allied_id, {-70.0f, 5.0f, -70.0f});
    const std::uint32_t enemy =
        create(kernel, template_id_of(config, "gingerbread"), {-20.0f, 5.0f, -70.0f});
    const std::uint32_t player = create_player(kernel, config, {-75.0f, 5.5f, -75.0f});
    run(10);
    using Behavior = KernelAgentCommandBehavior;
    using Rejection = KernelAgentCommandRejection;
    std::uint64_t request = 1;

    // Refused, each for its own reason, and nothing changes.
    constexpr std::uint32_t kPeer = 3;
    require(answer(manager, order(kPeer, 0u, giant, Behavior::KernelAgentCommandBehavior_Guard,
                                  request++)) == Rejection::KernelAgentCommandRejection_RequesterDead);
    require(answer(manager, order(kPeer, player, 999999u, Behavior::KernelAgentCommandBehavior_Guard,
                                  request++)) == Rejection::KernelAgentCommandRejection_AgentMissing);
    require(answer(manager, order(kPeer, player, enemy, Behavior::KernelAgentCommandBehavior_Guard,
                                  request++)) == Rejection::KernelAgentCommandRejection_NotPlayerSide);
    require(answer(manager, order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_None,
                                  request++)) == Rejection::KernelAgentCommandRejection_BehaviorNotAllowed);
    require(answer(manager, order(kPeer, player, giant, 9u, request++)) ==
            Rejection::KernelAgentCommandRejection_BehaviorNotAllowed);
    KernelAgentCommand bad = order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_Patrol, request++);
    bad.route_id = 77u;  // not in the catalog
    require(answer(manager, bad) == Rejection::KernelAgentCommandRejection_InvalidData);
    bad = order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_Guard, request++);
    bad.route_id = 1u;  // a guard has no route
    require(answer(manager, bad) == Rejection::KernelAgentCommandRejection_InvalidData);
    bad = order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_LockTarget, request++);
    bad.target_net_id = player;  // its own side (B9)
    require(answer(manager, bad) == Rejection::KernelAgentCommandRejection_InvalidData);
    require(agent_of(manager, giant).controller_role == gs::AgentControllerRole::kGuard);
    require(!agent_of(manager, giant).remote_switched);

    // Obeyed: sent along the catalog route.
    require(config.agent_command_peer_cooldown_ticks == 150u);
    KernelAgentCommand patrol = order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_Patrol, request++);
    patrol.route_id = 1u;
    require(answer(manager, patrol) == Rejection::KernelAgentCommandRejection_None);
    require(agent_of(manager, giant).controller_role == gs::AgentControllerRole::kPatrol);
    require(agent_of(manager, giant).route_patrol.path.size() >= 4u);

    // The player who gave it now waits out its cooldown (§12.9): anything it
    // sends is refused, valid or not, until 150 ticks have passed.
    KernelAgentCommand lock = order(kPeer, player, giant, Behavior::KernelAgentCommandBehavior_LockTarget, request++);
    lock.target_net_id = enemy;
    require(answer(manager, lock) == Rejection::KernelAgentCommandRejection_RateLimited);
    // Another player is not held by it, only by the agent's own 10 ticks
    // (B11): too soon at 9, obeyed at 10.
    constexpr std::uint32_t kOtherPeer = 5;
    KernelAgentCommand guard = order(kOtherPeer, player, giant, Behavior::KernelAgentCommandBehavior_Guard, request++);
    run(9);
    require(answer(manager, guard) == Rejection::KernelAgentCommandRejection_TooSoon);
    run(1);
    guard.request_id = request++;
    require(answer(manager, guard) == Rejection::KernelAgentCommandRejection_None);
    require(agent_of(manager, giant).controller_role == gs::AgentControllerRole::kGuard);
    // The first player: still refused at 149 ticks, obeyed at 150.
    run(139);
    lock.request_id = request++;
    // Across several one-second flood windows: the cooldown is not theirs.
    require(answer(manager, lock) == Rejection::KernelAgentCommandRejection_RateLimited);
    run(1);
    lock.request_id = request++;
    require(answer(manager, lock) == Rejection::KernelAgentCommandRejection_None);
    require(agent_of(manager, giant).controller_role == gs::AgentControllerRole::kLockTarget);
    require(agent_of(manager, giant).lock_target == enemy);
    std::printf("[command] cooldown: refused at 0 and 149 ticks, obeyed at 150; "
                "other player too soon at 9, obeyed at 10\n");

    // A refused order starts no cooldown: a third player's mistake, then its
    // corrected order straight after, is obeyed.
    constexpr std::uint32_t kCarefulPeer = 6;
    run(10);
    KernelAgentCommand wrong = order(kCarefulPeer, player, giant, Behavior::KernelAgentCommandBehavior_Patrol, request++);
    wrong.route_id = 77u;
    require(answer(manager, wrong) == Rejection::KernelAgentCommandRejection_InvalidData);
    wrong.route_id = 1u;
    wrong.request_id = request++;
    require(answer(manager, wrong) == Rejection::KernelAgentCommandRejection_None);
    require(agent_of(manager, giant).controller_role == gs::AgentControllerRole::kPatrol);

    // A peer flooding the server: past 16 in a second the rest are refused
    // whatever they say, and the next second it is heard again.
    constexpr std::uint32_t kFlooder = 4;
    std::uint32_t limited = 0;
    for (int index = 0; index < 20; ++index) {
        // Refused for a dead requester when heard, which starts no cooldown.
        if (answer(manager, order(kFlooder, 0u, giant, Behavior::KernelAgentCommandBehavior_Guard,
                                  request++)) == Rejection::KernelAgentCommandRejection_RateLimited) {
            ++limited;
        }
    }
    std::printf("[command] flood guard limited %u of 20\n", limited);
    require(limited == 4u);
    run(30);
    require(answer(manager, order(kFlooder, 0u, giant, Behavior::KernelAgentCommandBehavior_Guard,
                                  request++)) == Rejection::KernelAgentCommandRejection_RequesterDead);
    Kernel_Destroy(kernel);
}

// The host's own player orders the giant; the order and the answer go
// through the kernel and GameServer as a remote player's would.
void the_whole_trip_on_a_listen_host() {
    gs::GameServerGameplayConfig config = quiet_config();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_ListenServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    set_ground(kernel, config, read_ground_scene());
    require(Kernel_StartListenServer(kernel, 8022));
    gs::GameServer server(kernel, config);
    std::vector<KernelEvent> events(256);
    const auto step = [&]() {
        server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            server.handle_event(events[index]);
        }
    };
    const std::uint32_t giant = create(
        kernel, template_id_of(config, "allied_gingerbread_giant"), {-70.0f, 5.0f, -70.0f});
    for (int tick = 0; tick < 10; ++tick) step();
    KernelLocalPlayerInfo local{};
    require(Kernel_GetLocalPlayerInfo(kernel, &local));
    require(local.player_net_id != 0u);
    {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        const bool found = Kernel_ServerGetEntityState(kernel, local.player_net_id, &state);
        std::printf("[trip] local player %u found=%d valid=%u hp=%u flags=0x%x y=%.1f\n",
                    local.player_net_id, found ? 1 : 0, state.valid, state.hp,
                    state.visual_flags, state.position.y);
    }

    KernelAgentCommand patrol{};
    patrol.struct_size = sizeof(patrol);
    patrol.request_id = 41u;
    patrol.agent_net_id = giant;
    patrol.behavior = KernelAgentCommandBehavior_Patrol;
    patrol.route_id = 1u;
    require(Kernel_SubmitAgentCommand(kernel, &patrol));
    step();
    KernelAgentCommandOutcome outcomes[4]{};
    const std::uint32_t count = Kernel_PollAgentCommandOutcomes(kernel, outcomes, 4u);
    std::printf("[trip] outcomes %u, status %u, reason %u\n", count,
                count > 0u ? outcomes[0].status : 255u,
                count > 0u ? outcomes[0].rejection_reason : 255u);
    require(count == 1u);
    require(outcomes[0].request_id == 41u);
    require(outcomes[0].agent_net_id == giant);
    require(outcomes[0].status == KernelAgentCommandStatus_Accepted);
    require(agent_of(server.agent_runtime_manager(), giant).controller_role ==
            gs::AgentControllerRole::kPatrol);

    // And a refusal comes back the same way.
    patrol.request_id = 42u;
    patrol.route_id = 77u;
    require(Kernel_SubmitAgentCommand(kernel, &patrol));
    step();
    require(Kernel_PollAgentCommandOutcomes(kernel, outcomes, 4u) == 1u);
    require(outcomes[0].request_id == 42u);
    require(outcomes[0].status == KernelAgentCommandStatus_Rejected);
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    the_manager_decides();
    the_whole_trip_on_a_listen_host();
    std::printf("ai_agent_command_test passed\n");
    return 0;
}
