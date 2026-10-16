// A prop with a trigger volume is set down where the volume is clear too
// (docs/PORTAL_PLAN.md D16), not only its hit volume: a portal whose gate
// stands in a wall cannot be walked into.
//
// The portal's shape: a thin hit volume (the frame) on the throw side of the
// origin and a trigger volume (the gate) on the thrower's side, the way it
// lands when thrown along +z. A tall block stands just in front of the gate,
// clear of the frame. With the trigger volume, placement backs off the way the
// throw came until the gate is clear of the block; without it -- the control,
// and what placement did before -- it stays where the bottle landed, the gate
// inside the block.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"
#include "world/public/components.h"
#include "world/public/world.h"

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

void add_box(
    ne::physics::PhysicsWorld& physics,
    std::uint32_t collider_id,
    ne::physics::CollisionObjectKind kind,
    ne::physics::CollisionLayer layer,
    const glm::vec3& center,
    const glm::vec3& half_extents) {
    ne::physics::CollisionObjectDescriptor box;
    box.identity = ne::physics::CollisionObjectIdentity{
        0, collider_id, ne::physics::kHitZoneUnscaled, kind, layer};
    box.shape.type = ne::physics::CollisionShapeType::kBox;
    box.shape.half_extents = half_extents;
    box.position = center;
    std::string error;
    require(physics.upsert_object(box, &error));
}

ne::ColliderInstance box_collider(
    ne::NetId net_id,
    std::uint32_t template_id,
    std::uint32_t purpose,
    const glm::vec3& local_center,
    const glm::vec3& half_extents) {
    ne::ColliderInstance collider{};
    collider.collider_template_id = template_id;
    collider.owner_net_id = net_id;
    collider.entity_net_id = net_id;
    collider.entity_type = ne::EntityType::kProp;
    collider.shape_type = ne::ColliderShapeType::kOrientedBox;
    collider.purpose_flags = purpose;
    collider.local_center = local_center;
    collider.world_center = local_center;
    collider.half_extents = half_extents;
    return collider;
}

// Where placement sets the portal down, with or without its gate.
std::optional<glm::vec3> place(bool with_gate) {
    ne::physics::PhysicsWorld physics;
    require(physics.valid());
    add_box(physics, 1, ne::physics::CollisionObjectKind::kTerrain,
            ne::physics::CollisionLayer::kTerrain,
            glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{20.0f, 0.5f, 20.0f});
    // Three metres tall, so nothing is set down on top of it: z -0.6 to -0.3.
    add_box(physics, 2, ne::physics::CollisionObjectKind::kStaticObstacle,
            ne::physics::CollisionLayer::kStaticObstacle,
            glm::vec3{0.0f, 1.5f, -0.45f}, glm::vec3{0.5f, 1.5f, 0.15f});
    physics.optimize_broad_phase();

    ne::World world;
    world.set_collision_world(&physics);
    const ne::NetId portal = world.spawn_entity(
        ne::EntityType::kProp, ne::ActorType::kUnknown, 0, glm::vec3{0.0f});
    // The frame: z 0 to 0.3, clear of the block.
    world.collider_registry().upsert_entity_collider(
        portal, 900,
        box_collider(portal, 900, KernelColliderPurpose_Hit,
                     glm::vec3{0.0f, 1.3f, 0.15f}, glm::vec3{1.0f, 1.3f, 0.15f}));
    if (with_gate) {
        // The gate: z -0.4 to 0.1, its front 0.1 m into the block.
        world.collider_registry().upsert_entity_collider(
            portal, 901,
            box_collider(portal, 901, KernelColliderPurpose_Trigger,
                         glm::vec3{0.0f, 1.275f, -0.15f},
                         glm::vec3{0.9f, 1.275f, 0.25f}));
    }
    return ne::find_clear_spawn_spot(
        world, portal, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0);
}

