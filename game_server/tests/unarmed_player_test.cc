// A player with no weapon at all: allowed (design D18, 2026-10-07), and able
// to do nothing but play on items.
//
// Two doors used to refuse an empty loadout -- the YAML loader (1-4
// weapon_slots) and Kernel_ServerSetEntityCombatState (weapon_slot_count 0).
// Both now let a player through; an agent still needs a weapon.
//
// The worry with no weapon is weapon id 0. WeaponState answers "no active
// weapon" with id 0, and id 0 is also the shipped Rifle; a client with nothing
// selected still sends selected_weapon 0. Measured on main before the change
// and pinned here: the loadout, not the catalog or the entity's mechanics,
// decides what can fire. A weapon id not in the loadout -- 0, or a dropped
// weapon's -- does nothing, even while its mechanics are still on the entity.
//
// The controls: the rifle hits a grunt 8 m down the aim while it is still in
// the loadout, and the staff fires before and after the unarmed stretch -- so
// an unharmed grunt means "the shot did not happen", not "the shot missed".
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
#include <stdexcept>
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

// The shipped catalog with one template's weapon_slots emptied. Returns the
// load error, or "" with *out filled.
std::string load_with_empty_weapon_slots(
    const std::string& template_file,
    gs::GameServerGameplayConfig* out) {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const std::filesystem::path source = catalog_root();
    const std::filesystem::path root =
        std::filesystem::path(tmp) / ("catalog_" + template_file);
    std::filesystem::remove_all(root);
    std::filesystem::copy(source, root, std::filesystem::copy_options::recursive);
    const std::filesystem::path path = root / "entity_templates" / template_file;
    std::ifstream in(path);
    require(in.good());
    std::string text;
    std::string line;
    bool in_slots = false;
    bool replaced = false;
    while (std::getline(in, line)) {
        if (line.rfind("weapon_slots:", 0) == 0) {
            text += "weapon_slots: []\n";
            in_slots = true;
            replaced = true;
            continue;
        }
        if (in_slots && line.rfind("  - ", 0) == 0) continue;
        in_slots = false;
        text += line + "\n";
    }
    in.close();
    require(replaced);
    std::ofstream(path, std::ios::trunc) << text;
    try {
        *out = gs::load_gameplay_config_from_catalog_file(
            (root / "gameplay_catalog.yaml").string());
    } catch (const std::exception& error) {
        return error.what();
    }
    return "";
}

void loadout_yaml_allows_an_unarmed_player_only() {
    gs::GameServerGameplayConfig unarmed;
    const std::string player_error =
        load_with_empty_weapon_slots("1_player.yaml", &unarmed);
    std::fprintf(stderr, "player weapon_slots []: %s\n",
                 player_error.empty() ? "ok" : player_error.c_str());
    require(player_error.empty());
    const KernelCombatStateDefinition combat = gs::make_player_combat_state(unarmed);
    require(combat.weapon_slot_count == 0u);
    require(combat.active_weapon_slot == 0u);
    // The kernel takes the catalog, and a player spawned the way game_server
    // spawns one takes the empty loadout.
    {
        KernelConfig kernel_config{};
        kernel_config.mode = KernelMode_DedicatedServer;
        kernel_config.tick.server_tick_rate = 30;
        kernel_config.tick.snapshot_rate = 15;
        kernel_config.max_events = 256;
        kernel_config.max_render_states = 64;
        KernelHandle* kernel = Kernel_Create(&kernel_config);
        require(kernel != nullptr);
        require(gs::load_kernel_gameplay_catalog(kernel, unarmed));
        require(Kernel_StartDedicatedServer(kernel, 8057));
        const std::uint32_t player = spawn_actor(
            kernel, unarmed.player.actor_template_id, gs::kActorTypePlayer, 0u,
            KernelVec3{0.0f, 1.0f, 0.0f});
        KernelCombatStateDefinition spawn_combat = combat;
        require(Kernel_ServerSetEntityCombatState(kernel, player, &spawn_combat));
        Kernel_Update(kernel, kTickSeconds);
        require(entity_state(kernel, player).weapon_slot_count == 0u);
        Kernel_Destroy(kernel);
    }

    gs::GameServerGameplayConfig ignored;
    const std::string agent_error =
        load_with_empty_weapon_slots("26_chaser_grunt.yaml", &ignored);
    std::fprintf(stderr, "chaser_grunt weapon_slots []: %s\n", agent_error.c_str());
    require(agent_error.find("weapon_slots count must be 1 to 4") != std::string::npos);
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

    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    combat.hp = entity_state(arena.kernel, arena.player).hp;
    combat.active_weapon_slot = 0;
    for (std::size_t slot = 0; slot < KERNEL_MAX_WEAPON_SLOTS; ++slot) {
        combat.weapon_ids[slot] = 0;
        combat.ammo[slot] = 0;
        combat.reserve_magazines[slot] = 0;
    }

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

    // And its strike lands.
    require(grunt_end < grunt_after_cleared);

    // Unarmed: nothing in any slot.
    combat.weapon_slot_count = 0;
    combat.active_weapon_slot = 0;
    combat.weapon_ids[0] = 0;
    combat.ammo[0] = 0;
    combat.reserve_magazines[0] = 0;
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    arena.tick();
    print_state("unarmed", arena);
    require(entity_state(arena.kernel, arena.player).weapon_slot_count == 0u);
    // An active slot that names a slot it does not have is still refused.
    combat.active_weapon_slot = 1;
    require(!Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    combat.active_weapon_slot = 0;
    const std::uint16_t grunt_unarmed = entity_state(arena.kernel, arena.grunt).hp;
    const auto still_unarmed = [&](const char* label) {
        const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
        if (me.weapon_slot_count != 0u || me.action.action_template_id != 0u ||
            entity_state(arena.kernel, arena.grunt).hp != grunt_unarmed) {
            std::fprintf(stderr, "changed after: %s\n", label);
            require(false);
        }
    };
    // The rifle's mechanics are back on the entity, so only the empty loadout
    // stands between selected_weapon 0 and a rifle shot.
    {
        KernelWeaponMechanicsDefinition mechanics = config.weapons.definitions[kRifle];
        require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    }
    arena.hold(kRifle, KernelActionBinding_PrimaryFire, 9);
    print_state("unarmed, fire id 0", arena);
    still_unarmed("unarmed fire id 0");
    arena.hold(kMeteorStaff, KernelActionBinding_PrimaryFire, 2);
    still_unarmed("unarmed fire id 13");
    arena.hold(kRifle, KernelActionBinding_Reload, 1);
    still_unarmed("unarmed reload");
    arena.tick(60);
    still_unarmed("unarmed, 60 idle ticks");

    // Armed again, the staff fires.
    combat.weapon_slot_count = 1;
    combat.weapon_ids[0] = kMeteorStaff;
    combat.ammo[0] = 2;
    combat.reserve_magazines[0] = 6;
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    arena.tick();
    arena.hold(kMeteorStaff, KernelActionBinding_PrimaryFire, 2);
    print_state("re-armed, fire id 13", arena);
    require(entity_state(arena.kernel, arena.player).ammo[0] == 1u);

    std::fprintf(
        stderr,
        "grunt hp: full %u, armed rifle %u, id0 w/ mechanics %u, id0 cleared %u, end %u\n",
        grunt_full, grunt_after_control, grunt_after_id0, grunt_after_cleared, grunt_end);

    loadout_yaml_allows_an_unarmed_player_only();
    std::puts("unarmed_player_test passed");
    return 0;
}
