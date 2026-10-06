// What a player with no weapon at all does when the client still sends Fire.
//
// The design ask (2026-10-07) is to let a player carry no weapon and play on
// items alone. A YAML loadout cannot say that -- the loader refuses an empty
// `weapon_slots` -- so the only way to get there today is at runtime, the way
// a future drop-your-last-weapon would: Kernel_ServerSetEntityCombatState with
// weapon_slot_count 0.
//
// The worry is weapon id 0. WeaponState answers "no active weapon" with id 0,
// and id 0 is also the shipped Rifle. A client with no weapon still sends a
// KernelPlayerInput whose selected_weapon defaults to 0. If anything resolves
// that against the catalog instead of the loadout, an unarmed player fires a
// rifle.
//
// The control fires the rifle while it is still in the loadout and shows a
// grunt 8 m down the aim losing hp -- so an unharmed grunt later means "the
// shot did not happen", not "the shot missed".
//
// Measured 2026-10-07 on main (78d9350); the requires below pin it:
//
//   An empty loadout cannot be reached. The YAML loader requires 1-4
//   weapon_slots, and Kernel_ServerSetEntityCombatState refuses
//   weapon_slot_count 0 and leaves the old loadout in place.
//
//   The id-0 worry does not bite on the fire path. With the staff as the only
//   weapon, Fire with selected_weapon 0 does nothing -- no shot, no ammo, no
//   action -- even while the rifle's mechanics are still on the entity, and
//   the same for a dropped weapon's id (14) and for Reload. The loadout, not
//   the catalog or the entity's mechanics, decides what can fire. The staff
//   still fires.
//
// Weapon mechanics are per entity: game_server sets each loadout weapon's with
// Kernel_ServerSetEntityWeaponMechanics. A player made with
// Kernel_ServerCreateEntity alone fires nothing, which is why this test sets
// them by hand.

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

std::uint32_t spawn_actor(
    KernelHandle* kernel,
    std::uint32_t template_id,
    std::uint16_t actor_type,
    std::uint32_t peer,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = actor_type;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.owner_peer = peer;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
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
    std::uint32_t grunt = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void tick(int count = 1) {
        for (int index = 0; index < count; ++index) {
            Kernel_Update(kernel, kTickSeconds);
        }
    }

    // Holds `binding` for `ticks` ticks, aimed at the grunt's chest, then lets
    // go. One action instance for the whole hold, as a client sends it.
    void hold(std::uint8_t weapon, std::uint16_t binding, int ticks) {
        const KernelServerEntityState me = entity_state(kernel, player);
        const KernelServerEntityState target = entity_state(kernel, grunt);
        const std::uint32_t action = 9000u + next_action++;
        for (int tick_index = 0; tick_index <= ticks; ++tick_index) {
            const bool held = tick_index < ticks;
            KernelPlayerInput input{};
            input.input_seq = next_seq++;
            input.aim_dir = KernelVec3{
                target.position.x - me.position.x,
                (target.position.y + 1.0f) - (me.position.y + 1.0f),
                target.position.z - me.position.z};
            input.selected_weapon = weapon;
            input.action_intent =
                KernelActionIntent{action, binding, 0u, 0u};
            input.action_input =
                KernelActionInput{action, static_cast<std::uint8_t>(held ? 1u : 0u), 0u, 0u};
            require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
            tick();
        }
        tick(10);
    }
};

