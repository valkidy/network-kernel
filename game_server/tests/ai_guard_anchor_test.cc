// docs/AI_BEHAVIOR_PLAN.md §12.11: a guard's post (anchor).
//
// A guard takes where it first goes idle as its post, and after a fight walks
// back to it along a path, fighting whatever it meets on the way as from any
// Idle. set_behavior may give it a post elsewhere; only a guard takes one.

#include <algorithm>
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

void destroy(KernelHandle* kernel, std::uint32_t net_id) {
    KernelEntityLifecycleCommand command{};
    command.struct_size = sizeof(command);
    command.command_type = KernelEntityLifecycleCommandType_Destroy;
    command.net_id = net_id;
    require(Kernel_ServerEnqueueEntityLifecycle(kernel, KernelCommandSource_Test, &command));
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// Positions are read from the kernel after each tick; the runtime's own copy
// is a tick behind.
KernelVec3 position_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state.position;
}

}  // namespace

int main() {
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
    require(Kernel_StartDedicatedServer(kernel, 8026));

    gs::AgentRuntimeManager manager(kernel, config);
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            manager.tick(kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    };

    // Its post is where it first goes idle.
    const std::uint32_t guard =
        create(kernel, template_id_of(config, "gingerbread"), {0.0f, 0.0f, 0.0f});
    run(30);
    require(agent_of(manager, guard).controller_role == gs::AgentControllerRole::kGuard);
    require(agent_of(manager, guard).has_guard_anchor);
    const KernelVec3 post = agent_of(manager, guard).guard_anchor;
    require(horizontal(post, KernelVec3{0.0f, 0.0f, 0.0f}) < 0.5f);

    // A fight draws it off; once the player is gone and forgotten it walks
    // back. Before this it stood wherever the fight ended.
    const std::uint32_t player = create_player(kernel, config, {12.0f, 0.5f, 0.0f});
    float farthest = 0.0f;
    for (int tick = 0; tick < 300 && farthest < 8.0f; ++tick) {
        run(1);
        farthest = std::max(farthest, horizontal(position_of(kernel, guard), post));
    }
    std::printf("[anchor] drawn %.1f m off its post\n", farthest);
    require(farthest >= 8.0f);
    destroy(kernel, player);
    bool home = false;
    int ticks_home = 0;
    for (; ticks_home < 600 && !home; ++ticks_home) {
        run(1);
        home = agent_of(manager, guard).sentry.state == gs::AgentSentryState::kIdle &&
            horizontal(position_of(kernel, guard), post) <= 1.0f;
    }
    std::printf("[anchor] back on its post after %d ticks\n", ticks_home);
    require(home);
    // And it stays there.
    run(60);
    require(horizontal(position_of(kernel, guard), post) <= 1.5f);

    // Posted elsewhere by set_behavior: it walks there and holds it.
    using Result = gs::BehaviorSwitchResult;
    gs::AgentControllerSpec posted;
    posted.role = gs::AgentControllerRole::kGuard;
    posted.has_anchor = true;
    posted.anchor = KernelVec3{-15.0f, 0.0f, 10.0f};
    require(manager.set_behavior(guard, posted) == Result::kAccepted);
    require(agent_of(manager, guard).guard_anchor.x == -15.0f);
    bool arrived = false;
    for (int tick = 0; tick < 300 && !arrived; ++tick) {
        run(1);
        arrived = horizontal(position_of(kernel, guard), posted.anchor) <= 1.0f;
    }
    std::printf("[anchor] reached the post it was given: %d\n", arrived ? 1 : 0);
    require(arrived);

    // A guard with no post given takes where it is when switched (B4).
    gs::AgentControllerSpec plain;
    plain.role = gs::AgentControllerRole::kGuard;
    require(manager.set_behavior(guard, plain) == Result::kAccepted);
    run(5);
    require(horizontal(agent_of(manager, guard).guard_anchor, posted.anchor) <= 1.5f);

    // Only a guard takes a post, and only a finite one.
    gs::AgentControllerSpec wrong = posted;
    wrong.role = gs::AgentControllerRole::kHunter;
    require(manager.set_behavior(guard, wrong) == Result::kInvalidData);
    wrong.role = gs::AgentControllerRole::kPatrol;
    wrong.route = {KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{5.0f, 0.0f, 0.0f}};
    require(manager.set_behavior(guard, wrong) == Result::kInvalidData);
    wrong = posted;
    wrong.anchor.x = std::nanf("");
    require(manager.set_behavior(guard, wrong) == Result::kInvalidData);

    Kernel_Destroy(kernel);
    std::printf("ai_guard_anchor_test passed\n");
    return 0;
}
