// The shipped targeted-strike weapons, driven end to end on a dedicated server
// with the shipped catalog and static scene: a press, the mark on the ground,
// the fall, the damage. Also prints what one cast measures -- ticks from press
// to mark, mark to fall, fall to damage, and how many times one target is hit
// -- because those are the numbers tuning will ask about.
//
// Projectiles report no template over the API, so they are told apart by how
// they move: a mark or a blast rests on the ground, a meteor or a beam falls.
// A second target out of reach is the control for every damage check.
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

#include "game_server/src/agent_runtime.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint8_t kMeteorStaff = 13;
constexpr std::uint8_t kMeteorStormStaff = 14;
constexpr std::uint8_t kSkyLaser = 15;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

struct Catalog {
    network_example::game_server::GameServerGameplayConfig config;
    network_example::game_server::KernelGameplayCatalogStorage storage;
    std::vector<std::uint8_t> scene_bytes;
};

Catalog load_catalog() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip")
            .string());
    Catalog catalog;
    catalog.config =
        network_example::game_server::load_gameplay_config_from_bundle_memory(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            "gameplay_catalog.yaml");
    catalog.storage =
        network_example::game_server::build_kernel_gameplay_catalog(catalog.config);
    catalog.scene_bytes =
        network_example::game_server::load_gameplay_bundle_entry_bytes(
            bundle.data(),
            static_cast<std::uint32_t>(bundle.size()),
            catalog.config.static_collision_scene.entry_path);
    require(!catalog.scene_bytes.empty());
    return catalog;
}

