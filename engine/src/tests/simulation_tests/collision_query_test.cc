#include <cassert>
#include <string>

#include "physics/public/physics_world.h"

namespace {

network_example::physics::CollisionObjectDescriptor actor(
    std::uint32_t entity_net_id,
    std::uint32_t collider_id,
    float x) {
    using namespace network_example::physics;
    CollisionObjectDescriptor object{};
    object.identity.entity_net_id = entity_net_id;
    object.identity.collider_id = collider_id;
    object.identity.kind = CollisionObjectKind::kActorHitbox;
    object.identity.layer = CollisionLayer::kDamageable;
    object.shape.type = CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3(0.5f);
    object.position = glm::vec3(x, 0.0f, 0.0f);
    return object;
}

}  // namespace

int main() {
    using namespace network_example::physics;
    PhysicsWorld world;
    assert(world.valid());
    std::string error;
    assert(world.upsert_object(actor(2, 20, 5.0f), &error));
    assert(world.upsert_object(actor(1, 10, 5.0f), &error));

    RayCastRequest ray{};
    ray.origin = glm::vec3(0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 10.0f;
    const auto hits = world.ray_cast_all(ray);
    assert(hits.size() == 2);
    assert(hits[0].identity.entity_net_id == 1);
    assert(hits[1].identity.entity_net_id == 2);

    ray.filter.ignored_entity_net_id = 1;
    CollisionHit closest{};
    assert(world.ray_cast_closest(ray, &closest));
    assert(closest.identity.entity_net_id == 2);

    OverlapRequest overlap{};
    overlap.shape.type = CollisionShapeType::kSphere;
    overlap.shape.radius = 1.0f;
    overlap.position = glm::vec3(5.0f, 0.0f, 0.0f);
    overlap.filter.ignored_collider_id = 20;
    const auto overlaps = world.overlap_all(overlap);
    assert(overlaps.size() == 1);
    assert(overlaps[0].identity.collider_id == 10);
    return 0;
}