void print_state(const char* label, Arena& arena) {
    const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
    const KernelServerEntityState target = entity_state(arena.kernel, arena.grunt);
    std::fprintf(
        stderr,
        "%s: grunt hp=%u | slots=%u active=%u ids=[%u %u %u %u] "
        "ammo=[%u %u %u %u] reserve=[%u %u %u %u] reloading=%u action_template=%u\n",
        label,
        target.hp,
        me.weapon_slot_count,
        me.active_weapon_slot,
        me.weapon_ids[0], me.weapon_ids[1], me.weapon_ids[2], me.weapon_ids[3],
        me.ammo[0], me.ammo[1], me.ammo[2], me.ammo[3],
        me.reserve_magazines[0], me.reserve_magazines[1],
        me.reserve_magazines[2], me.reserve_magazines[3],
        me.is_reloading,
        me.action.action_template_id);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const std::vector<std::uint8_t> scene = read_ground_scene();
    const gs::ActorTemplateConfig* grunt_template = nullptr;
    for (const auto& candidate : config.actor_templates) {
        if (candidate.name == "chaser_grunt") grunt_template = &candidate;
    }
    require(grunt_template != nullptr);

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
    require(Kernel_StartDedicatedServer(arena.kernel, 8056));

    arena.player = spawn_actor(
        arena.kernel, config.player.actor_template_id, gs::kActorTypePlayer, 0u,
        KernelVec3{0.0f, 1.0f, 0.0f});
    arena.grunt = spawn_actor(
        arena.kernel, grunt_template->actor_template_id, gs::kActorTypeAgent, 0u,
        KernelVec3{8.0f, 1.0f, 0.0f});
    // What game_server does for a joining player: each loadout weapon's
    // mechanics go on the entity. Spawning alone leaves them off and nothing
    // fires.
    for (const std::uint8_t weapon : {std::uint8_t{13}, std::uint8_t{14}, std::uint8_t{15}, kRifle}) {
        require(config.weapons.configured[weapon]);
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[weapon];
        require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    }
    arena.tick(30);
    print_state("start", arena);

    // Control: the rifle, while the loadout still holds it, hits the grunt.
    const std::uint16_t grunt_full = entity_state(arena.kernel, arena.grunt).hp;
    arena.hold(kRifle, KernelActionBinding_PrimaryFire, 9);
    print_state("armed rifle", arena);
    const std::uint16_t grunt_after_control = entity_state(arena.kernel, arena.grunt).hp;
    require(grunt_after_control < grunt_full);

    // Unarm: nothing in any slot.
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    combat.hp = entity_state(arena.kernel, arena.player).hp;
    combat.active_weapon_slot = 0;
    for (std::size_t slot = 0; slot < KERNEL_MAX_WEAPON_SLOTS; ++slot) {
        combat.weapon_ids[slot] = 0;
        combat.ammo[slot] = 0;
        combat.reserve_magazines[slot] = 0;
    }
    combat.weapon_slot_count = 0;
    const bool unarm_accepted =
        Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat);
    arena.tick();
    std::fprintf(stderr, "set 0 slots accepted=%d\n", unarm_accepted ? 1 : 0);
    print_state("after set 0 slots", arena);
    require(!unarm_accepted);
    require(entity_state(arena.kernel, arena.player).weapon_slot_count == 4u);

    // The closest legal thing: the staff alone. The rifle's mechanics are
    // still on the entity, as they would be after a drop that only rewrote
    // the loadout.
    combat.weapon_slot_count = 1;
    combat.weapon_ids[0] = kMeteorStaff;
    combat.ammo[0] = 3;
    combat.reserve_magazines[0] = 6;
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    arena.tick();
    print_state("staff only", arena);
    const auto unchanged = [&](const char* label) {
        const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
        if (me.weapon_slot_count != 1u || me.weapon_ids[0] != kMeteorStaff ||
            me.ammo[0] != 3u || me.reserve_magazines[0] != 6u ||
            entity_state(arena.kernel, arena.grunt).hp != grunt_after_control) {
            std::fprintf(stderr, "changed after: %s\n", label);
            require(false);
        }
    };
    unchanged("staff only");

    // What a client with nothing selected sends by default: selected_weapon 0.
    arena.hold(kRifle, KernelActionBinding_PrimaryFire, 9);
    print_state("staff only, fire id 0", arena);
    unchanged("fire id 0");
    const std::uint16_t grunt_after_id0 = entity_state(arena.kernel, arena.grunt).hp;

    // A client whose UI still shows a dropped weapon.
    arena.hold(14, KernelActionBinding_PrimaryFire, 2);
    print_state("staff only, fire id 14", arena);
    unchanged("fire id 14");

    arena.hold(kRifle, KernelActionBinding_Reload, 1);
    print_state("staff only, reload id 0", arena);
    unchanged("reload id 0");

    // The rifle's mechanics gone too.
    require(Kernel_ServerClearEntityWeaponMechanics(arena.kernel, arena.player, kRifle));
    arena.hold(kRifle, KernelActionBinding_PrimaryFire, 9);
    print_state("rifle mechanics cleared, fire id 0", arena);
    unchanged("fire id 0, mechanics cleared");
    const std::uint16_t grunt_after_cleared = entity_state(arena.kernel, arena.grunt).hp;

    // The one weapon left still works.
    arena.hold(kMeteorStaff, KernelActionBinding_PrimaryFire, 2);
    print_state("staff only, fire id 13", arena);
    // The control for every "unchanged" above: the staff does fire.
    require(entity_state(arena.kernel, arena.player).ammo[0] == 2u);

    arena.tick(60);
    print_state("after 60 idle ticks", arena);
    const std::uint16_t grunt_end = entity_state(arena.kernel, arena.grunt).hp;

    std::fprintf(
        stderr,
        "grunt hp: full %u, armed rifle %u, id0 w/ mechanics %u, id0 cleared %u, end %u\n",
        grunt_full, grunt_after_control, grunt_after_id0, grunt_after_cleared, grunt_end);
    // And its strike lands.
    require(grunt_end < grunt_after_cleared);
    std::puts("unarmed_fire_probe_test passed");
    return 0;
}
