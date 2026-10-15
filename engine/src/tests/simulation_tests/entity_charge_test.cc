// Kernel_ServerEnqueueEntityCharge: an agent's own charge, held by a
// ChargeLockout.
//
// The claims: the run holds its speed and line whatever the controller steers;
// it ends on its tick count; the static world stops it and staggers the
// charger; another actor's body stops it without a stagger; a stagger stops it
// dead; its contact strikes what it runs into once per charge and ends it; and
// what is not a live grounded agent carrying a melee weapon is refused.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectDescriptor;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::CollisionShapeType;
using network_example::physics::PhysicsWorld;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "entity_charge_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr std::uint8_t kChargeWeapon = 3;

void give_charge_weapon(World& world, entt::entity entity) {
    WeaponTuning& tuning = world.registry().get_or_emplace<WeaponTuning>(entity);
    tuning.configured[kChargeWeapon] = true;
    WeaponMechanicsDefinition& horns = tuning.definitions[kChargeWeapon];
    horns.id = kChargeWeapon;
    horns.mode = WeaponFireMode::kMelee;
    horns.magazine_size = 1;
    horns.damage = 30;
    horns.collision_mask = kCollisionMaskDamageable;
    horns.melee_range = 1.2f;
    horns.melee_fov_degrees = 120.0f;
}

// An agent on a 100 m slab whose top is y = 0, driven by the real movement
// solver, as entity_leap_test's is. The arming tick is `tick`, and its
// movement step is the run's first.
struct Fixture {
    explicit Fixture(glm::vec3 spawn, bool player = false)
        : engine(KernelConfig{}), world(engine.simulation_world()) {
        world.set_collision_world(&physics);
        add_box(100, glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{50.0f, 0.5f, 50.0f});
        actor = player ? world.spawn_player(7, spawn) : world.spawn_enemy(spawn);
        entity = *world.find_entity(actor);
        world.registry().get_or_emplace<Health>(entity) = Health{50, 50};
        world.registry().emplace_or_replace<StaggerProfile>(
            entity, StaggerProfile{150.0f, 1.0f, 5.0f, 20u, 15u, 45u});
        give_charge_weapon(world, entity);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 2.0f;
        movement.controller_type = MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

        ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = actor;
        collider.entity_net_id = actor;
        collider.entity_type = EntityType::kActor;
        collider.actor_type = player ? ActorType::kPlayer : ActorType::kAgent;
        collider.shape_type = ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
        collider.world_center = spawn + collider.local_center;
        collider.radius = 0.35f;
        collider.capsule_half_height = 0.55f;
        const ColliderInstance& stored =
            world.collider_registry().upsert_entity_collider(actor, 10, collider);
        movement.movement_collider_id = stored.collider_id;
        movement_collider_id = stored.collider_id;
        sync_body();
        tick = engine.current_tick() - 8u;
        for (int i = 0; i < 8; ++i) step(glm::vec2{0.0f});
        require(tick == engine.current_tick());
        require(grounded());
    }

    void add_box(std::uint32_t id, glm::vec3 center, glm::vec3 half_extents) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            0, id, 0,
            CollisionObjectKind::kStaticObstacle,
            CollisionLayer::kStaticObstacle,
        };
        object.shape.type = CollisionShapeType::kBox;
        object.shape.half_extents = half_extents;
        object.position = center;
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    // Another actor's movement capsule standing at `feet`, and nothing else:
    // a body in the way. Its collider id must differ from the charger's: the
    // movement filter ignores the charger's own collider id outright, so a
    // body sharing it would be walked straight through.
    void add_body(NetId net_id, glm::vec3 feet) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            net_id, 777, 0,
            CollisionObjectKind::kActorMovement,
            CollisionLayer::kActorMovement,
        };
        object.shape.type = CollisionShapeType::kCapsule;
        object.shape.radius = 0.35f;
        object.shape.capsule_half_height = 0.55f;
        object.position = feet + glm::vec3{0.0f, 0.9f, 0.0f};
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    void sync_body() {
        const Transform& transform = world.registry().get<Transform>(entity);
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            actor, movement_collider_id, 0,
            CollisionObjectKind::kActorMovement,
            CollisionLayer::kActorMovement,
        };
        object.shape.type = CollisionShapeType::kCapsule;
        object.shape.radius = 0.35f;
        object.shape.capsule_half_height = 0.55f;
        object.position = transform.position + glm::vec3{0.0f, 0.9f, 0.0f};
        std::string error;
        require(physics.upsert_object(object, &error));
    }

    // One movement step on `tick`, the controller holding `move`, as a chaser
    // does every tick.
    void step(const glm::vec2& move) {
        KernelPlayerInput input{};
        input.move = KernelVec2{move.x, move.y};
        const std::vector<QueuedInput> inputs{
            QueuedInput{7, input, tick, 0, false, actor},
        };
        simulate_actor_movement(world, inputs, kTick, tick, &events, &stats);
        sync_body();
        ++tick;
    }

    bool charge(glm::vec3 direction, float speed, std::uint32_t ticks,
                std::uint8_t weapon = kChargeWeapon) {
        return EntityStateSystem{}.start_charge(
            engine, actor, KernelVec3{direction.x, direction.y, direction.z},
            speed, ticks, weapon);
    }

    bool charging() const {
        return world.registry().all_of<ChargeLockout>(entity);
    }

    bool staggered() const {
        return is_staggered(world, entity, tick);
    }

    glm::vec3 position() const {
        return world.registry().get<Transform>(entity).position;
    }

    bool grounded() const {
        return world.registry().get<MovementState>(entity).ground_state ==
            MovementState::GroundState::kGrounded;
    }

    PhysicsWorld physics;
    KernelEngine engine;
    World& world;
    NetId actor = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
    std::uint32_t tick = 0;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