// A portal landing where another's gate stands, clear of everything physical:
// no wall, only the other gate -- which is no body, so only the trigger-volume
// check sees it. Its gate reaches z 0.1 to 0.3, into the new frame (0 to 0.3).
std::optional<glm::vec3> place_by_another_gate(bool other_gate) {
    ne::physics::PhysicsWorld physics;
    require(physics.valid());
    add_box(physics, 1, ne::physics::CollisionObjectKind::kTerrain,
            ne::physics::CollisionLayer::kTerrain,
            glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{20.0f, 0.5f, 20.0f});
    physics.optimize_broad_phase();

    ne::World world;
    world.set_collision_world(&physics);
    if (other_gate) {
        const ne::NetId other = world.spawn_entity(
            ne::EntityType::kProp, ne::ActorType::kUnknown, 0, glm::vec3{0.0f});
        ne::ColliderInstance gate = box_collider(
            other, 901, KernelColliderPurpose_Trigger,
            glm::vec3{0.0f, 1.275f, 0.2f}, glm::vec3{0.9f, 1.275f, 0.1f});
        gate.world_bounds = ne::ColliderWorldBounds{gate.world_center, gate.half_extents};
        world.collider_registry().upsert_entity_collider(other, 901, gate);
    }
    const ne::NetId portal = world.spawn_entity(
        ne::EntityType::kProp, ne::ActorType::kUnknown, 0, glm::vec3{0.0f});
    world.collider_registry().upsert_entity_collider(
        portal, 900,
        box_collider(portal, 900, KernelColliderPurpose_Hit,
                     glm::vec3{0.0f, 1.3f, 0.15f}, glm::vec3{1.0f, 1.3f, 0.15f}));
    world.collider_registry().upsert_entity_collider(
        portal, 901,
        box_collider(portal, 901, KernelColliderPurpose_Trigger,
                     glm::vec3{0.0f, 1.275f, -0.15f},
                     glm::vec3{0.9f, 1.275f, 0.25f}));
    return ne::find_clear_spawn_spot(
        world, portal, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0);
}

// Exits on a slope (docs/PORTAL_PLAN.md, 2026-10-16): the ground under the
// destination is 0.5 m up and rises 10 degrees toward +x, while the template's
// destination says y = 0. Exits used to keep that y -- half buried, every slot
// "blocked" by the terrain, both exits and their arrivals stacked under the
// ground. Now each is set on the ground.
constexpr float kSlopeDegrees = 10.0f;
constexpr float kGroundAtDestination = 0.5f;

float slope_height(float x) {
    return kGroundAtDestination + std::tan(glm::radians(kSlopeDegrees)) * (x - 50.0f);
}

struct Slope {
    ne::physics::PhysicsWorld physics;
    ne::World world;

    Slope() {
        require(physics.valid());
        // A thin slab tilted about z, its top through (50, 0.5) at 10 degrees.
        const glm::quat tilt = glm::angleAxis(glm::radians(kSlopeDegrees), glm::vec3{0.0f, 0.0f, 1.0f});
        ne::physics::CollisionObjectDescriptor slab;
        slab.identity = ne::physics::CollisionObjectIdentity{
            0, 1, ne::physics::kHitZoneUnscaled,
            ne::physics::CollisionObjectKind::kTerrain,
            ne::physics::CollisionLayer::kTerrain};
        slab.shape.type = ne::physics::CollisionShapeType::kBox;
        slab.shape.half_extents = glm::vec3{40.0f, 0.5f, 20.0f};
        slab.position = glm::vec3{50.0f, kGroundAtDestination, 50.0f} - tilt * glm::vec3{0.0f, 0.5f, 0.0f};
        slab.rotation = tilt;
        std::string error;
        require(physics.upsert_object(slab, &error));
        physics.optimize_broad_phase();
        world.set_collision_world(&physics);
    }

    // An exit: a 2 m trigger-only footprint standing on its origin.
    ne::NetId exit() {
        const ne::NetId id = world.spawn_entity(
            ne::EntityType::kProp, ne::ActorType::kUnknown, 0, glm::vec3{50.0f, 0.0f, 50.0f});
        world.collider_registry().upsert_entity_collider(
            id, 950,
            box_collider(id, 950, KernelColliderPurpose_Trigger,
                         glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{1.0f, 1.0f, 1.0f}));
        return id;
    }

    // Where the kernel moves an exit once placed: its footprint follows.
    void stand(ne::NetId id, const glm::vec3& at) {
        for (ne::ColliderInstance& collider : world.collider_registry().mutable_instances()) {
            if (collider.entity_net_id == id) {
                collider.world_center = at + collider.local_center;
                collider.world_bounds = ne::ColliderWorldBounds{collider.world_center, collider.half_extents};
            }
        }
    }
};

