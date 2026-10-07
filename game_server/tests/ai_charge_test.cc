// An AI attacking with a charge weapon (D21) holds it through the charge and
// lets go, which is when it casts. Before, the executor only ever sent
// held = 1, so a charge never went off: the control below runs that old
// behaviour (charge_ticks 0) and shows no cast in two seconds of attacking.
//
// An agent that gives up mid-charge is not left stuck in it: the kernel's
// input-free weapons pass every tick times the charge out, and the next
// intent goes through.
//
// A dedicated server with the shipped catalog: a chaser grunt given the
// meteor staff (whose cast charges for 20 ticks), aiming at a player on the
// ground 6 m away, driven straight through ActorIntentExecutor tick by tick.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "game_server/src/actor_intent_executor.h"
#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint8_t kMeteorStaff = 13;

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

std::vector<std::uint8_t> read_ground_scene() {
    const std::filesystem::path path =
        catalog_root() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

KernelServerEntityState state_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

std::uint32_t spawn(
    KernelHandle* kernel, std::uint32_t template_id, std::uint16_t actor_type,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = actor_type;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

// Attacks for up to `ticks` ticks with `charge_ticks`; returns the tick the
// staff's charge was spent on, or -1.
// With `abandon_after` > 0 the agent stops attacking after that many ticks,
// mid-charge, and asks to reload instead while the player keeps sending input
// every tick -- so the tick is never input-free. Returns the tick the reload
// starts on, or -1 if the abandoned charge leaves the agent stuck.
int attack(
    const gs::GameServerGameplayConfig& config, std::uint32_t charge_ticks,
    std::uint16_t port, int ticks, int abandon_after = 0) {
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 1024;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
    require(Kernel_StartDedicatedServer(kernel, port));

    const gs::ActorTemplateConfig* grunt = nullptr;
    for (const gs::ActorTemplateConfig& candidate : config.actor_templates) {
        if (candidate.name == "chaser_grunt") grunt = &candidate;
    }
    require(grunt != nullptr);
    const std::uint32_t agent = spawn(
        kernel, grunt->actor_template_id, gs::kActorTypeAgent, KernelVec3{0.0f, 1.0f, 0.0f});
    const std::uint32_t player = spawn(
        kernel, config.player.actor_template_id, gs::kActorTypePlayer,
        KernelVec3{6.0f, 1.0f, 0.0f});
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(kernel, player, &combat));
    // The grunt with the staff.
    KernelWeaponMechanicsDefinition staff = config.weapons.definitions[kMeteorStaff];
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, agent, &staff));
    combat.hp = state_of(kernel, agent).hp;
    combat.max_hp = state_of(kernel, agent).max_hp;
    combat.weapon_slot_count = 1;
    combat.active_weapon_slot = 0;
    for (std::size_t slot = 0; slot < KERNEL_MAX_WEAPON_SLOTS; ++slot) {
        combat.weapon_ids[slot] = 0;
        combat.ammo[slot] = 0;
        combat.reserve_magazines[slot] = 0;
    }
    combat.weapon_ids[0] = kMeteorStaff;
    // One short of full when abandoning, so a reload has something to do.
    combat.ammo[0] = abandon_after > 0 ? 2 : 3;
    combat.reserve_magazines[0] = 6;
    require(Kernel_ServerSetEntityCombatState(kernel, agent, &combat));
    for (int index = 0; index < 20; ++index) Kernel_Update(kernel, kTickSeconds);

    gs::ActorIntentExecutorConfig executor_config;
    executor_config.weapon_id = kMeteorStaff;
    executor_config.charge_ticks = charge_ticks;
    const gs::ActorIntentExecutor executor(executor_config);
    gs::AgentRuntimeState runtime;
    runtime.net_id = agent;
    network_example::ai::ScopedIntent intent;
    intent.scope = network_example::ai::IntentScope::kActor;
    intent.type = "AttackTarget";
    intent.subject = agent;
    if (abandon_after > 0) {
        network_example::ai::ScopedIntent reload = intent;
        reload.type = "Reload";
        std::uint32_t player_seq = 1;
        int reload_tick = -1;
        for (int tick = 1; tick <= ticks && reload_tick < 0; ++tick) {
            gs::SentryPerceptionSnapshot perception;
            perception.self_state = state_of(kernel, agent);
            perception.has_self_state = true;
            perception.has_visible_target = true;
            perception.target_id = player;
            perception.has_target_position = true;
            perception.target_position = state_of(kernel, player).position;
            executor.execute(
                kernel, &runtime, tick <= abandon_after ? intent : reload, perception);
            KernelPlayerInput idle{};
            idle.input_seq = player_seq++;
            idle.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
            require(Kernel_ServerSubmitEntityInput(kernel, player, &idle));
            Kernel_Update(kernel, kTickSeconds);
            const KernelServerEntityState after = state_of(kernel, agent);
            require(after.ammo[0] == 2u);  // the abandoned charge never cast
            if (tick > abandon_after && after.is_reloading != 0u) reload_tick = tick;
        }
        Kernel_Destroy(kernel);
        return reload_tick;
    }
    int cast_tick = -1;
    for (int tick = 1; tick <= ticks && cast_tick < 0; ++tick) {
        gs::SentryPerceptionSnapshot perception;
        perception.self_state = state_of(kernel, agent);
        perception.has_self_state = true;
        perception.has_visible_target = true;
        perception.target_id = player;
        perception.has_target_position = true;
        perception.target_position = state_of(kernel, player).position;
        executor.execute(kernel, &runtime, intent, perception);
        Kernel_Update(kernel, kTickSeconds);
        if (state_of(kernel, agent).ammo[0] < 3u) cast_tick = tick;
    }
    Kernel_Destroy(kernel);
    return cast_tick;
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_catalog_file(
        (catalog_root() / "gameplay_catalog.yaml").string());
    // The charge time an agent's executor is given comes from the weapon's
    // fire action.
    const std::uint32_t charge = gs::weapon_charge_ticks(config, kMeteorStaff);
    require(charge == 20u);
    require(gs::weapon_charge_ticks(config, 0) == 0u);  // the rifle presses

    // Before: held forever, never cast.
    const int never = attack(config, 0u, 8065, 60);
    std::printf("held forever: cast at %d\n", never);
    require(never < 0);

    // Now: held through the charge, released, cast.
    const int cast = attack(config, charge, 8066, 60);
    std::printf("charge %u: cast at tick %d\n", charge, cast);
    require(cast > static_cast<int>(charge));
    require(cast <= static_cast<int>(charge) + 4);

    // Abandoned mid-charge while the world keeps receiving input: the charge
    // times out (hold_input_timeout_ticks 6) on the kernel's own every-tick
    // pass, and the agent's next intent goes through.
    const int reloaded = attack(config, charge, 8067, 60, 5);
    std::printf("abandoned after 5: reload starts at tick %d\n", reloaded);
    require(reloaded > 5);
    require(reloaded <= 5 + 6 + 3);

    std::puts("ai_charge_test passed");
    return 0;
}
