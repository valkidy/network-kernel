// The shipped tornado (projectile 29), driven end to end on a dedicated server
// with the shipped catalog and static scene. It has no weapon yet, so the
// player's Fire Floor is pointed at it.
//
// What is pinned is the design: one entity for the tornado's whole life (the
// periodic pull is its own overlap, nothing is spawned per pull), a hostile
// in reach is pulled -- lifted off the ground -- and not hurt, the bystander
// out of reach is untouched, and the funnel rides the ground at its hover.
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
// Fire Floor: the catalog's weapon that casts an area effect. The tornado has
// no weapon of its own yet, so this test points Fire Floor at it.
constexpr std::uint8_t kFireFloor = 4;
constexpr std::uint16_t kPort = 8030;

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

const network_example::game_server::ProjectileTemplateConfig* find_projectile(
    const Catalog& catalog,
    const char* name) {
    for (const auto& projectile : catalog.config.projectile_templates) {
        if (projectile.name == name) {
            return &projectile;
        }
    }
    return nullptr;
}

void the_shipped_tornado_is_a_ground_following_column() {
    const Catalog catalog = load_catalog();
    const auto* tornado = find_projectile(catalog, "tornado");
    require(tornado != nullptr);
    require(tornado->definition.projectile_template_id == 29u);
    const KernelProjectileMechanicsDefinition& mechanics =
        tornado->definition.mechanics;
    require(mechanics.sync_mode ==
            KernelProjectileSyncMode_LocalPredictedDeterministic);
    require(mechanics.area_effect.shape == KernelAreaEffectShape_Cylinder);
    require(mechanics.area_effect.motion == KernelAreaEffectMotion_GroundFollow);
    // From the ground to 17 m: the funnel's 2 m hover plus the column's 15 m
    // half height, which reaches a hive_airship's hull (14 to 17 m).
    require(mechanics.area_effect.half_height == 15.0f);
    require(mechanics.area_effect.hover_height + mechanics.area_effect.half_height >=
            17.0f);
    require(mechanics.area_effect.motion_collision_mask ==
            KERNEL_COLLISION_LAYER_TERRAIN);
    require(mechanics.area_effect.damage_interval_ticks == 15u);
    require(mechanics.area_effect.lifetime_ticks == 300u);
    require(tornado->projectile_impact_trigger.action_graph_ref ==
            "action_tornado_pull");
}

// The shipped scene and catalog, a player whose fire floor throws a tornado,
// and whatever `spawn_targets` places, all settled on the ground.
template <typename SpawnTargets>
void open_arena(
    Arena& arena,
    const Catalog& catalog,
    std::uint16_t port,
    SpawnTargets spawn_targets) {
    const auto* tornado = find_projectile(catalog, "tornado");
    require(tornado != nullptr);

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    config.max_events = 4096;
    config.max_render_states = 256;
    arena.kernel = Kernel_Create(&config);
    require(arena.kernel != nullptr);
    KernelStaticCollisionSceneConfig scene{};
    scene.struct_size = sizeof(scene);
    scene.artifact_bytes = catalog.scene_bytes.data();
    scene.artifact_size = static_cast<std::uint32_t>(catalog.scene_bytes.size());
    scene.scene_id = catalog.config.static_collision_scene.scene_id;
    scene.collider_id = catalog.config.static_collision_scene.collider_id;
    scene.collision_layer = catalog.config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena.kernel, &scene));
    require(Kernel_StartDedicatedServer(arena.kernel, port));
    // The kernel's own validation of the new mechanics runs here.
    require(Kernel_LoadGameplayCatalog(
        arena.kernel, &catalog.storage.definition, nullptr));

    arena.player = spawn_actor(
        arena.kernel, catalog.config.player.actor_template_id,
        network_example::game_server::kActorTypePlayer,
        KernelVec3{0.0f, 1.0f, 0.0f});
    spawn_targets(arena);

    require(catalog.config.weapons.configured[kFireFloor]);
    KernelWeaponMechanicsDefinition mechanics =
        catalog.config.weapons.definitions[kFireFloor];
    mechanics.projectile_template_id = tornado->definition.projectile_template_id;
    require(Kernel_ServerSetEntityWeaponMechanics(
        arena.kernel, arena.player, &mechanics));
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
    combat.weapon_ids[0] = kFireFloor;
    combat.ammo[0] = mechanics.magazine_size;
    combat.reserve_magazines[0] = 2;
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));

    arena.tick(30);
    const KernelServerEntityState settled = entity_state(arena.kernel, arena.player);
    require(std::fabs(settled.velocity.y) < 0.05f);
    arena.ground = settled.position.y;
    require(projectiles(arena.kernel).empty());
}

