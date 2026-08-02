#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "physics/public/physics_world.h"

namespace {

using network_example::physics::CollisionHit;
using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectDescriptor;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::CollisionQueryStats;
using network_example::physics::CollisionShapeDescriptor;
using network_example::physics::CollisionShapeType;
using network_example::physics::CharacterDescriptor;
using network_example::physics::CharacterGroundState;
using network_example::physics::CharacterMoveRequest;
using network_example::physics::CharacterMoveResult;
using network_example::physics::OverlapRequest;
using network_example::physics::PhysicsWorld;
using network_example::physics::PhysicsWorldConfig;
using network_example::physics::RayCastRequest;
using network_example::physics::ShapeCastRequest;

std::vector<std::uint8_t> read_bytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

CollisionObjectDescriptor actor(
    std::uint32_t entity_net_id,
    std::uint32_t collider_id,
    const glm::vec3& position,
    std::uint32_t gameplay_category) {
    CollisionObjectDescriptor object{};
    object.identity = CollisionObjectIdentity{
        entity_net_id,
        collider_id,
        7,
        CollisionObjectKind::kActorHitbox,
        CollisionLayer::kDamageable,
        0,
        gameplay_category,
    };
    object.shape.type = CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3(0.5f);
    object.position = position;
    return object;
}

void populate(PhysicsWorld* world) {
    std::string error;
    assert(world->upsert_object(actor(
        2,
        20,
        glm::vec3(5.0f, 2.0f, 0.0f),
        network_example::physics::kGameplayCategoryHostileSide), &error));
    assert(world->upsert_object(actor(
        1,
        10,
        glm::vec3(5.0f, 2.0f, 0.0f),
        network_example::physics::kGameplayCategoryPlayerSide), &error));
    assert(world->upsert_object(actor(
        4,
        40,
        glm::vec3(5.0f, 2.0f, 0.0f),
        network_example::physics::kGameplayCategoryNeutral), &error));
    CollisionObjectDescriptor disabled = actor(
        3,
        30,
        glm::vec3(2.0f, 2.0f, 0.0f),
        network_example::physics::kGameplayCategoryHostileSide);
    disabled.enabled = false;
    assert(world->upsert_object(disabled, &error));
}

void populate_query_fixture(
    PhysicsWorld* world,
    std::uint32_t player_count,
    std::uint32_t hostile_count,
    float y) {
    std::string error;
    const std::uint32_t actor_count = player_count + hostile_count;
    for (std::uint32_t index = 0; index < actor_count; ++index) {
        const std::uint32_t gameplay_category = index < player_count
            ? network_example::physics::kGameplayCategoryPlayerSide
            : network_example::physics::kGameplayCategoryHostileSide;
        assert(world->upsert_object(actor(
            index + 1,
            index + 1,
            glm::vec3(2.0f + static_cast<float>(index), y, 0.0f),
            gameplay_category), &error));
    }
}

CollisionQueryStats run_shape_query_fixture(
    PhysicsWorld* world,
    float y,
    std::uint32_t gameplay_category_mask,
    std::size_t expected_hits) {
    ShapeCastRequest request{};
    request.shape.type = CollisionShapeType::kSphere;
    request.shape.radius = 0.25f;
    request.start = glm::vec3(0.0f, y, 0.0f);
    request.displacement = glm::vec3(30.0f, 0.0f, 0.0f);
    request.filter.collision_mask =
        network_example::physics::collision_layer_bit(
            CollisionLayer::kDamageable);
    request.filter.gameplay_category_mask = gameplay_category_mask;
    world->reset_query_stats();
    for (std::uint32_t query = 0; query < 200; ++query) {
        assert(world->shape_cast_all(request).size() == expected_hits);
    }
    return world->query_stats();
}

void require_equal(const std::vector<CollisionHit>& lhs, const std::vector<CollisionHit>& rhs) {
    assert(lhs.size() == rhs.size());
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        assert(lhs[index].identity.entity_net_id == rhs[index].identity.entity_net_id);
        assert(lhs[index].identity.collider_id == rhs[index].identity.collider_id);
        assert(lhs[index].identity.hit_zone == rhs[index].identity.hit_zone);
        assert(lhs[index].distance == rhs[index].distance);
        assert(lhs[index].fraction == rhs[index].fraction);
        assert(lhs[index].position == rhs[index].position);
        assert(lhs[index].normal == rhs[index].normal);
        assert(lhs[index].subshape_id == rhs[index].subshape_id);
    }
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::vector<std::uint8_t> terrain = read_bytes(argv[1]);

