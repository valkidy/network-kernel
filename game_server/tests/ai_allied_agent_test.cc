// C4 of docs/AI_BEHAVIOR_PLAN.md: an agent on the player's side.
//
// The allied gingerbread giant (40) is the giant (39) with its camp, its
// bounds and bones' layer and its sweep's mask on the player's side. Its sweep
// hurts an enemy and never a player standing in reach, and the enemy's blows
// land on it. As a hunter it goes after enemies from the shared table -- not
// after players, which is what a table of players alone would send it to --
// and an enemy hunter counts it as one of the player's side.

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

void destroy(KernelHandle* kernel, std::uint32_t net_id) {
    KernelEntityLifecycleCommand command{};
    command.struct_size = sizeof(command);
    command.command_type = KernelEntityLifecycleCommandType_Destroy;
    command.net_id = net_id;
    require(Kernel_ServerEnqueueEntityLifecycle(kernel, KernelCommandSource_Test, &command));
}

std::vector<KernelDamageRecord> damage_records(KernelHandle* kernel) {
    const std::uint32_t count = Kernel_ServerQueryDamageRecords(kernel, nullptr, 0u);
    std::vector<KernelDamageRecord> records(count);
    for (KernelDamageRecord& record : records) record.struct_size = sizeof(record);
    require(Kernel_ServerQueryDamageRecords(kernel, records.data(), count) == count);
    return records;
}

const gs::AgentRuntimeState& agent_of(const gs::AgentRuntimeManager& manager, std::uint32_t net_id) {
    for (const gs::AgentRuntimeState& agent : manager.agents()) {
        if (agent.net_id == net_id) return agent;
    }
    require(false);
    return manager.agents().front();
}