void the_tornado_pulls_what_it_reaches_without_hurting_it() {
    const Catalog catalog = load_catalog();
    Arena arena;
    open_arena(arena, catalog, kPort, [&](Arena& field) {
        const network_example::game_server::ActorTemplateConfig* grunt = nullptr;
        for (const auto& candidate : catalog.config.actor_templates) {
            if (candidate.name == "chaser_grunt") {
                grunt = &candidate;
            }
        }
        require(grunt != nullptr);
        field.target = spawn_actor(
            field.kernel, grunt->actor_template_id,
            network_example::game_server::kActorTypeAgent,
            KernelVec3{8.0f, 1.0f, 0.0f});
        field.bystander = spawn_actor(
            field.kernel, grunt->actor_template_id,
            network_example::game_server::kActorTypeAgent,
            KernelVec3{0.0f, 1.0f, 30.0f});
    });

    const std::uint16_t target_hp = entity_state(arena.kernel, arena.target).hp;
    const std::uint16_t bystander_hp = entity_state(arena.kernel, arena.bystander).hp;
    const float bystander_y = entity_state(arena.kernel, arena.bystander).position.y;

    const KernelServerEntityState target = entity_state(arena.kernel, arena.target);
    arena.fire(kFireFloor, KernelVec3{target.position.x, arena.ground + 1.0f, 0.0f});

    float target_highest = -1000.0f;
    float bystander_highest = -1000.0f;
    std::uint32_t tornado_net_id = 0;
    float previous_x = -1000.0f;
    int ticks_alive = 0;
    for (int tick = 1; tick <= 120; ++tick) {
        arena.tick();
        const std::vector<KernelServerEntityState> live = projectiles(arena.kernel);
        if (live.empty()) {
            require(tornado_net_id == 0u);
            continue;
        }
        // One entity, the same one, every tick it is alive.
        require(live.size() == 1u);
        if (tornado_net_id == 0u) {
            tornado_net_id = live.front().net_id;
        }
        require(live.front().net_id == tornado_net_id);
        ++ticks_alive;
        // Riding the shipped ground at its 2 m hover, and moving out.
        require(std::fabs(live.front().position.y - (arena.ground + 2.0f)) < 0.1f);
        require(live.front().position.x >= previous_x);
        previous_x = live.front().position.x;
        // The collider query reports the column the overlap runs -- upright,
        // centred on the funnel, the collider's radius and half height -- not
        // a sphere the overlap never was.
        KernelColliderShapeQuery shape_query{};
        shape_query.struct_size = sizeof(shape_query);
        shape_query.entity_net_id = tornado_net_id;
        KernelColliderShapeView shape{};
        require(Kernel_QueryColliderShapes(arena.kernel, &shape_query, &shape, 1) == 1u);
        require(shape.shape_type == KernelColliderShapeType_Cylinder);
        require(shape.shape_params.x == 15.0f && shape.shape_params.y == 3.0f);
        require(std::fabs(shape.world_center.y - live.front().position.y) < 0.001f);
        require(shape.world_rotation.w == 1.0f);

        target_highest = std::max(
            target_highest, entity_state(arena.kernel, arena.target).position.y);
        bystander_highest = std::max(
            bystander_highest,
            entity_state(arena.kernel, arena.bystander).position.y);
    }
    std::printf(
        "tornado: alive %d ticks, reached x = %.2f; target rose %.2f m\n",
        ticks_alive,
        previous_x,
        target_highest - arena.ground);
    require(tornado_net_id != 0u);
    require(ticks_alive >= 100);
    // Pulled: apply_pull lifts what it moves, and nothing else here leaves
    // the ground.
    require(target_highest > arena.ground + 0.3f);
    require(bystander_highest < bystander_y + 0.05f);
    // And not hurt -- the tornado deals no damage of its own, its graph none.
    require(entity_state(arena.kernel, arena.target).hp == target_hp);
    require(entity_state(arena.kernel, arena.bystander).hp == bystander_hp);
}

// The column reaches a hive_airship hovering 14 m up, and pulls it: across,
// toward the funnel, at the height it holds -- a hover never takes the
// vertical part of a launch. Nothing about the response is special-cased; it
// is what the pull and the hover controller compute.
void the_tornado_reaches_a_hovering_airship() {
    const Catalog catalog = load_catalog();
    std::uint32_t airship_template = 0;
    for (const auto& candidate : catalog.config.actor_templates) {
        if (candidate.name == "hive_airship") {
            airship_template = candidate.actor_template_id;
        }
    }
    require(airship_template != 0u);

    Arena arena;
    open_arena(arena, catalog, kPort + 1, [&](Arena& field) {
        field.target = spawn_actor(
            field.kernel, airship_template,
            network_example::game_server::kActorTypeAgent,
            KernelVec3{8.0f, 15.0f, 0.0f});
    });
    // Let it find its altitude.
    arena.tick(150);
    const KernelServerEntityState hovering = entity_state(arena.kernel, arena.target);
    const float altitude = hovering.position.y - arena.ground;
    require(altitude > 12.0f);
    const std::uint16_t airship_hp = hovering.hp;

    arena.fire(kFireFloor, KernelVec3{hovering.position.x, arena.ground + 1.0f, 0.0f});
    float lowest = 1000.0f;
    float highest = -1000.0f;
    float moved = 0.0f;
    for (int tick = 1; tick <= 120; ++tick) {
        arena.tick();
        const KernelServerEntityState state = entity_state(arena.kernel, arena.target);
        lowest = std::min(lowest, state.position.y);
        highest = std::max(highest, state.position.y);
        moved = std::max(moved, horizontal_distance(state.position, hovering.position));
    }
    std::printf(
        "airship: altitude %.2f m, pulled %.2f m across, height %.2f..%.2f\n",
        altitude,
        moved,
        lowest - arena.ground,
        highest - arena.ground);
    // Pulled across...
    require(moved > 0.5f);
    // ...at the height it holds, and not hurt.
    require(highest - lowest < 1.0f);
    require(entity_state(arena.kernel, arena.target).hp == airship_hp);
}

}  // namespace

int main() {
    the_shipped_tornado_is_a_ground_following_column();
    the_tornado_pulls_what_it_reaches_without_hurting_it();
    the_tornado_reaches_a_hovering_airship();
    return 0;
}
