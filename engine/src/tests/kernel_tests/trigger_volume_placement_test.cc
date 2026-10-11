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

    std::printf("trigger_volume_placement_test: PASS\n");
    return 0;
}