// ---------------------------------------------------------------------------

// 8 m/s for 10 steps is 2.67 m along +x, while the controller pushes the other
// way at its own 2 m/s the whole time. The step after the last one is the
// controller's again.
void holds_its_line_and_speed_then_hands_back() {
    Fixture f(glm::vec3{0.0f});
    require(f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
    for (int n = 0; n < 10; ++n) {
        require(f.charging());
        f.step(glm::vec2{-1.0f, 0.0f});
    }
    require(!f.charging());
    require(std::fabs(f.position().x - 8.0f * 10.0f * kTick) < 0.05f);
    require(std::fabs(f.position().z) < 0.01f);
    require(f.grounded());
    const float after_run = f.position().x;
    f.step(glm::vec2{-1.0f, 0.0f});
    require(f.position().x < after_run);
}

// A wall 2 m ahead: the run stops against it and the charger is staggered, on
// its own profile's duration.
void a_wall_stops_it_and_staggers_the_charger() {
    Fixture f(glm::vec3{0.0f});
    f.add_box(200, glm::vec3{2.5f, 1.0f, 0.0f}, glm::vec3{0.5f, 1.0f, 3.0f});
    require(f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 60u));
    int steps = 0;
    while (f.charging() && steps < 60) {
        f.step(glm::vec2{1.0f, 0.0f});
        ++steps;
    }
    require(steps < 30);
    require(!f.charging());
    require(f.position().x < 2.0f);
    require(f.staggered());
    bool event = false;
    for (const KernelEvent& e : f.events) {
        event = event || (e.type == KernelEventType_Staggered && e.net_id == f.actor);
    }
    require(event);
    // Rooted while staggered, whatever it is told.
    const glm::vec3 held = f.position();
    f.step(glm::vec2{1.0f, 0.0f});
    require(glm::length(f.position() - held) < 0.01f);
}

// Another actor's body in the way stops it too, but that is not a wall.
void a_body_stops_it_without_a_stagger() {
    Fixture f(glm::vec3{0.0f});
    f.add_body(9999, glm::vec3{2.0f, 0.0f, 0.0f});
    require(f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 60u));
    int steps = 0;
    while (f.charging() && steps < 60) {
        f.step(glm::vec2{1.0f, 0.0f});
        ++steps;
    }
    require(steps < 30);
    require(f.position().x < 1.4f);
    require(!f.staggered());
}

// Staggered mid-run, it stops dead: a stagger roots, and it ends the charge.
void a_stagger_stops_it_dead() {
    Fixture f(glm::vec3{0.0f});
    require(f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 60u));
    f.step(glm::vec2{0.0f});
    f.step(glm::vec2{0.0f});
    require(f.charging());
    require(apply_stagger(f.world, f.actor, 0u, 150.0f, 0u, f.tick - 1u, nullptr));
    const glm::vec3 held = f.position();
    f.step(glm::vec2{0.0f});
    require(!f.charging());
    require(glm::length(f.position() - held) < 0.01f);
}

