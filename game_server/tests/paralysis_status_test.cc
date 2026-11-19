// The paralysis status end to end (docs/PLAYER_WEAPON_REVAMP_PLAN.md I4, D11):
// the shipped status, applied by a real hit, holds a player down for its
// 120 ticks. While it stands the player
//
//   - does not walk, whatever it asks (a control walks the same input after);
//   - starts no action: its fire costs nothing and spawns nothing;
//   - is still pushed: a rocket's knockback carries it;
//   - builds no stagger: two rockets inside the decay window stagger it after
//     (the control), and do not while it is down;
//   - carries KERNEL_VISUAL_FLAG_DOWNED, which goes when the status does.
//
// The hit is the water bubble staff's bullet with its status swapped for
// paralysis in a copy of the shipped catalog -- a projectile that applies a
// status on contact, so nothing else about the shot is new. Player one shoots;
// player two, 3 m away, is the one held.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

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
constexpr std::uint8_t kWaterBubble = 17;
constexpr std::uint8_t kRocket = 3;
constexpr std::uint32_t kShooterPeer = 1;
constexpr std::uint32_t kTargetPeer = 2;

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The shipped catalog with the water bubble bullet applying paralysis.
gs::GameServerGameplayConfig paralysing_catalog() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    require(tmp != nullptr);
    const fs::path root = fs::path(tmp) / "paralysis_catalog";
    fs::remove_all(root);
    fs::copy(runfiles_catalog(), root,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    const fs::path bullet =
        root / "projectile_templates" / "31_projectile_water_bubble_spammer_bullet.yaml";
    std::string text = read_text(bullet);
    const std::string from = "status: water_bubble";
    const std::size_t at = text.find(from);
    require(at != std::string::npos);
    text.replace(at, from.size(), "status: paralysis");
    // The copy's files are symlinks into the runfiles: drop the link first.
    fs::remove(bullet);
    std::ofstream(bullet, std::ios::binary) << text;
    return gs::load_gameplay_config_from_catalog_file((root / "gameplay_catalog.yaml").string());
}

KernelServerEntityState state_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

