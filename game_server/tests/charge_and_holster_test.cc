// P5, design D21: the charge trigger mode (K7) and the holstered auto-reload
// (K8), on a dedicated server with the shipped catalog -- the meteor staff's
// cast turned into a charge in a temporary copy, since no shipped weapon
// charges yet.
//
// K7: hold to charge, release to cast. A release before commit_offset_ticks
// cancels and spends nothing; holding past it does not cast by itself, the
// release does; changing weapon mid-charge cancels.
// K8: the weapon in hand follows the input's selection. One put away with an
// unfull magazine comes back reloaded -- full, one reserve spent -- if it was
// away for its reload action's time, and unchanged if it was not. A full one
// spends nothing.

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
constexpr std::uint8_t kRifle = 0;
constexpr std::uint8_t kMeteorStaff = 13;
constexpr int kChargeTicks = 20;
// meteor_staff_reload's commit_offset_ticks.
constexpr int kStaffReloadTicks = 60;

std::filesystem::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace / "game_server" /
        "gameplay_catalog";
}

// The shipped catalog with meteor_staff_cast made a charge.
gs::GameServerGameplayConfig catalog_with_charged_staff() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const std::filesystem::path root = std::filesystem::path(tmp) / "catalog_charge";
    std::filesystem::remove_all(root);
    std::filesystem::copy(catalog_root(), root, std::filesystem::copy_options::recursive);
    std::ofstream(root / "action_templates" / "meteor_staff_cast.yaml", std::ios::trunc)
        << "id: 4119\n"
           "name: meteor_staff_cast\n"
           "trigger_mode: charge\n"
           "flags: [cancel_on_death, cancel_on_weapon_change]\n"
           "ammo_cost_per_commit: 1\n"
           "commit_offset_ticks: " << kChargeTicks << "\n"
           "commit_interval_ticks: 30\n"
           "max_commit_count: 1\n"
           "recovery_ticks: 10\n"
           "hold_input_timeout_ticks: 6\n";
    return gs::load_gameplay_config_from_catalog_file(
        (root / "gameplay_catalog.yaml").string());
}

std::vector<std::uint8_t> read_ground_scene() {
    const std::filesystem::path path =
        catalog_root() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void send(std::uint8_t weapon, std::uint32_t action, bool start, bool held) {
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        // Down at the ground ahead: the staff's meteor is a targeted strike,
        // which needs somewhere to land or casts nothing.
        input.aim_dir = KernelVec3{1.0f, -0.4f, 0.0f};
        input.selected_weapon = weapon;
        if (action != 0u) {
            if (start) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input =
                KernelActionInput{action, static_cast<std::uint8_t>(held ? 1u : 0u), 0u, 0u};
        }
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        Kernel_Update(kernel, kTickSeconds);
    }

    // Holding `weapon` with nothing pressed.
    void idle(std::uint8_t weapon, int ticks) {
        for (int index = 0; index < ticks; ++index) send(weapon, 0u, false, false);
    }

    // Presses fire with `weapon` and holds for `ticks`; releases unless
    // `release` is false. Returns the action instance.
    std::uint32_t charge(std::uint8_t weapon, int ticks, bool release = true) {
        const std::uint32_t action = 7000u + next_action++;
        for (int index = 0; index < ticks; ++index) {
            send(weapon, action, index == 0, true);
        }
        if (release) send(weapon, action, false, false);
        return action;
    }

    KernelServerEntityState me() const { return entity_state(kernel, player); }
};

std::uint16_t staff_ammo(const KernelServerEntityState& state) {
    for (std::uint32_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == kMeteorStaff) return state.ammo[slot];
    }
    require(false);
    return 0;
}

std::uint16_t staff_reserve(const KernelServerEntityState& state) {
    for (std::uint32_t slot = 0; slot < state.weapon_slot_count; ++slot) {
        if (state.weapon_ids[slot] == kMeteorStaff) return state.reserve_magazines[slot];
    }
    require(false);
    return 0;
}

std::uint32_t held_weapon(const KernelServerEntityState& state) {
    return state.weapon_ids[state.active_weapon_slot];
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = catalog_with_charged_staff();
    const std::vector<std::uint8_t> scene = read_ground_scene();
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 1024;
    kernel_config.max_render_states = 64;
    Arena arena;
    arena.kernel = Kernel_Create(&kernel_config);
    require(arena.kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(arena.kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena.kernel, &scene_config));
    require(Kernel_StartDedicatedServer(arena.kernel, 8064));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    for (const std::uint8_t weapon : {kMeteorStaff, std::uint8_t{14}, std::uint8_t{15}, kRifle}) {
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[weapon];
        require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    }
    arena.idle(kMeteorStaff, 30);
    require(held_weapon(arena.me()) == kMeteorStaff);
    const std::uint16_t full = staff_ammo(arena.me());
    const std::uint16_t reserve = staff_reserve(arena.me());
    require(full == 3u);

    // K7. Released early: cancelled, nothing spent.
    arena.charge(kMeteorStaff, kChargeTicks / 2);
    arena.idle(kMeteorStaff, 15);
    require(staff_ammo(arena.me()) == full);

    // Held well past the charge time: still nothing until the release...
    arena.charge(kMeteorStaff, kChargeTicks + 10, false);
    require(staff_ammo(arena.me()) == full);
    // ...which casts. (Reusing the instance: the same press, let go.)
    arena.send(kMeteorStaff, 7000u + arena.next_action - 1u, false, false);
    require(staff_ammo(arena.me()) == full - 1u);
    arena.idle(kMeteorStaff, 15);

    // A weapon change mid-charge cancels it.
    {
        const std::uint32_t action = arena.charge(kMeteorStaff, kChargeTicks + 2, false);
        arena.send(kRifle, action, false, true);
        arena.send(kRifle, action, false, false);
        require(staff_ammo(arena.me()) == full - 1u);
        arena.idle(kMeteorStaff, 15);
        require(staff_ammo(arena.me()) == full - 1u);
    }

    // K8. The weapon in hand follows the selection.
    arena.idle(kRifle, 2);
    require(held_weapon(arena.me()) == kRifle);
    // Away for less than the reload: back as it went.
    arena.idle(kRifle, kStaffReloadTicks / 2);
    arena.idle(kMeteorStaff, 2);
    require(held_weapon(arena.me()) == kMeteorStaff);
    require(staff_ammo(arena.me()) == full - 1u);
    require(staff_reserve(arena.me()) == reserve);
    // Away for the whole reload: back full, one reserve spent.
    arena.idle(kRifle, kStaffReloadTicks + 2);
    arena.idle(kMeteorStaff, 2);
    require(staff_ammo(arena.me()) == full);
    require(staff_reserve(arena.me()) == reserve - 1u);
    // Away full: nothing to do, nothing spent.
    arena.idle(kRifle, kStaffReloadTicks + 2);
    arena.idle(kMeteorStaff, 2);
    require(staff_ammo(arena.me()) == full);
    require(staff_reserve(arena.me()) == reserve - 1u);

    std::puts("charge_and_holster_test passed");
    return 0;
}
