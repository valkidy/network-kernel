// Lock-on Magic Missile end to end (docs/PLAYER_WEAPON_REVAMP_PLAN.md I2, D3):
// an actor strike (KernelWeaponFireMode_TargetedStrikeActor, ABI 110) fired
// through the real input path by a player at two gingerbread infantry, the
// near one 10 m straight ahead and the far one 12 m out, a few degrees off --
// both inside the 45 m, 20-degree cone.
//
//   - aimed away from both: refused, no ammunition spent, nobody hurt;
//   - aimed at them: the near one takes 12-16 missiles of 8 and the far one
//     none -- the volley goes to the one actor the cast locked;
//   - the near one with only 20 hp left: it dies a few missiles in, and the
//     rest are not fired at the far one, which a strike that searched afresh
//     each repeat would have turned to (plan D3c).
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

namespace fs = std::filesystem;
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
constexpr std::uint8_t kLockOnMagicMissile = 27;
constexpr std::uint32_t kPeer = 1;

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
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

// An agent that dies is despawned: gone counts as 0.
std::uint16_t hp_or_gone(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    return Kernel_ServerGetEntityState(kernel, net_id, &state) ? state.hp : 0u;
}

std::uint32_t template_id_of(const gs::GameServerGameplayConfig& config, const char* name) {
    for (const auto& entity : config.entity_templates) {
        if (entity.name == name) return entity.actor_template_id;
    }
    require(false);
    return 0;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void send(const KernelVec3& aim, std::uint32_t action, bool start) {
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        input.aim_dir = aim;
        input.selected_weapon = kLockOnMagicMissile;
        if (action != 0u) {
            if (start) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input = KernelActionInput{action, 0u, 0u, 0u};
        }
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        Kernel_Update(kernel, kTickSeconds);
    }

    void idle(const KernelVec3& aim, int ticks) {
        for (int index = 0; index < ticks; ++index) send(aim, 0u, false);
    }

    // One press, then `ticks` more: the cast (45 ticks), the volley (up to
    // 45 more) and the longest flight are all inside 150.
    void cast(const KernelVec3& aim, int ticks) {
        const std::uint32_t action = 7000u + next_action++;
        send(aim, action, true);
        for (int index = 0; index < ticks; ++index) send(aim, action, false);
    }

    std::uint16_t ammo() const {
        const KernelServerEntityState me = state_of(kernel, player);
        return me.ammo[me.active_weapon_slot];
    }

    std::uint32_t spawn_hostile(std::uint32_t template_id, const KernelVec3& at) {
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_type = KernelEntityType_Actor;
        create.actor_type = KernelActorType_Agent;
        create.entity_template_id = template_id;
        create.actor_template_id = template_id;
        create.position = at;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
        require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
        return net_id;
    }
};

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    const KernelWeaponMechanicsDefinition& weapon = config.weapons.definitions[kLockOnMagicMissile];
    require(config.weapons.configured[kLockOnMagicMissile]);
    require(weapon.fire_mode == KernelWeaponFireMode_TargetedStrikeActor);
    std::uint16_t missile_damage = 0;
    for (const auto& projectile : config.projectile_templates) {
        if (projectile.name == "lock_on_magic_missile_bolt") {
            missile_damage = projectile.definition.mechanics.damage;
        }
    }
    require(missile_damage == 8u);

    const std::vector<std::uint8_t> scene =
        read_bytes(runfiles_catalog() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh");
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 4096;
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
    require(Kernel_StartDedicatedServer(arena.kernel, 8099));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.owner_peer = kPeer;
    create.position = KernelVec3{0.0f, 1.0f, 0.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    combat.weapon_slot_count = 1;
    combat.active_weapon_slot = 0;
    combat.weapon_ids[0] = kLockOnMagicMissile;
    combat.ammo[0] = weapon.magazine_size;
    combat.reserve_magazines[0] = weapon.reserve_magazines;
    KernelWeaponMechanicsDefinition mechanics = weapon;
    require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));

    const std::uint32_t infantry = template_id_of(config, "gingerbread_infantry");
    const std::uint32_t near = arena.spawn_hostile(infantry, {10.0f, 1.0f, 0.0f});
    const std::uint32_t far = arena.spawn_hostile(infantry, {12.0f, 1.0f, 1.0f});
    const KernelVec3 at_them{1.0f, 0.0f, 0.0f};
    const KernelVec3 away{-1.0f, 0.0f, 0.0f};
    arena.idle(at_them, 30);
    const std::uint16_t full = state_of(arena.kernel, near).hp;
    require(full >= 200u);
    require(state_of(arena.kernel, far).hp == full);

    // Aimed away: nobody in the cone, refused at no cost.
    {
        const std::uint16_t before = arena.ammo();
        arena.cast(away, 150);
        std::fprintf(stderr, "lock_on_mm[away]: ammo %u -> %u\n", before, arena.ammo());
        require(arena.ammo() == before);
        require(state_of(arena.kernel, near).hp == full);
        require(state_of(arena.kernel, far).hp == full);
    }

    // Aimed at them: the near one takes the whole volley, the far one nothing.
    {
        const std::uint16_t before = arena.ammo();
        arena.cast(at_them, 150);
        const std::uint16_t near_hp = state_of(arena.kernel, near).hp;
        const std::uint16_t far_hp = state_of(arena.kernel, far).hp;
        const int missiles = (full - near_hp) / missile_damage;
        std::fprintf(stderr,
                     "lock_on_mm[volley]: ammo %u -> %u, near %u -> %u (%d missiles, "
                     "remainder %d), far %u -> %u\n",
                     before, arena.ammo(), full, near_hp, missiles,
                     (full - near_hp) % missile_damage, full, far_hp);
        require(arena.ammo() + 1u == before);
        require((full - near_hp) % missile_damage == 0);
        require(missiles >= 12 && missiles <= 16);
        require(far_hp == full);
    }

    // The near one at 20 hp: it dies after three missiles, and the rest do not
    // turn to the far one.
    {
        require(Kernel_ServerSetEntityHealth(arena.kernel, near, 20u));
        arena.idle(at_them, 40);
        arena.cast(at_them, 150);
        const std::uint16_t near_hp = hp_or_gone(arena.kernel, near);
        const std::uint16_t far_hp = state_of(arena.kernel, far).hp;
        std::fprintf(stderr, "lock_on_mm[dies]: near 20 -> %u, far %u -> %u\n",
                     near_hp, full, far_hp);
        require(near_hp == 0u);
        require(far_hp == full);
    }

    std::puts("lock_on_magic_missile_test passed");
    return 0;
}
