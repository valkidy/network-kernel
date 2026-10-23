// P3b of docs/AI_CONTROLLER_PLAN.md: an agent hit by something it cannot see
// reacts (D6, D8). A gingerbread faces -x as agents do until they turn; an
// allied beam sentry 10 m behind it, on the player side, sees it and burns it.
// The gingerbread must stay idle until the first hit, alert with its
// attention on the sentry the tick after, then turn, see it and go for it.

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
    require(Kernel_StartDedicatedServer(kernel, 7919));
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

    const std::uint32_t gingerbread =
        create(kernel, template_id_of(config, "gingerbread"), {0.0f, 0.0f, 0.0f});
    const KernelVec3 behind{10.0f, 0.0f, 0.0f};
    const std::uint32_t sentry =
        create(kernel, template_id_of(config, "allied_beam_sentry"), behind);

    bool hit = false;
    int ticks_to_hit = 0;
    for (; ticks_to_hit < 600 && !hit; ++ticks_to_hit) {
        step();
        const gs::AgentRuntimeState* agent = agent_of(game_server, gingerbread);
        require(agent != nullptr);
        for (const KernelDamageRecord& record : query(kernel)) {
            if (record.victim_net_id == gingerbread) {
                require(record.attacker_net_id == sentry);
                hit = true;
            }
        }
        // Control: until it is hit it has not noticed the sentry behind it.
        if (!hit) require(agent->sentry.state == gs::AgentSentryState::kIdle);
    }
    require(hit);

    // The next tick reads that hit: a short alert, attention on the sentry.
    step();
    const gs::AgentRuntimeState* agent = agent_of(game_server, gingerbread);
    require(agent != nullptr);
    std::printf("[reaction] hit after %d ticks; state %d by_damage %d attention (%.1f, %.1f)\n",
                ticks_to_hit, static_cast<int>(agent->sentry.state),
                agent->sentry.alerted_by_damage ? 1 : 0, agent->sentry.attention_point.x,
                agent->sentry.attention_point.z);
    require(agent->sentry.state == gs::AgentSentryState::kAlert);
    require(agent->sentry.alerted_by_damage);
    require(std::hypot(agent->sentry.attention_point.x - behind.x,
                       agent->sentry.attention_point.z - behind.z) < 2.0f);

    // It turns, sees it, and -- after the short alert, not the 90-tick one --
    // goes for it.
    int ticks_to_pursue = 0;
    for (; ticks_to_pursue < 90; ++ticks_to_pursue) {
        step();
        agent = agent_of(game_server, gingerbread);
        if (agent == nullptr || gs::is_pursuing(agent->sentry.state)) break;
    }
    require(agent != nullptr);
    std::printf("[reaction] pursuing after %d more ticks, target %u (sentry %u)\n",
                ticks_to_pursue, agent->sentry.target, sentry);
    require(gs::is_pursuing(agent->sentry.state));
    require(agent->sentry.target == sentry);
    require(ticks_to_pursue < 60);

    Kernel_Destroy(kernel);
    std::printf("damage_reaction_test passed\n");
    return 0;
}