std::uint32_t collider_id_of(const Catalog& catalog, const char* name) {
    for (const auto& collider : catalog.config.colliders.templates) {
        if (collider.name == name) {
            return collider.definition.template_id;
        }
    }
    return 0;
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

std::vector<KernelServerEntityState> projectiles(KernelHandle* kernel) {
    std::vector<KernelServerEntityState> states(256);
    for (KernelServerEntityState& state : states) {
        state.struct_size = sizeof(KernelServerEntityState);
    }
    const std::uint32_t count = Kernel_ServerQueryEntities(
        kernel,
        KernelEntityType_Projectile,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    states.resize(count);
    return states;
}

float speed(const KernelServerEntityState& state) {
    return std::sqrt(
        state.velocity.x * state.velocity.x +
        state.velocity.y * state.velocity.y +
        state.velocity.z * state.velocity.z);
}

float horizontal_distance(const KernelVec3& lhs, const KernelVec3& rhs) {
    return std::hypot(lhs.x - rhs.x, lhs.z - rhs.z);
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint32_t player = 0;
    std::uint32_t target = 0;
    std::uint32_t bystander = 0;
    float ground = 0.0f;
    std::uint32_t next_action = 1;
    std::uint32_t next_seq = 1;

    ~Arena() {
        if (kernel != nullptr) {
            Kernel_Destroy(kernel);
        }
    }

    void tick(int count = 1) {
        for (int index = 0; index < count; ++index) {
            Kernel_Update(kernel, kTickSeconds);
        }
    }

    KernelVec3 launch_point() {
        const KernelServerEntityState state = entity_state(kernel, player);
        return KernelVec3{
            state.position.x, state.position.y + 1.0f, state.position.z};
    }

    void fire(std::uint8_t weapon, const KernelVec3& aim_at) {
        const KernelVec3 from = launch_point();
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        input.aim_dir = KernelVec3{
            aim_at.x - from.x, aim_at.y - from.y, aim_at.z - from.z};
        input.selected_weapon = weapon;
        const std::uint32_t action = 9000u + next_action++;
        input.action_intent =
            KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
        input.action_input = KernelActionInput{action, 1u, 0u, 0u};
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
    }

    std::uint16_t ammo() { return entity_state(kernel, player).ammo[0]; }
};

std::uint32_t spawn_actor(
    KernelHandle* kernel,
    std::uint32_t template_id,
    std::uint16_t actor_type,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = actor_type;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

// A dedicated server on the shipped scene: the player holding `weapon` in its
// only slot, a grunt 8 m ahead, and a second grunt 30 m off to the side that no
// cast here reaches.
void open_arena(
    const Catalog& catalog,
    std::uint8_t weapon,
    std::uint16_t port,
    Arena* arena) {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 4096;
    config.max_render_states = 256;
    arena->kernel = Kernel_Create(&config);
    require(arena->kernel != nullptr);

    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena->kernel, &scene));
    require(Kernel_StartDedicatedServer(arena->kernel, port));
    require(Kernel_LoadGameplayCatalog(
        arena->kernel, &catalog.storage.definition, nullptr));

    const std::uint32_t player_template = catalog.config.player.actor_template_id;
    const network_example::game_server::ActorTemplateConfig* grunt = nullptr;
    for (const auto& candidate : catalog.config.actor_templates) {
        if (candidate.name == "chaser_grunt") {
            grunt = &candidate;
        }
    }
    require(player_template != 0);
    require(grunt != nullptr);

    arena->player = spawn_actor(
        arena->kernel, player_template,
        network_example::game_server::kActorTypePlayer,
        KernelVec3{0.0f, 1.0f, 0.0f});
    arena->target = spawn_actor(
        arena->kernel, grunt->actor_template_id,
        network_example::game_server::kActorTypeAgent,
        KernelVec3{8.0f, 1.0f, 0.0f});
    arena->bystander = spawn_actor(
        arena->kernel, grunt->actor_template_id,
        network_example::game_server::kActorTypeAgent,
        KernelVec3{0.0f, 1.0f, 30.0f});

    KernelWeaponMechanicsDefinition mechanics =
        catalog.config.weapons.definitions[weapon];
    require(catalog.config.weapons.configured[weapon]);
    require(Kernel_ServerSetEntityWeaponMechanics(
        arena->kernel, arena->player, &mechanics));
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_slot = 0;
    combat.weapon_slot_count = 1;
    combat.collider_template_id = collider_id_of(catalog, "player_hit_aabb");
    require(combat.collider_template_id != 0);
    combat.move_speed_meters_per_second = 5.0f;
    combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    combat.weapon_ids[0] = weapon;
    combat.ammo[0] = mechanics.magazine_size;
    combat.reserve_magazines[0] = 2;
    require(Kernel_ServerSetEntityCombatState(arena->kernel, arena->player, &combat));

    // Let everyone settle, then take the ground from the player: a scene that
    // silently failed to load would leave them falling, and every position
    // below would be meaningless.
    arena->tick(30);
    const KernelServerEntityState settled = entity_state(arena->kernel, arena->player);
    require(std::fabs(settled.velocity.y) < 0.05f);
    require(settled.position.y > -2.0f && settled.position.y < 3.0f);
    arena->ground = settled.position.y;
    require(std::fabs(entity_state(arena->kernel, arena->target).position.y -
                      arena->ground) < 0.5f);
}

// Meteor Staff at the grunt: the mark lands at its feet on the next tick, the
// meteor spawns 40 m up when the mark expires, and the grunt is hurt when it
// lands. The bystander is not.
void meteor_staff_lands_on_the_target(const Catalog& catalog) {
    Arena arena;
    open_arena(catalog, kMeteorStaff, 7891, &arena);
    const KernelServerEntityState target = entity_state(arena.kernel, arena.target);
    const std::uint16_t bystander_hp =
        entity_state(arena.kernel, arena.bystander).hp;
    require(arena.ammo() == 3u);

    arena.fire(kMeteorStaff, KernelVec3{
        target.position.x, target.position.y + 0.8f, target.position.z});
    int mark_tick = -1;
    int fall_tick = -1;
    int hit_tick = -1;
    KernelVec3 mark{};
    for (int tick = 1; tick <= 60 && hit_tick < 0; ++tick) {
        arena.tick();
        for (const KernelServerEntityState& projectile : projectiles(arena.kernel)) {
            if (mark_tick < 0 && speed(projectile) < 1e-3f) {
                mark_tick = tick;
                mark = projectile.position;
            }
            if (fall_tick < 0 && projectile.velocity.y < -10.0f) {
                fall_tick = tick;
                require(projectile.position.y > arena.ground + 30.0f);
            }
        }
        if (entity_state(arena.kernel, arena.target).hp < target.hp) {
            hit_tick = tick;
        }
    }
    std::printf(
        "meteor_staff: mark +%d ticks, fall +%d, damage +%d (from press)\n",
        mark_tick, fall_tick, hit_tick);
    require(mark_tick >= 1 && mark_tick <= 2);
    require(horizontal_distance(mark, target.position) < 0.6f);
    require(std::fabs(mark.y - arena.ground) < 0.3f);
    require(fall_tick - mark_tick >= 19 && fall_tick - mark_tick <= 21);
    require(hit_tick - fall_tick >= 14 && hit_tick - fall_tick <= 17);
    require(arena.ammo() == 2u);
    require(entity_state(arena.kernel, arena.bystander).hp == bystander_hp);
}

// Aimed at the sky: no mark, no charge spent.
void meteor_staff_refuses_the_sky(const Catalog& catalog) {
    Arena arena;
    open_arena(catalog, kMeteorStaff, 7892, &arena);
    const KernelVec3 from = arena.launch_point();
    arena.fire(kMeteorStaff, KernelVec3{from.x + 2.0f, from.y + 50.0f, from.z});
    arena.tick(5);
    require(projectiles(arena.kernel).empty());
    require(arena.ammo() == 3u);
}

// Meteor Storm Staff at the ground by the grunt: 10-15 fuses around the aim
// point, as many meteors, the grunt hit more than once, the bystander never.
void meteor_storm_scatters_around_the_aim(const Catalog& catalog) {
    Arena arena;
    open_arena(catalog, kMeteorStormStaff, 7893, &arena);
    const KernelServerEntityState target = entity_state(arena.kernel, arena.target);
    const std::uint16_t bystander_hp =
        entity_state(arena.kernel, arena.bystander).hp;
    const KernelVec3 aim{target.position.x - 1.0f, arena.ground, target.position.z};
    arena.fire(kMeteorStormStaff, aim);

    std::size_t most_resting = 0;
    std::vector<std::uint32_t> meteors;
    int hits = 0;
    std::uint16_t hp = target.hp;
    float farthest_mark = 0.0f;
    for (int tick = 1; tick <= 120; ++tick) {
        arena.tick();
        std::size_t resting = 0;
        for (const KernelServerEntityState& projectile : projectiles(arena.kernel)) {
            if (speed(projectile) < 1e-3f) {
                ++resting;
                farthest_mark = std::max(
                    farthest_mark, horizontal_distance(projectile.position, aim));
            } else if (projectile.velocity.y < -10.0f &&
                       std::find(meteors.begin(), meteors.end(),
                                 projectile.net_id) == meteors.end()) {
                meteors.push_back(projectile.net_id);
            }
        }
        // Between the storm mark expiring (10) and the first fuse (20) only
        // fuses rest on the ground.
        if (tick >= 12 && tick <= 19) {
            most_resting = std::max(most_resting, resting);
        }
        const std::uint16_t now = entity_state(arena.kernel, arena.target).hp;
        if (now < hp) {
            ++hits;
            hp = now;
        }
    }
    std::printf(
        "meteor_storm: %zu fuses, %zu meteors, farthest mark %.2f m, "
        "grunt hit %d times (%u -> %u hp)\n",
        most_resting, meteors.size(), farthest_mark, hits,
        static_cast<unsigned>(target.hp), static_cast<unsigned>(hp));
    require(most_resting >= 10 && most_resting <= 15);
    require(meteors.size() == most_resting);
    require(farthest_mark <= 6.3f);
    require(hits >= 1);
    require(entity_state(arena.kernel, arena.bystander).hp == bystander_hp);
}

// Sky Laser at the grunt: half a second of warning, a two-tick fall, then a
// burn that hits it again and again.
void sky_laser_burns_the_target(const Catalog& catalog) {
    Arena arena;
    open_arena(catalog, kSkyLaser, 7894, &arena);
    const KernelServerEntityState target = entity_state(arena.kernel, arena.target);
    arena.fire(kSkyLaser, KernelVec3{
        target.position.x, target.position.y + 0.8f, target.position.z});
    int first_hit = -1;
    int hits = 0;
    std::uint16_t hp = target.hp;
    for (int tick = 1; tick <= 70; ++tick) {
        arena.tick();
        const std::uint16_t now = entity_state(arena.kernel, arena.target).hp;
        if (now < hp) {
            if (first_hit < 0) {
                first_hit = tick;
            }
            ++hits;
            hp = now;
        }
    }
    std::printf(
        "sky_laser: first damage +%d ticks, %d hits (%u -> %u hp)\n",
        first_hit, hits, static_cast<unsigned>(target.hp),
        static_cast<unsigned>(hp));
    require(first_hit >= 16 && first_hit <= 20);
    require(hits >= 5);
}

}  // namespace

int main() {
    const Catalog catalog = load_catalog();
    meteor_staff_lands_on_the_target(catalog);
    meteor_staff_refuses_the_sky(catalog);
    meteor_storm_scatters_around_the_aim(catalog);
    sky_laser_burns_the_target(catalog);
    std::printf("targeted_strike_catalog_test passed\n");
    return 0;
}