float horizontal(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

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
    const std::uint32_t allied_id = template_id_of(config, "allied_gingerbread_giant");
    const std::uint32_t gingerbread_id = template_id_of(config, "gingerbread");
    require(allied_id == 40u);
    const gs::ActorTemplateConfig* allied = gs::find_actor_template(config, allied_id);
    require(allied != nullptr);
    require(allied->vision.camp == KernelAgentCamp_PlayerSide);
    require(allied->default_controller_role == gs::AgentControllerRole::kGuard);

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
    require(Kernel_StartDedicatedServer(kernel, 8494));

    gs::AgentRuntimeManager manager(kernel, config);
    const auto run = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick) {
            manager.tick(kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    };

    // --- Who hits whom ---
    // A: a player 3 m in front of the giant and an enemy gingerbread 10 m out,
    // which goes for the nearer of its hostiles -- the player -- and so is cut
    // down at the player's side, inside the sweep's 7 m and 120 degrees, which
    // catches up to four. The player is in that sweep's reach; only the mask
    // keeps it whole.
    const std::uint32_t giant = create(kernel, allied_id, {0.0f, 0.0f, 0.0f});
    const std::uint32_t player = create_player(kernel, config, {3.0f, 0.5f, 0.0f});
    const std::uint32_t enemy = create(kernel, gingerbread_id, {10.0f, 0.0f, 0.0f});
    const auto alive = [&](std::uint32_t net_id) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        return Kernel_ServerGetEntityState(kernel, net_id, &state) && state.valid != 0u &&
            state.hp > 0u;
    };
    std::uint32_t giant_hit_enemy = 0;
    std::uint32_t player_in_reach = 0;
    std::uint32_t giant_hit_player = 0;
    for (int tick = 0; tick < 600 && giant_hit_enemy == 0u; ++tick) {
        run(1);
        const bool player_alive = alive(player);
        const KernelVec3 player_at = position_of(kernel, player);
        const KernelVec3 giant_at = position_of(kernel, giant);
        for (const KernelDamageRecord& record : damage_records(kernel)) {
            if (record.attacker_net_id == giant && record.victim_net_id == enemy) {
                ++giant_hit_enemy;
                std::printf("[sides] sweep caught the enemy at %.1f %.1f; player at %.1f %.1f, "
                            "%.1f m from it and %.1f m from the giant\n",
                            record.hit_position.x, record.hit_position.z, player_at.x,
                            player_at.z, horizontal(player_at, record.hit_position),
                            horizontal(player_at, giant_at));
                if (player_alive && horizontal(player_at, record.hit_position) < 3.0f &&
                    horizontal(player_at, giant_at) < 6.5f) {
                    ++player_in_reach;
                }
            }
            if (record.attacker_net_id == giant && record.victim_net_id == player) ++giant_hit_player;
        }
    }
    std::printf("[sides] giant hit enemy %u, player in reach %u, giant hit player %u\n",
                giant_hit_enemy, player_in_reach, giant_hit_player);
    require(giant_hit_enemy > 0u);
    require(player_in_reach > 0u);
    require(giant_hit_player == 0u);
    destroy(kernel, player);
    run(3);

    // B: an enemy's blows land on it -- its bounds and bones are where the
    // enemy's player_side weapons strike. Started 2 m behind it, outside its
    // 140-degree cone, so the giant's far longer reach does not cut it down
    // before it swings (it faced the first enemy, along +x).
    const KernelVec3 giant_now = position_of(kernel, giant);
    const std::uint32_t attacker =
        create(kernel, gingerbread_id, {giant_now.x - 2.0f, giant_now.y, giant_now.z});
    std::uint32_t enemy_hit_giant = 0;
    for (int tick = 0; tick < 600 && enemy_hit_giant == 0u; ++tick) {
        run(1);
        for (const KernelDamageRecord& record : damage_records(kernel)) {
            if (record.attacker_net_id == attacker && record.victim_net_id == giant) {
                ++enemy_hit_giant;
            }
        }
    }
    std::printf("[sides] enemy hit giant %u\n", enemy_hit_giant);
    require(enemy_hit_giant > 0u);
    run(150);
    if (alive(attacker)) destroy(kernel, attacker);
    run(3);

    // --- A player-side hunter goes after enemies, not players ---
    // An enemy 70 m off, past the giant's 35 m cone, and a player 40 m off the
    // other way, nearer: a table of players would send it to the player.
    const KernelVec3 at = position_of(kernel, giant);
    const std::uint32_t far_enemy =
        create(kernel, gingerbread_id, {at.x + 70.0f, at.y, at.z});
    const std::uint32_t far_player =
        create_player(kernel, config, KernelVec3{at.x - 40.0f, at.y + 0.5f, at.z});
    run(2);
    gs::AgentControllerSpec hunter;
    hunter.role = gs::AgentControllerRole::kHunter;
    require(manager.set_behavior(giant, hunter) == gs::BehaviorSwitchResult::kAccepted);
    run(5);
    require(agent_of(manager, giant).hunt_target == far_enemy);
    const float enemy_from = horizontal(position_of(kernel, giant), position_of(kernel, far_enemy));
    run(90);
    const float enemy_to = horizontal(position_of(kernel, giant), position_of(kernel, far_enemy));
    std::printf("[table] allied hunter: %.1f m -> %.1f m from the enemy\n", enemy_from, enemy_to);
    require(agent_of(manager, giant).hunt_target == far_enemy);
    require(enemy_to < enemy_from - 2.0f);

    // --- An enemy hunter counts the giant as the player's side ---
    // The player gone, the enemy made a hunter: the giant is what it hunts.
    destroy(kernel, far_player);
    run(3);
    hunter.role = gs::AgentControllerRole::kHunter;
    require(manager.set_behavior(far_enemy, hunter) == gs::BehaviorSwitchResult::kAccepted);
    run(5);
    std::printf("[table] enemy hunter's target %u (giant %u)\n",
                agent_of(manager, far_enemy).hunt_target, giant);
    require(agent_of(manager, far_enemy).hunt_target == giant);

    Kernel_Destroy(kernel);
    std::printf("ai_allied_agent_test passed\n");
    return 0;
}
