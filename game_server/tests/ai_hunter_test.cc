// P6a of docs/AI_CONTROLLER_PLAN.md: a hunter (D10) goes after a target it
// has never seen. The gingerbread, made a hunter for this test, starts 60 m
// from a player -- far outside its 26 m cone, and off to its side. It has to
// seek towards the player from the shared broad-phase table, see it, and
// pursue it. Then, told to stand as a garrison when its target is gone, it
// has to do that once the player is removed from the world.

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
#include "game_server/src/agent_runtime.h"
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

std::vector<KernelDamageRecord> query(KernelHandle* kernel) {
    const std::uint32_t count = Kernel_ServerQueryDamageRecords(kernel, nullptr, 0u);
    std::vector<KernelDamageRecord> records(count);
    for (KernelDamageRecord& record : records) record.struct_size = sizeof(record);
    require(Kernel_ServerQueryDamageRecords(kernel, records.data(), count) == count);
    return records;
}



const gs::AgentRuntimeState* agent_of(gs::GameServer& server, std::uint32_t net_id) {
    for (const gs::AgentRuntimeState& agent : server.agent_runtime_manager().agents()) {
        if (agent.net_id == net_id) return &agent;
    }
    return nullptr;
}

}  // namespace

int main() {
    gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    config.game_rules.clear();
    config.world_rule_spawns.clear();
    config.patrols.clear();
    // The gingerbread as a hunter that turns garrison once its target is gone.
    const std::uint32_t gingerbread_id = template_id_of(config, "gingerbread");
    for (auto* templates : {&config.actor_templates, &config.entity_templates}) {
        for (gs::ActorTemplateConfig& actor : *templates) {
            if (actor.actor_template_id != gingerbread_id) continue;
            actor.default_controller_role = gs::AgentControllerRole::kHunter;
            actor.hunter.target_source = gs::HunterTargetSource::kNearestHostile;
            actor.hunter.on_target_lost = gs::HunterOnTargetLost::kGarrison;
        }
    }
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
    require(Kernel_StartDedicatedServer(kernel, 7925));
    gs::GameServer game_server(kernel, config);
    std::array<KernelEvent, 256> events{};
    const auto step = [&]() {
        game_server.tick(kTickSeconds);
        Kernel_Update(kernel, kTickSeconds);
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            game_server.handle_event(events[index]);
        }
    };
    const auto position_of = [&](std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(kernel, net_id, &state));
        return state.position;
    };

    const std::uint32_t hunter = create(kernel, gingerbread_id, {0.0f, 0.0f, 0.0f});
    const std::uint32_t player =
        create(kernel, template_id_of(config, "player"), {0.0f, 0.0f, 60.0f});
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(kernel, player, &vision));
    KernelServerEntityState player_state{};
    player_state.struct_size = sizeof(player_state);
    require(Kernel_ServerGetEntityState(kernel, player, &player_state));
    const std::uint16_t full_hp = player_state.max_hp;

    // Seeking: it heads for the player it cannot see.
    for (int tick = 0; tick < 30; ++tick) step();
    const gs::AgentRuntimeState* agent = agent_of(game_server, hunter);
    require(agent != nullptr);
    require(agent->controller_role == gs::AgentControllerRole::kHunter);
    require(agent->hunt_target == player);
    require(agent->sentry.state == gs::AgentSentryState::kSeek);
    const float start_gap = std::hypot(position_of(hunter).x - 0.0f, position_of(hunter).z - 60.0f);

    int seen_at = -1;
    for (int tick = 0; tick < 900 && seen_at < 0; ++tick) {
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        step();
        agent = agent_of(game_server, hunter);
        require(agent != nullptr);
        if (agent->sentry.state != gs::AgentSentryState::kSeek) seen_at = tick;
    }
    const KernelVec3 found_at = position_of(hunter);
    const float gap = std::hypot(found_at.x - 0.0f, found_at.z - 60.0f);
    std::printf("[hunter] seek ended after %d ticks, gap %.1f -> %.1f m, state %d\n",
                seen_at, start_gap, gap, static_cast<int>(agent->sentry.state));
    require(seen_at >= 0);
    require(gap < 26.0f);  // inside its cone's reach: it got there
    for (int tick = 0; tick < 150; ++tick) {
        require(Kernel_ServerSetEntityHealth(kernel, player, full_hp));
        step();
    }
    agent = agent_of(game_server, hunter);
    require(gs::is_pursuing(agent->sentry.state));
    require(agent->sentry.target == player);

    // Gone for good: with on_target_lost garrison, it stays where it is.
    KernelEntityLifecycleCommand destroy{};
    destroy.struct_size = sizeof(destroy);
    destroy.command_type = KernelEntityLifecycleCommandType_Destroy;
    destroy.net_id = player;
    require(Kernel_ServerEnqueueEntityLifecycle(kernel, KernelCommandSource_Test, &destroy));
    for (int tick = 0; tick < 5; ++tick) step();
    agent = agent_of(game_server, hunter);
    require(agent != nullptr);
    std::printf("[hunter] after its target was removed: role %d\n",
                static_cast<int>(agent->controller_role));
    require(agent->controller_role == gs::AgentControllerRole::kGarrison);
    require(agent->hunt_target == 0u);

    Kernel_Destroy(kernel);
    std::printf("ai_hunter_test passed\n");
    return 0;
}
