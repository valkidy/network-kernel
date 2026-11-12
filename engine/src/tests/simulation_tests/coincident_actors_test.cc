// Two actors on the exact same spot.
//
// The movement pass steps every actor against the bodies as they stood at the
// start of the tick. Two capsules sharing one position each find the other's
// body exactly on top of themselves, and the push-out Jolt picks for that is the
// same for both: both are shoved the full overlap the same way, end the tick on
// the same spot again, and repeat. A nest that released two gingerbread onto
// one point produced exactly this -- a pair moving as one body at ~24 m/s with
// nobody asking it to move (gameplay clip of 2026-11-13, net 86/87).
//
// The claims: actors sharing a position come apart and then stay put; three
// do too; and the control -- two actors that overlap without coinciding --
// separates without being flung, which is the behaviour the fix restores.
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
#include "simulation/public/simulation.h"

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
        std::fprintf(stderr, "coincident_actors_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kRadius = 0.35f;

float horizontal(const glm::vec3& v) {
    return glm::length(glm::vec2{v.x, v.z});
}

struct Actor {
    NetId net_id = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
};

// Agents on a 100 m slab whose top is y = 0, driven by the real movement
// solver, each with a movement body synced after every pass as the kernel does.
struct Fixture {
    Fixture() : engine(KernelConfig{}), world(engine.simulation_world()) {
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
        tick = engine.current_tick();
    }

    void add(glm::vec3 feet) {
        Actor actor;
        actor.net_id = world.spawn_enemy(feet);
        actor.entity = *world.find_entity(actor.net_id);
        MovementState& movement = world.registry().get<MovementState>(actor.entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

        ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = actor.net_id;
        collider.entity_net_id = actor.net_id;
        collider.entity_type = EntityType::kActor;
        collider.actor_type = ActorType::kAgent;
        collider.shape_type = ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
        collider.world_center = feet + collider.local_center;
        collider.radius = kRadius;
        collider.capsule_half_height = 0.55f;
        const ColliderInstance& stored = world.collider_registry()
            .upsert_entity_collider(actor.net_id, 10, collider);
        movement.movement_collider_id = stored.collider_id;
        actor.movement_collider_id = stored.collider_id;
        actors.push_back(actor);
    }

    void sync_bodies() {
        for (const Actor& actor : actors) {
            const Transform& transform = world.registry().get<Transform>(actor.entity);
            CollisionObjectDescriptor object{};
            object.identity = CollisionObjectIdentity{
                actor.net_id, actor.movement_collider_id, 0,
                CollisionObjectKind::kActorMovement,
                CollisionLayer::kActorMovement,
            };
            object.shape.type = CollisionShapeType::kCapsule;
            object.shape.radius = kRadius;
            object.shape.capsule_half_height = 0.55f;
            object.position = transform.position + glm::vec3{0.0f, 0.9f, 0.0f};
            std::string error;
            require(physics.upsert_object(object, &error));
        }
    }

    // Standing still: no input, so nothing but the bodies moves anyone.
    void step(int steps) {
        for (int i = 0; i < steps; ++i) {
            sync_bodies();
            simulate_actor_movement(world, {}, kTick, tick, &events, &stats);
            ++tick;
        }
        sync_bodies();
    }

    glm::vec3 position(std::size_t index) const {
        return world.registry().get<Transform>(actors[index].entity).position;
    }

    float gap(std::size_t a, std::size_t b) const {
        return horizontal(position(a) - position(b));
    }

    PhysicsWorld physics;
    KernelEngine engine;
    World& world;
    std::vector<Actor> actors;
    std::uint32_t tick = 0;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

// The control: 0.3 m apart, overlapping by 0.4 m. They push apart and stop.
// If this one ever travels, the claims below are measuring something else.
void overlapping_actors_separate_and_stop() {
    Fixture f;
    f.add(glm::vec3{0.0f, 0.0f, 0.0f});
    f.add(glm::vec3{0.3f, 0.0f, 0.0f});
    f.step(30);
    require(f.gap(0, 1) >= 2.0f * kRadius - 0.05f);
    for (std::size_t i = 0; i < f.actors.size(); ++i) {
        std::printf("overlap: actor %zu at (%.3f, %.3f)\n", i, f.position(i).x, f.position(i).z);
        require(horizontal(f.position(i)) < 1.5f);
    }
}

void coincident_group_separates_and_stays(std::size_t count) {
    Fixture f;
    for (std::size_t i = 0; i < count; ++i) f.add(glm::vec3{2.0f, 0.0f, -3.0f});
    f.step(30);
    const glm::vec3 origin{2.0f, 0.0f, -3.0f};
    for (std::size_t i = 0; i < count; ++i) {
        std::printf("coincident x%zu: actor %zu at (%.3f, %.3f), %.2f m from the spawn\n",
            count, i, f.position(i).x, f.position(i).z, horizontal(f.position(i) - origin));
        // Without the fix the group travels ~0.8 m a tick: ~24 m by now.
        require(horizontal(f.position(i) - origin) < 1.5f);
        for (std::size_t j = i + 1; j < count; ++j) {
            require(f.gap(i, j) >= 2.0f * kRadius - 0.05f);
        }
    }
    // And they have stopped: a second of standing moves nobody.
    std::vector<glm::vec3> before;
    for (std::size_t i = 0; i < count; ++i) before.push_back(f.position(i));
    f.step(30);
    for (std::size_t i = 0; i < count; ++i) {
        require(horizontal(f.position(i) - before[i]) < 0.05f);
    }
}

}  // namespace

int main() {
    overlapping_actors_separate_and_stop();
    coincident_group_separates_and_stays(2);
    coincident_group_separates_and_stays(3);
    std::puts("coincident_actors_test: ok");
    return 0;
}
