// What a snapshot says a blocked actor is moving at.
//
// Jolt hands a character back the velocity it was asked for, whatever stopped
// it, so an agent walking into another actor's body keeps its full move speed
// in Velocity while standing still. The snapshot has to say what the step made
// good instead, or the client draws it running in place and extrapolates it
// into whatever it is pressed against (P3 in docs/DEDICATED_SERVER_DOCKER_PLAN.md:
// a melee crowd around the player read as a frozen world).
//
// The claims: a blocked agent replicates ~zero horizontal speed and is not
// flagged moving, though its simulated velocity is unchanged; a free agent
// replicates its full speed; a blocked player keeps its simulated velocity,
// which the owner's knockback replay depends on; and an actor the movement
// step skips stops replicating its last made-good motion.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"
#include "sync/public/snapshot.h"

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
        std::fprintf(stderr, "blocked_actor_velocity_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kSpeed = 2.0f;

float horizontal(const glm::vec3& v) {
    return glm::length(glm::vec2{v.x, v.z});
}

// An actor on a 100 m slab whose top is y = 0, driven by the real movement
// solver, as entity_charge_test's is.
struct Fixture {
    explicit Fixture(bool player)
        : engine(KernelConfig{}), world(engine.simulation_world()) {
        world.set_collision_world(&physics);
        CollisionObjectDescriptor slab{};
        slab.identity = CollisionObjectIdentity{
            0, 100, 0,
            CollisionObjectKind::kStaticObstacle,
            CollisionLayer::kStaticObstacle,
        };
        slab.shape.type = CollisionShapeType::kBox;
        slab.shape.half_extents = glm::vec3{50.0f, 0.5f, 50.0f};
        slab.position = glm::vec3{0.0f, -0.5f, 0.0f};
        std::string error;
        require(physics.upsert_object(slab, &error));

        actor = player ? world.spawn_player(7, glm::vec3{0.0f})
                       : world.spawn_enemy(glm::vec3{0.0f});
        entity = *world.find_entity(actor);
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = kSpeed;
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
        collider.world_center = collider.local_center;
        collider.radius = 0.35f;
        collider.capsule_half_height = 0.55f;
        const ColliderInstance& stored =
            world.collider_registry().upsert_entity_collider(actor, 10, collider);
        movement.movement_collider_id = stored.collider_id;
        movement_collider_id = stored.collider_id;
        sync_body();
        tick = engine.current_tick();
        for (int i = 0; i < 8; ++i) step(glm::vec2{0.0f});
    }

    // Another actor's movement capsule standing at `feet`.
    void add_body(glm::vec3 feet) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            9999, 777, 0,
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

    void walk_along_x(int steps) {
        for (int i = 0; i < steps; ++i) step(glm::vec2{1.0f, 0.0f});
    }

    glm::vec3 position() const {
        return world.registry().get<Transform>(entity).position;
    }

    glm::vec3 simulated_velocity() const {
        return world.registry().get<Velocity>(entity).linear;
    }

    const EntitySnapshot& snapshot_entry() {
        snapshot = build_world_snapshot(world, tick, 0, 0);
        for (const EntitySnapshot& entry : snapshot.entities) {
            if (entry.net_id == actor) return entry;
        }
        require(false);
        return snapshot.entities.front();
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
    WorldSnapshot snapshot;
};

// ---------------------------------------------------------------------------

// The control: nothing in the way, the snapshot says the full walking speed.
// Without it, a replicated speed of zero below could mean the agent was never
// walking at all.
void a_free_agent_replicates_its_speed() {
    Fixture f(false);
    f.walk_along_x(30);
    require(f.position().x > 1.5f);
    require(horizontal(f.snapshot_entry().velocity) > kSpeed * 0.9f);
    require((derived_visual_flags(f.world, f.entity) & kVisualFlagMoving) != 0u);
}

// A body 1 m ahead: the agent stops against it within a few steps and goes on
// pushing. The simulation still says 2 m/s; the snapshot says ~0 and the
// moving flag is off.
void a_blocked_agent_replicates_what_it_made_good() {
    Fixture f(false);
    f.add_body(glm::vec3{1.0f, 0.0f, 0.0f});
    f.walk_along_x(30);
    require(f.position().x < 0.4f);
    // The premise: Jolt handed the asked-for velocity back. If this ever stops
    // holding, the fix below has nothing left to do.
    require(horizontal(f.simulated_velocity()) > kSpeed * 0.9f);
    const MovementState& movement = f.world.registry().get<MovementState>(f.entity);
    require(movement.has_made_good_velocity);
    require(horizontal(movement.made_good_velocity) < 0.1f);
    require(horizontal(f.snapshot_entry().velocity) < 0.1f);
    require((derived_visual_flags(f.world, f.entity) & kVisualFlagMoving) == 0u);
}

// The same push by a player: its owner replays a knockback from the replicated
// horizontal velocity, so it keeps the simulated one.
void a_blocked_player_keeps_its_simulated_velocity() {
    Fixture f(true);
    f.add_body(glm::vec3{1.0f, 0.0f, 0.0f});
    f.walk_along_x(30);
    require(f.position().x < 0.4f);
    require(horizontal(f.snapshot_entry().velocity) > kSpeed * 0.9f);
}

// An actor the step skips -- no controller -- has no made-good motion this
// tick, and the snapshot falls back to its simulated velocity rather than
// repeating the last step's.
void a_skipped_actor_drops_its_made_good_motion() {
    Fixture f(false);
    f.add_body(glm::vec3{1.0f, 0.0f, 0.0f});
    f.walk_along_x(30);
    require(f.world.registry().get<MovementState>(f.entity).has_made_good_velocity);
    f.world.registry().get<MovementState>(f.entity).controller_type =
        MovementState::ControllerType::kNone;
    f.step(glm::vec2{1.0f, 0.0f});
    require(!f.world.registry().get<MovementState>(f.entity).has_made_good_velocity);
    require(horizontal(f.snapshot_entry().velocity) ==
            horizontal(f.simulated_velocity()));
}

}  // namespace

int main() {
    a_free_agent_replicates_its_speed();
    a_blocked_agent_replicates_what_it_made_good();
    a_blocked_player_keeps_its_simulated_velocity();
    a_skipped_actor_drops_its_made_good_motion();
    std::puts("blocked_actor_velocity_test: ok");
    return 0;
}