    PhysicsWorld world0(PhysicsWorldConfig{0});
    PhysicsWorld world2(PhysicsWorldConfig{2});
    assert(world0.valid());
    assert(world2.valid());
    assert(world0.query_worker_count() == 0);
    assert(world2.query_worker_count() == 2);

    const CollisionObjectIdentity terrain_identity{
        0,
        1,
        0,
        CollisionObjectKind::kTerrain,
        CollisionLayer::kTerrain,
    };
    std::string error;
    assert(world0.load_static_scene(terrain, terrain_identity, &error));
    assert(world2.load_static_scene(terrain, terrain_identity, &error));
    populate(&world0);
    populate(&world2);

    RayCastRequest ray{};
    ray.origin = glm::vec3(0.0f, 2.0f, 0.0f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 10.0f;
    ray.filter.collision_mask =
        network_example::physics::collision_layer_bit(CollisionLayer::kDamageable);
    const std::vector<CollisionHit> hits0 = world0.ray_cast_all(ray);
    const std::vector<CollisionHit> hits2 = world2.ray_cast_all(ray);
    require_equal(hits0, hits2);
    assert(hits0.size() == 3);
    assert(hits0[0].identity.entity_net_id == 1);
    assert(hits0[1].identity.entity_net_id == 2);
    assert(hits0[2].identity.entity_net_id == 4);

    ray.filter.ignored_entity_net_id = 1;
    const std::vector<CollisionHit> excluded = world0.ray_cast_all(ray);
    assert(excluded.size() == 2);
    assert(excluded[0].identity.entity_net_id == 2);

    CollisionHit closest{};
    assert(world0.ray_cast_closest(ray, &closest));
    assert(closest.identity.entity_net_id == 2);

    ShapeCastRequest shape_cast{};
    shape_cast.shape.type = CollisionShapeType::kSphere;
    shape_cast.shape.radius = 0.25f;
    shape_cast.start = glm::vec3(0.0f, 2.0f, 0.0f);
    shape_cast.displacement = glm::vec3(10.0f, 0.0f, 0.0f);
    shape_cast.filter = ray.filter;
    assert(world0.shape_cast_closest(shape_cast, &closest));

    OverlapRequest overlap{};
    overlap.shape.type = CollisionShapeType::kSphere;
    overlap.shape.radius = 1.0f;
    overlap.position = glm::vec3(5.0f, 2.0f, 0.0f);
    overlap.filter = ray.filter;
    assert(world0.overlap_all(overlap).size() == 2);

    ray.filter.ignored_entity_net_id = 0;
    ray.filter.gameplay_category_mask =
        network_example::physics::kGameplayCategoryHostileSide;
    shape_cast.filter = ray.filter;
    overlap.filter = ray.filter;
    assert(world0.ray_cast_all(ray).size() == 1);
    assert(world0.shape_cast_all(shape_cast).size() == 1);
    assert(world0.overlap_all(overlap).size() == 1);

    ray.filter.gameplay_category_mask =
        network_example::physics::kGameplayCategoryDamageable;
    shape_cast.filter = ray.filter;
    overlap.filter = ray.filter;
    assert(world0.ray_cast_all(ray).size() == 3);
    assert(world0.shape_cast_all(shape_cast).size() == 3);
    assert(world0.overlap_all(overlap).size() == 3);

    ray.filter.gameplay_category_mask = 0;
    shape_cast.filter = ray.filter;
    overlap.filter = ray.filter;
    assert(world0.ray_cast_all(ray).empty());
    assert(world0.shape_cast_all(shape_cast).empty());
    assert(world0.overlap_all(overlap).empty());

    assert(world0.set_object_transform(
        20,
        glm::vec3(8.0f, 2.0f, 0.0f),
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
    assert(world0.remove_object(20));
    assert(!world0.remove_object(20));

    RayCastRequest terrain_ray{};
    terrain_ray.origin = glm::vec3(0.0f, 20.0f, 0.0f);
    terrain_ray.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    terrain_ray.max_distance = 100.0f;
    terrain_ray.filter.collision_mask =
        network_example::physics::collision_layer_bit(CollisionLayer::kTerrain);
    terrain_ray.filter.gameplay_category_mask = 0;
    assert(world0.ray_cast_closest(terrain_ray, &closest));
    assert(closest.identity.kind == CollisionObjectKind::kTerrain);

    PhysicsWorld normal_world(PhysicsWorldConfig{0});
    CollisionObjectDescriptor tilted = actor(
        100,
        100,
        glm::vec3(0.0f),
        network_example::physics::kGameplayCategoryNeutral);
    constexpr float kHalfAngleRadians = 0.2617993878f;
    tilted.rotation = glm::quat(
        std::cos(kHalfAngleRadians),
        0.0f,
        0.0f,
        std::sin(kHalfAngleRadians));
    assert(normal_world.upsert_object(tilted, &error));
    RayCastRequest normal_ray{};
    normal_ray.origin = glm::vec3(0.0f, 3.0f, 0.0f);
    normal_ray.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    normal_ray.max_distance = 10.0f;
    normal_ray.filter.collision_mask =
        network_example::physics::collision_layer_bit(
            CollisionLayer::kDamageable);
    normal_ray.filter.gameplay_category_mask =
        network_example::physics::kGameplayCategoryNeutral;
    assert(normal_world.ray_cast_closest(normal_ray, &closest));
    assert(std::abs(closest.normal.x) > 0.4f);
    assert(closest.normal.y > 0.8f && closest.normal.y < 0.9f);

    std::vector<std::uint8_t> corrupt = terrain;
    corrupt[0] ^= 0xffu;
    assert(!world0.load_static_scene(corrupt, terrain_identity, &error));
    corrupt = terrain;
    corrupt.resize(corrupt.size() - 1);
    assert(!world0.load_static_scene(corrupt, terrain_identity, &error));

    PhysicsWorld balanced_fixture(PhysicsWorldConfig{0, true});
    assert(balanced_fixture.valid());
    populate_query_fixture(&balanced_fixture, 12, 12, 50.0f);
    const CollisionQueryStats balanced_none = run_shape_query_fixture(
        &balanced_fixture, 50.0f, 0, 0);
    const CollisionQueryStats balanced_hostile = run_shape_query_fixture(
        &balanced_fixture,
        50.0f,
        network_example::physics::kGameplayCategoryHostileSide,
        12);
    const CollisionQueryStats balanced_damageable = run_shape_query_fixture(
        &balanced_fixture,
        50.0f,
        network_example::physics::kGameplayCategoryDamageable,
        24);
    assert(balanced_none.shape_cast_query_count == 200);
    assert(balanced_hostile.shape_cast_query_count == 200);
    assert(balanced_damageable.shape_cast_query_count == 200);
    assert(balanced_none.damageable_actor_broadphase_layers_accepted == 0);
    assert(balanced_hostile.damageable_actor_broadphase_layers_accepted > 0);
    assert(balanced_damageable.damageable_actor_broadphase_layers_accepted > 0);
    assert(balanced_none.raw_jolt_hits_collected == 0);
    assert(balanced_hostile.raw_jolt_hits_collected * 2 ==
           balanced_damageable.raw_jolt_hits_collected);
    assert(balanced_hostile.final_hits_accepted * 2 ==
           balanced_damageable.final_hits_accepted);
    assert(balanced_hostile.player_object_layers_accepted == 0);
    assert(balanced_hostile.hostile_object_layers_accepted > 0);
    assert(balanced_damageable.player_object_layers_accepted > 0);
    assert(balanced_damageable.hostile_object_layers_accepted > 0);

    PhysicsWorld production_fixture(PhysicsWorldConfig{0, true});
    assert(production_fixture.valid());
    populate_query_fixture(&production_fixture, 1, 23, 100.0f);
    const CollisionQueryStats production_hostile = run_shape_query_fixture(
        &production_fixture,
        100.0f,
        network_example::physics::kGameplayCategoryHostileSide,
        23);
    const CollisionQueryStats production_damageable = run_shape_query_fixture(
        &production_fixture,
        100.0f,
        network_example::physics::kGameplayCategoryDamageable,
        24);
    assert(production_hostile.raw_jolt_hits_collected <
           production_damageable.raw_jolt_hits_collected);
    assert(production_damageable.raw_jolt_hits_collected -
           production_hostile.raw_jolt_hits_collected == 200);

    PhysicsWorld movement_world(PhysicsWorldConfig{0});
    assert(movement_world.valid());
    CollisionObjectDescriptor floor{};
    floor.identity = CollisionObjectIdentity{
        0,
        100,
        0,
        CollisionObjectKind::kStaticObstacle,
        CollisionLayer::kStaticObstacle,
    };
    floor.shape.type = CollisionShapeType::kBox;
    floor.shape.half_extents = glm::vec3{10.0f, 0.5f, 10.0f};
    floor.position = glm::vec3{0.0f, -0.5f, 0.0f};
    assert(movement_world.upsert_object(floor, &error));

    CollisionObjectDescriptor movement_body{};
    movement_body.identity = CollisionObjectIdentity{
        77,
        101,
        0,
        CollisionObjectKind::kActorMovement,
        CollisionLayer::kActorMovement,
    };
    movement_body.shape.type = CollisionShapeType::kCapsule;
    movement_body.shape.radius = 0.35f;
    movement_body.shape.capsule_half_height = 0.55f;
    movement_body.position = glm::vec3{3.0f, 0.9f, 0.0f};
    assert(movement_world.upsert_object(movement_body, &error));
    CollisionObjectDescriptor invalid_capsule = movement_body;
    invalid_capsule.identity.collider_id = 102;
    invalid_capsule.shape.radius = 0.0f;
    assert(!movement_world.upsert_object(invalid_capsule, &error));

    RayCastRequest movement_ray{};
    movement_ray.origin = glm::vec3{0.0f, 0.9f, 0.0f};
    movement_ray.direction = glm::vec3{1.0f, 0.0f, 0.0f};
    movement_ray.max_distance = 10.0f;
    movement_ray.filter.collision_mask =
        network_example::physics::kMovementCollisionMask;
    assert(movement_world.ray_cast_closest(movement_ray, &closest));
    assert(closest.identity.collider_id == 101);
    movement_ray.filter.collision_mask =
        network_example::physics::kCollisionMaskAll;
    assert(!movement_world.ray_cast_closest(movement_ray, &closest));

    CharacterDescriptor character{};
    character.character_id = 7;
    character.shape.type = CollisionShapeType::kCapsule;
    character.shape.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
    character.shape.radius = 0.35f;
    character.shape.capsule_half_height = 0.55f;
    character.max_slope_degrees = 50.0f;
    assert(movement_world.upsert_character(character, &error));
    CharacterMoveRequest move{};
    move.character_id = 7;
    move.current_position = glm::vec3{0.0f, 2.0f, 0.0f};
    move.delta_seconds = 1.0f / 30.0f;
    move.step_height = 0.4f;
    move.ground_snap_distance = 0.5f;
    move.filter.collision_mask =
        network_example::physics::kMovementCollisionMask;
    move.filter.ignored_entity_net_id = 7;
    CharacterMoveResult move_result{};
    for (int tick = 0; tick < 120; ++tick) {
        move.linear_velocity.y += -9.81f * move.delta_seconds;
        assert(movement_world.move_character(move, &move_result, &error));
        move.current_position = move_result.position;
        move.linear_velocity = move_result.linear_velocity;
        if (move_result.ground_state == CharacterGroundState::kGrounded) {
            break;
        }
    }
    assert(move_result.ground_state == CharacterGroundState::kGrounded);
    assert(move_result.position.y > -0.01f);
    assert(movement_world.remove_character(7));
    assert(!movement_world.move_character(move, &move_result, &error));
    return 0;
}