// Refusals: a player, a weapon it does not carry or that is not melee, a bad
// direction or speed, an actor already charging or staggered.
void refuses_what_it_cannot_run() {
    {
        Fixture f(glm::vec3{0.0f}, true);
        require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
    }
    Fixture f(glm::vec3{0.0f});
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u, 4u));
    WeaponTuning& tuning = f.world.registry().get<WeaponTuning>(f.entity);
    tuning.definitions[kChargeWeapon].mode = WeaponFireMode::kHitscan;
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
    tuning.definitions[kChargeWeapon].mode = WeaponFireMode::kMelee;
    require(!f.charge(glm::vec3{0.0f, 1.0f, 0.0f}, 8.0f, 10u));
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 0.0f, 10u));
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 0u));
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f,
                      KERNEL_MAX_IMPULSE_LOCKOUT_TICKS + 1u));
    require(f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
    f.world.registry().remove<ChargeLockout>(f.entity);
    require(apply_stagger(f.world, f.actor, 0u, 150.0f, 0u, f.tick, nullptr));
    require(!f.charge(glm::vec3{1.0f, 0.0f, 0.0f}, 8.0f, 10u));
}

// ---------------------------------------------------------------------------
// Contact, without movement: the melee cone from where the charger stands,
// facing the way it runs.

Health& health(World& world, NetId net_id) {
    return world.registry().get<Health>(*world.find_entity(net_id));
}

NetId spawn_target(World& world, glm::vec3 position) {
    const NetId target = world.spawn_enemy(position);
    health(world, target) = Health{100, 100};
    world.registry().get<Hitbox>(*world.find_entity(target)) =
        Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    return target;
}

NetId spawn_charger(World& world) {
    const NetId charger = world.spawn_enemy(glm::vec3{0.0f});
    const entt::entity entity = *world.find_entity(charger);
    give_charge_weapon(world, entity);
    ChargeLockout charge{};
    charge.velocity = glm::vec3{8.0f, 0.0f, 0.0f};
    charge.until_tick = 100u;
    charge.weapon_id = kChargeWeapon;
    charge.instance_id = 1u;
    world.registry().emplace<ChargeLockout>(entity, charge);
    return charger;
}

void contact(World& world, std::uint32_t tick) {
    std::vector<KernelEvent> events;
    DamagePipeline pipeline;
    simulate_charge_contacts(world, tick, 0u, &pipeline, nullptr);
    pipeline.confirm_ready(world, 0, tick, &events);
}

// What it runs into is struck, and the charge ends on it. What is behind it
// is not.
void contact_strikes_ahead_and_ends_the_charge() {
    World world;
    const NetId ahead = spawn_target(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const NetId behind = spawn_target(world, glm::vec3{-1.0f, 0.0f, 0.0f});
    const NetId charger = spawn_charger(world);
    contact(world, 1u);
    require(health(world, ahead).hp == 70);
    require(health(world, behind).hp == 100);
    require(!world.registry().all_of<ChargeLockout>(*world.find_entity(charger)));
}

// Nothing in reach: no damage, and the charge runs on.
void contact_with_nothing_keeps_running() {
    World world;
    const NetId far = spawn_target(world, glm::vec3{4.0f, 0.0f, 0.0f});
    const NetId charger = spawn_charger(world);
    contact(world, 1u);
    require(health(world, far).hp == 100);
    require(world.registry().all_of<ChargeLockout>(*world.find_entity(charger)));
}

// Once per charge: a target the charge has already struck is not struck again
// on a later tick, which would otherwise happen on every tick it stays in the
// cone.
void contact_strikes_each_target_once_per_charge() {
    World world;
    const NetId ahead = spawn_target(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const NetId charger = spawn_charger(world);
    ChargeLockout& charge =
        world.registry().get<ChargeLockout>(*world.find_entity(charger));
    charge.struck.push_back(ahead);
    contact(world, 1u);
    contact(world, 2u);
    require(health(world, ahead).hp == 100);
    require(world.registry().all_of<ChargeLockout>(*world.find_entity(charger)));
}

}  // namespace

int main() {
    holds_its_line_and_speed_then_hands_back();
    a_wall_stops_it_and_staggers_the_charger();
    a_body_stops_it_without_a_stagger();
    a_stagger_stops_it_dead();
    refuses_what_it_cannot_run();
    contact_strikes_ahead_and_ends_the_charge();
    contact_with_nothing_keeps_running();
    contact_strikes_each_target_once_per_charge();
    std::puts("entity_charge_test: ok");
    return 0;
}