float horizontal_distance(const KernelVec3& a, const KernelVec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

bool downed(KernelHandle* kernel, std::uint32_t net_id) {
    return (state_of(kernel, net_id).visual_flags & KERNEL_VISUAL_FLAG_DOWNED) != 0u;
}

bool staggered(KernelHandle* kernel, std::uint32_t net_id) {
    return (state_of(kernel, net_id).visual_flags & KERNEL_VISUAL_FLAG_STAGGERED) != 0u;
}

struct Player {
    std::uint32_t net_id = 0;
    std::uint32_t next_seq = 1;
};

struct Arena {
    KernelHandle* kernel = nullptr;
    Player shooter;
    Player target;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    // One input for each player, then one tick.
    void tick(const KernelPlayerInput& shooter_input, const KernelPlayerInput& target_input) {
        KernelPlayerInput a = shooter_input;
        a.input_seq = shooter.next_seq++;
        require(Kernel_ServerSubmitEntityInput(kernel, shooter.net_id, &a));
        KernelPlayerInput b = target_input;
        b.input_seq = target.next_seq++;
        require(Kernel_ServerSubmitEntityInput(kernel, target.net_id, &b));
        Kernel_Update(kernel, kTickSeconds);
    }

    static KernelPlayerInput idle_input(std::uint8_t weapon, KernelVec3 aim) {
        KernelPlayerInput input{};
        input.aim_dir = aim;
        input.selected_weapon = weapon;
        return input;
    }

    void idle(int ticks, std::uint8_t shooter_weapon = kWaterBubble) {
        for (int index = 0; index < ticks; ++index) {
            tick(idle_input(shooter_weapon, {1.0f, 0.0f, 0.0f}),
                 idle_input(kWaterBubble, {-1.0f, 0.0f, 0.0f}));
        }
    }

    // The shooter presses `weapon`'s fire once, held for `held_ticks`.
    void shooter_fires(std::uint8_t weapon, int held_ticks) {
        const std::uint32_t action = 4000u + next_action++;
        for (int index = 0; index <= held_ticks; ++index) {
            KernelPlayerInput input = idle_input(weapon, {1.0f, 0.0f, 0.0f});
            if (index == 0) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input = KernelActionInput{
                action, static_cast<std::uint8_t>(index < held_ticks ? 1u : 0u), 0u, 0u};
            tick(input, idle_input(kWaterBubble, {-1.0f, 0.0f, 0.0f}));
        }
    }

    // The target walks +x (away from the shooter) for `ticks`; how far it got.
    float target_walks(int ticks) {
        const KernelVec3 from = state_of(kernel, target.net_id).position;
        for (int index = 0; index < ticks; ++index) {
            KernelPlayerInput walk = idle_input(kWaterBubble, {-1.0f, 0.0f, 0.0f});
            walk.move = KernelVec2{0.0f, -1.0f};
            tick(idle_input(kWaterBubble, {1.0f, 0.0f, 0.0f}), walk);
        }
        return horizontal_distance(from, state_of(kernel, target.net_id).position);
    }

    // The target presses fire at the shooter, held for its first commit.
    void target_fires() {
        const std::uint32_t action = 6000u + next_action++;
        for (int index = 0; index <= 2; ++index) {
            KernelPlayerInput input = idle_input(kWaterBubble, {-1.0f, 0.0f, 0.0f});
            if (index == 0) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input =
                KernelActionInput{action, static_cast<std::uint8_t>(index < 2 ? 1u : 0u), 0u, 0u};
            tick(idle_input(kWaterBubble, {1.0f, 0.0f, 0.0f}), input);
        }
    }

    std::uint16_t target_ammo() const {
        return state_of(kernel, target.net_id).ammo[0];
    }

    // Two rockets at the target, 31 ticks apart: inside the player's 30-tick
    // stagger decay delay, so their meter adds up past the threshold. Returns
    // whether the target was staggered at any tick of it; peak_rise is how
    // high the knockback lifted it. A direct hit bursts on the target itself,
    // so the blast's radial push is all lift (5 m/s, ~1.3 m) and no carry.
    float peak_rise = 0.0f;
    bool two_rockets() {
        bool seen = false;
        peak_rise = 0.0f;
        const float ground = state_of(kernel, target.net_id).position.y;
        for (int rocket = 0; rocket < 2; ++rocket) {
            shooter_fires(kRocket, 0);
            for (int index = 0; index < 30; ++index) {
                idle(1, kRocket);
                seen = seen || staggered(kernel, target.net_id);
                peak_rise = std::max(
                    peak_rise, state_of(kernel, target.net_id).position.y - ground);
            }
        }
        for (int index = 0; index < 10; ++index) {
            idle(1, kRocket);
            seen = seen || staggered(kernel, target.net_id);
            peak_rise = std::max(
                peak_rise, state_of(kernel, target.net_id).position.y - ground);
        }
        return seen;
    }

    void put_back(const KernelVec3& at) {
        const KernelServerEntityState state = state_of(kernel, target.net_id);
        require(Kernel_ServerSetEntityTransform(kernel, target.net_id, &at, &state.rotation));
        const KernelVec3 zero{0.0f, 0.0f, 0.0f};
        require(Kernel_ServerSetEntityVelocity(kernel, target.net_id, &zero));
    }
};

std::uint32_t spawn_player(
    KernelHandle* kernel,
    const gs::GameServerGameplayConfig& config,
    std::uint32_t peer,
    const KernelVec3& position,
    const KernelVec3& facing) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.owner_peer = peer;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    (void)facing;
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, config.player.actor_template_id));
    // Two weapons, slot 0 the bubble staff and slot 1 the rocket, full.
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    combat.weapon_slot_count = 2;
    combat.active_weapon_slot = 0;
    combat.weapon_ids[0] = kWaterBubble;
    combat.weapon_ids[1] = kRocket;
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        const KernelWeaponMechanicsDefinition& weapon =
            config.weapons.definitions[combat.weapon_ids[slot]];
        combat.ammo[slot] = weapon.magazine_size;
        combat.reserve_magazines[slot] = weapon.reserve_magazines;
        KernelWeaponMechanicsDefinition mechanics = weapon;
        require(Kernel_ServerSetEntityWeaponMechanics(kernel, net_id, &mechanics));
    }
    require(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
    return net_id;
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = paralysing_catalog();
    std::uint32_t paralysis_ticks = 0;
    for (const auto& status : config.status_effect_templates) {
        if (status.name == "paralysis") paralysis_ticks = status.duration_ticks;
    }
    require(paralysis_ticks == 120u);

    const std::vector<std::uint8_t> scene =
        read_bytes(runfiles_catalog() / "mesh_assets" / "jolt" / "plane_200x200.joltmesh");
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 2048;
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
    require(Kernel_StartDedicatedServer(arena.kernel, 8097));

    arena.shooter.net_id = spawn_player(
        arena.kernel, config, kShooterPeer, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    arena.target.net_id = spawn_player(
        arena.kernel, config, kTargetPeer, {3.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    arena.idle(30);
    const KernelVec3 home = state_of(arena.kernel, arena.target.net_id).position;
    require(!downed(arena.kernel, arena.target.net_id));

    // Controls, before anything is applied: the walk goes somewhere, a fire
    // spends, and two rockets stagger.
    const float free_walk = arena.target_walks(20);
    std::fprintf(stderr, "paralysis: free walk %.2f m in 20 ticks\n", free_walk);
    require(free_walk > 1.0f);
    arena.put_back(home);
    arena.idle(10);
    {
        const std::uint16_t before = arena.target_ammo();
        arena.target_fires();
        require(arena.target_ammo() < before);
    }
    arena.idle(130);
    const bool control_staggered = arena.two_rockets();
    const float free_rise = arena.peak_rise;
    std::fprintf(stderr, "paralysis: two rockets, free: lifted %.2f m, staggered %s\n",
                 free_rise, control_staggered ? "yes" : "no");
    require(control_staggered);
    require(free_rise > 0.5f);
    // Past the stagger's immunity, and the meter decayed.
    arena.idle(150);
    arena.put_back(home);
    arena.idle(30);
    require(!staggered(arena.kernel, arena.target.net_id));

    // The hit: one bubble bullet from 3 m.
    arena.shooter_fires(kWaterBubble, 1);
    int waited = 0;
    while (!downed(arena.kernel, arena.target.net_id) && waited < 60) {
        arena.idle(1);
        ++waited;
    }
    require(downed(arena.kernel, arena.target.net_id));
    std::fprintf(stderr, "paralysis: downed %d ticks after the shot\n", waited);
    int downed_ticks = 0;

    // Held: the walk goes nowhere.
    const float held_walk = arena.target_walks(20);
    downed_ticks += 20;
    std::fprintf(stderr, "paralysis: held walk %.3f m in 20 ticks\n", held_walk);
    require(held_walk < 0.05f);

    // No action: the fire spends nothing.
    {
        const std::uint16_t before = arena.target_ammo();
        arena.target_fires();
        downed_ticks += 3;
        require(arena.target_ammo() == before);
    }

    // Pushed, and not staggered, by the same two rockets.
    const std::uint16_t hp_before_rockets = state_of(arena.kernel, arena.target.net_id).hp;
    const bool held_staggered = arena.two_rockets();
    downed_ticks += 2 * 31 + 10;
    const std::uint16_t hp_after_rockets = state_of(arena.kernel, arena.target.net_id).hp;
    std::fprintf(stderr,
                 "paralysis: two rockets, downed: hp %u -> %u, lifted %.2f m, staggered %s\n",
                 hp_before_rockets, hp_after_rockets, arena.peak_rise,
                 held_staggered ? "yes" : "no");
    // They hit (the damage), they pushed (the lift, as free), and no stagger.
    require(hp_after_rockets < hp_before_rockets);
    require(arena.peak_rise > 0.8f * free_rise);
    require(!held_staggered);
    require(downed(arena.kernel, arena.target.net_id));

    // It ends with the status, and the walk works again.
    while (downed(arena.kernel, arena.target.net_id) && downed_ticks < 200) {
        arena.idle(1);
        ++downed_ticks;
    }
    std::fprintf(stderr, "paralysis: up after ~%d ticks (status %u)\n",
                 downed_ticks, paralysis_ticks);
    require(!downed(arena.kernel, arena.target.net_id));
    require(downed_ticks <= static_cast<int>(paralysis_ticks) + 5);
    arena.idle(60);
    const float after_walk = arena.target_walks(20);
    std::fprintf(stderr, "paralysis: walk after %.2f m in 20 ticks\n", after_walk);
    require(after_walk > 1.0f);

    std::puts("paralysis_status_test passed");
    return 0;
}
