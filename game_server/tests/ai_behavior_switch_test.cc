// C2 of docs/AI_BEHAVIOR_PLAN.md: AgentRuntimeManager::set_behavior switches
// an agent at runtime (B3), checked against what the behavior needs, and lets
// go of whatever it was fighting at once (B10). Guard and patrol each read
// their own leash.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/agent_behavior.h"
#include "game_server/src/agent_runtime_manager.h"
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

gs::AgentControllerSpec spec_of(
    gs::AgentControllerRole role,
    std::vector<KernelVec3> route = {}) {
    gs::AgentControllerSpec spec;
    spec.role = role;
    spec.route = std::move(route);
    return spec;
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// Each leashed behavior reads its own leash; a hunter is leashed as a guard.
void each_behavior_reads_its_own_leash() {
    gs::AgentChaserConfig config;
    config.guard.leash_meters = 3.0f;
    config.patrol.leash_meters = 7.0f;
    config.squad.leash_meters = 11.0f;
    const auto leash = [&](gs::AgentControllerRole role) {
        const gs::AgentBehavior* behavior = gs::agent_behavior_for_role(role);
        require(behavior != nullptr);
        return behavior->leash_meters(config);
    };
    require(leash(gs::AgentControllerRole::kGuard) == 3.0f);
    require(leash(gs::AgentControllerRole::kPatrol) == 7.0f);
    require(leash(gs::AgentControllerRole::kSquad) == 11.0f);
    require(leash(gs::AgentControllerRole::kHunter) == 3.0f);
    require(gs::agent_behavior_for_role(gs::AgentControllerRole::kSentry) == nullptr);
    require(gs::agent_behavior_for_role(gs::AgentControllerRole::kNone) == nullptr);
}

}  // namespace

int main() {
    each_behavior_reads_its_own_leash();

    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.preload_director_template_ids.clear();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    config.spawner_carriers.clear();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, 8493));

    gs::AgentRuntimeManager manager(kernel, config);
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            manager.tick(kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    };

    const std::uint32_t gingerbread =
        create(kernel, template_id_of(config, "gingerbread"), {0.0f, 0.0f, 0.0f});
    const std::uint32_t sentry =
        create(kernel, template_id_of(config, "sentry_grunt"), {60.0f, 0.0f, 60.0f});
    run(30);
    require(agent_of(manager, gingerbread).controller_role == gs::AgentControllerRole::kGuard);
    require(agent_of(manager, sentry).controller_role == gs::AgentControllerRole::kSentry);

    // Refused, each for its own reason, and nothing changes.
    using Result = gs::BehaviorSwitchResult;
    using Role = gs::AgentControllerRole;
    const std::vector<KernelVec3> route{{0.0f, 0.0f, 0.0f}, {15.0f, 0.0f, 0.0f}};
    require(manager.set_behavior(999999u, spec_of(Role::kGuard)) == Result::kAgentMissing);
    require(manager.set_behavior(gingerbread, spec_of(Role::kNone)) == Result::kInvalidData);
    require(manager.set_behavior(gingerbread, spec_of(Role::kSentry)) == Result::kNotAllowed);
    require(manager.set_behavior(gingerbread, spec_of(Role::kSquad)) == Result::kNotAllowed);
    require(manager.set_behavior(gingerbread, spec_of(Role::kPatrol)) == Result::kInvalidData);
    require(manager.set_behavior(gingerbread, spec_of(Role::kPatrol, {route[0]})) ==
            Result::kInvalidData);
    require(manager.set_behavior(gingerbread, spec_of(Role::kGuard, route)) ==
            Result::kInvalidData);
    require(manager.set_behavior(gingerbread, spec_of(Role::kHunter, route)) ==
            Result::kInvalidData);
    require(manager.set_behavior(sentry, spec_of(Role::kGuard)) == Result::kNotAllowed);
    require(agent_of(manager, gingerbread).controller_role == Role::kGuard);
    require(agent_of(manager, sentry).controller_role == Role::kSentry);

    // Guard -> patrol: it takes the route and walks it.
    require(manager.set_behavior(gingerbread, spec_of(Role::kPatrol, route)) == Result::kAccepted);
    require(agent_of(manager, gingerbread).controller_role == Role::kPatrol);
    require(agent_of(manager, gingerbread).route_patrol.path.size() >= 2u);
    const KernelVec3 patrol_from = agent_of(manager, gingerbread).position;
    run(60);
    const float patrolled = horizontal(agent_of(manager, gingerbread).position, patrol_from);
    std::printf("[switch] patrol walked %.2f m in 2 s\n", patrolled);
    require(patrolled > 2.0f);

    // Patrol -> guard: the route is gone and it stands its ground.
    require(manager.set_behavior(gingerbread, spec_of(Role::kGuard)) == Result::kAccepted);
    require(agent_of(manager, gingerbread).controller_role == Role::kGuard);
    require(agent_of(manager, gingerbread).route_patrol.path.empty());
    run(5);
    const KernelVec3 guard_from = agent_of(manager, gingerbread).position;
    run(60);
    const float guarded = horizontal(agent_of(manager, gingerbread).position, guard_from);
    std::printf("[switch] guard moved %.2f m in 2 s\n", guarded);
    require(guarded < 0.5f);

    // Engaged, then switched: it lets go at once (B10).
    const KernelVec3 at = agent_of(manager, gingerbread).position;
    for (const KernelVec2 side : {KernelVec2{5.0f, 0.0f}, KernelVec2{-5.0f, 0.0f},
                                  KernelVec2{0.0f, 5.0f}, KernelVec2{0.0f, -5.0f}}) {
        (void)create_player(kernel, config, KernelVec3{at.x + side.x, at.y + 0.5f, at.z + side.y});
    }
    bool engaged = false;
    for (int tick = 0; tick < 120 && !engaged; ++tick) {
        run(1);
        engaged = gs::is_engaged(agent_of(manager, gingerbread).sentry.state) &&
            agent_of(manager, gingerbread).sentry.target != 0u;
    }
    std::printf("[switch] engaged=%d\n", engaged ? 1 : 0);
    require(engaged);
    require(manager.set_behavior(gingerbread, spec_of(Role::kPatrol, route)) == Result::kAccepted);
    require(agent_of(manager, gingerbread).sentry.target == 0u);
    require(!agent_of(manager, gingerbread).sentry.has_attention_point);
    require(agent_of(manager, gingerbread).sentry.state == gs::AgentSentryState::kIdle);
    require(agent_of(manager, gingerbread).controller_role == Role::kPatrol);

    // Into a hunter, with its template's tuning; out of it again.
    require(manager.set_behavior(gingerbread, spec_of(Role::kHunter)) == Result::kAccepted);
    require(agent_of(manager, gingerbread).controller_role == Role::kHunter);
    require(agent_of(manager, gingerbread).route_patrol.path.empty());
    run(5);
    require(manager.set_behavior(gingerbread, spec_of(Role::kGuard)) == Result::kAccepted);
    require(agent_of(manager, gingerbread).hunt_target == 0u);
    require(agent_of(manager, gingerbread).seek_path.empty());

    Kernel_Destroy(kernel);
    std::printf("ai_behavior_switch_test passed\n");
    return 0;
}