void exits_stand_on_a_slope() {
    const glm::vec3 destination{50.0f, 0.0f, 50.0f};
    const glm::vec3 forward{0.0f, 0.0f, 1.0f};
    Slope slope;

    // The first rests on the highest ground under its footprint: its uphill
    // edge, x = 51.
    const ne::NetId first = slope.exit();
    const std::optional<glm::vec3> a = ne::find_clear_exit_spot(slope.world, first, destination, forward);
    require(a.has_value());
    std::printf("first exit at (%.2f, %.3f, %.2f), ground under its middle %.3f\n",
                a->x, a->y, a->z, slope_height(a->x));
    require(std::abs(a->x - 50.0f) < 0.01f && std::abs(a->z - 50.0f) < 0.01f);
    require(std::abs(a->y - slope_height(51.0f)) < 0.05f);
    slope.stand(first, *a);

    // The second takes the next slot across the forward, x = 52.5, also on
    // the ground there.
    const ne::NetId second = slope.exit();
    const std::optional<glm::vec3> b = ne::find_clear_exit_spot(slope.world, second, destination, forward);
    require(b.has_value());
    std::printf("second exit at (%.2f, %.3f, %.2f)\n", b->x, b->y, b->z);
    require(std::abs(b->x - 52.5f) < 0.01f);
    require(std::abs(b->y - slope_height(53.5f)) < 0.05f);

    // Every slot covered: still on the ground under the destination.
    const ne::NetId blocker = slope.world.spawn_entity(
        ne::EntityType::kProp, ne::ActorType::kUnknown, 0, destination);
    ne::ColliderInstance wide = box_collider(blocker, 951, KernelColliderPurpose_Trigger,
                                             glm::vec3{0.0f}, glm::vec3{40.0f, 10.0f, 3.0f});
    wide.world_bounds = ne::ColliderWorldBounds{destination, wide.half_extents};
    slope.world.collider_registry().upsert_entity_collider(blocker, 951, wide);
    const ne::NetId third = slope.exit();
    const std::optional<glm::vec3> c = ne::find_clear_exit_spot(slope.world, third, destination, forward);
    require(c.has_value());
    std::printf("no slot clear: (%.2f, %.3f, %.2f)\n", c->x, c->y, c->z);
    require(std::abs(c->x - 50.0f) < 0.01f && std::abs(c->y - a->y) < 0.01f);

    // A drop portal keeps its authored height: not set on the ground.
    Slope drop;
    const ne::NetId dropping = drop.exit();
    const std::optional<glm::vec3> d = ne::find_clear_exit_spot(
        drop.world, dropping, glm::vec3{50.0f, 20.0f, 50.0f}, glm::vec3{0.0f, -1.0f, 0.0f});
    require(d.has_value());
    require(std::abs(d->y - 20.0f) < 0.01f);
}

}  // namespace

int main() {
    // Control: the frame alone fits where the bottle landed.
    const std::optional<glm::vec3> without_gate = place(false);
    require(without_gate.has_value());
    std::printf("frame only: set down at z %.2f\n", without_gate->z);
    require(std::abs(without_gate->z) < 0.01f);

    // The gate in the block moves it back past the block: 0.5 m in, the frame
    // would stand in the block too; 1 m in, both are clear.
    const std::optional<glm::vec3> with_gate = place(true);
    require(with_gate.has_value());
    std::printf("frame and gate: set down at z %.2f\n", with_gate->z);
    require(std::abs(with_gate->z + 1.0f) < 0.01f);
    require(std::abs(with_gate->y) < 0.01f);

    // Control: alone, it lands where it struck.
    const std::optional<glm::vec3> alone = place_by_another_gate(false);
    require(alone.has_value());
    require(std::abs(alone->z) < 0.01f);
    // Another portal's gate there: it backs off one step, clear of that gate.
    const std::optional<glm::vec3> by_gate = place_by_another_gate(true);
    require(by_gate.has_value());
    std::printf("by another portal's gate: set down at z %.2f\n", by_gate->z);
    require(std::abs(by_gate->z + 0.5f) < 0.01f);

    exits_stand_on_a_slope();

    std::printf("trigger_volume_placement_test: PASS\n");
    return 0;
}
