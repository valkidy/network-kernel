#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"

namespace {

using network_example::ColliderInstance;
using network_example::ColliderShapeType;
using network_example::MovementSimulationStats;
using network_example::MovementState;
using network_example::NetId;
using network_example::NetworkIdentity;
using network_example::QueuedInput;
using network_example::Transform;
using network_example::Velocity;
using network_example::World;
using network_example::physics::CollisionLayer;
using network_example::physics::CollisionObjectDescriptor;
using network_example::physics::CollisionObjectIdentity;
using network_example::physics::CollisionObjectKind;
using network_example::physics::CollisionShapeType;
using network_example::physics::PhysicsWorld;

struct Fixture {
    Fixture(MovementState::ControllerType controller, glm::vec3 spawn)
        : world(false) {
        world.set_collision_world(&physics);
        add_box(
            100,
            glm::vec3{0.0f, -0.5f, 0.0f},
            glm::vec3{20.0f, 0.5f, 20.0f});
        player = world.spawn_player(7, spawn);
        const std::optional<entt::entity> found = world.find_entity(player);
        assert(found.has_value());
        entity = *found;
        MovementState& movement = world.registry().get<MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = controller;
        movement.movement_collider_template_id = 10;
        movement.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

        ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = player;
        collider.entity_net_id = player;
        collider.entity_type = network_example::EntityType::kActor;
        collider.actor_type = network_example::ActorType::kPlayer;
        collider.shape_type = ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = glm::vec3{0.0f, 0.9f, 0.0f};
        collider.world_center = spawn + collider.local_center;
        collider.radius = 0.35f;
        collider.capsule_half_height = 0.55f;
        ColliderInstance& stored = world.collider_registry().upsert_entity_collider(
            player, 10, collider);
        movement.movement_collider_id = stored.collider_id;
        movement_collider_id = stored.collider_id;
        sync_body();
    }

    void add_box(
        std::uint32_t collider_id,
        const glm::vec3& position,
        const glm::vec3& half_extents,
        const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            0,
            collider_id,
            0,
            CollisionObjectKind::kStaticObstacle,
            CollisionLayer::kStaticObstacle,
        };
        object.shape.type = CollisionShapeType::kBox;
        object.shape.half_extents = half_extents;
        object.position = position;
        object.rotation = rotation;
        std::string error;
        assert(physics.upsert_object(object, &error));
    }

    void add_movement_obstacle(
        std::uint32_t collider_id,
        std::uint32_t entity_net_id,
        const glm::vec3& position) {
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            entity_net_id,
            collider_id,
            0,
            CollisionObjectKind::kActorMovement,
            CollisionLayer::kActorMovement,
        };
        object.shape.type = CollisionShapeType::kCapsule;
        object.shape.radius = 0.35f;
        object.shape.capsule_half_height = 0.55f;
        object.position = position + glm::vec3{0.0f, 0.9f, 0.0f};
        std::string error;
        assert(physics.upsert_object(object, &error));
    }

    void sync_body() {
        const Transform& transform = world.registry().get<Transform>(entity);
        const ColliderInstance& collider = world.collider_registry().instances()[0];
        CollisionObjectDescriptor object{};
        object.identity = CollisionObjectIdentity{
            player,
            movement_collider_id,
            0,
            CollisionObjectKind::kActorMovement,
            CollisionLayer::kActorMovement,
        };
        object.shape.type = CollisionShapeType::kCapsule;
        object.shape.radius = collider.radius;
        object.shape.capsule_half_height = collider.capsule_half_height;
        object.position = transform.position + collider.local_center;
        std::string error;
        assert(physics.upsert_object(object, &error));
    }

    void tick(const PlayerInput* input = nullptr) {
        std::vector<QueuedInput> inputs;
        if (input != nullptr) {
            inputs.push_back(QueuedInput{7, *input, tick_index, 0, false, 0});
        }
        network_example::simulate_actor_movement(
            world,
            inputs,
            1.0f / 30.0f,
            tick_index,
            &events,
            &stats);
        sync_body();
        ++tick_index;
    }

    PhysicsWorld physics;
    World world;
    NetId player = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
    std::uint32_t tick_index = 1;
    std::vector<KernelEvent> events;
    MovementSimulationStats stats{};
};

std::size_t landed_event_count(const std::vector<KernelEvent>& events) {
    std::size_t count = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_ActorLanded) {
            ++count;
        }
    }
    return count;
}

void grounded_falls_lands_once_and_stops_requerying() {
    Fixture fixture(MovementState::ControllerType::kGrounded, {0.0f, 2.0f, 0.0f});
    for (int tick = 0; tick < 120; ++tick) {
        fixture.tick();
    }
    const MovementState& movement =
        fixture.world.registry().get<MovementState>(fixture.entity);
    const Transform& transform =
        fixture.world.registry().get<Transform>(fixture.entity);
    assert(movement.ground_state == MovementState::GroundState::kGrounded);
    assert(std::fabs(transform.position.y) < 0.02f);
    assert(landed_event_count(fixture.events) == 1);
    const std::uint64_t query_count = fixture.stats.grounded_query_count;
    fixture.tick();
    fixture.tick();
    assert(fixture.stats.grounded_query_count <= query_count + 1);
}

void grounded_initially_below_terrain_snaps_to_hit_position() {
    Fixture fixture(
        MovementState::ControllerType::kGrounded,
        {0.0f, -5.0f, 0.0f});
    fixture.tick();
    const MovementState& movement =
        fixture.world.registry().get<MovementState>(fixture.entity);
    const Transform& transform =
        fixture.world.registry().get<Transform>(fixture.entity);
    const Velocity& velocity =
        fixture.world.registry().get<Velocity>(fixture.entity);
    assert(movement.ground_state == MovementState::GroundState::kGrounded);
    assert(std::fabs(transform.position.y) < 0.02f);
    assert(std::fabs(velocity.linear.y) < 0.0001f);
    assert(landed_event_count(fixture.events) == 1);
}

void kinematic_blocks_on_wall_and_queries_ground_each_tick() {
    Fixture fixture(MovementState::ControllerType::kKinematic, {0.0f, 0.0f, 0.0f});
    fixture.add_box(
        200,
        glm::vec3{2.0f, 1.0f, 0.0f},
        glm::vec3{0.25f, 1.0f, 3.0f});
    fixture.tick();
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 60; ++tick) {
        input.input_seq = tick + 1;
        fixture.tick(&input);
    }
    const Transform& transform =
        fixture.world.registry().get<Transform>(fixture.entity);
    const MovementState& movement =
        fixture.world.registry().get<MovementState>(fixture.entity);
    assert(transform.position.x < 1.45f);
    assert(movement.ground_state == MovementState::GroundState::kGrounded);
    assert(fixture.stats.kinematic_move_count == 61);
    assert(fixture.stats.grounded_query_count == 61);
}

void character_is_grounded_and_slides_along_wall() {
    Fixture fixture(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    fixture.add_box(
        200,
        glm::vec3{2.0f, 1.0f, 0.0f},
        glm::vec3{0.25f, 1.0f, 20.0f});
    fixture.tick();
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 1.0f};
    for (std::uint32_t tick = 0; tick < 60; ++tick) {
        input.input_seq = tick + 1;
        fixture.tick(&input);
    }
    const Transform& transform =
        fixture.world.registry().get<Transform>(fixture.entity);
    const MovementState& movement =
        fixture.world.registry().get<MovementState>(fixture.entity);
    assert(transform.position.x < 1.5f);
    assert(transform.position.z > 3.0f);
    assert(movement.ground_state == MovementState::GroundState::kGrounded);
    assert(fixture.stats.character_move_count == 61);
    assert(landed_event_count(fixture.events) == 1);
}

void character_recovers_from_initial_penetration() {
    Fixture fixture(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    fixture.add_box(
        200,
        glm::vec3{0.0f, 0.9f, 0.0f},
        glm::vec3{0.2f, 0.9f, 1.0f});
    const glm::vec3 initial =
        fixture.world.registry().get<Transform>(fixture.entity).position;
    for (int tick = 0; tick < 10; ++tick) {
        fixture.tick();
    }
    const glm::vec3 recovered =
        fixture.world.registry().get<Transform>(fixture.entity).position;
    assert(std::isfinite(recovered.x));
    assert(std::isfinite(recovered.y));
    assert(std::isfinite(recovered.z));
    assert(glm::distance(initial, recovered) > 0.01f);
}

void character_steps_over_obstacle_while_kinematic_stops() {
    Fixture kinematic(
        MovementState::ControllerType::kKinematic,
        {0.0f, 0.0f, 0.0f});
    Fixture character(
        MovementState::ControllerType::kCharacter,
        {0.0f, 0.0f, 0.0f});
    kinematic.add_box(
        200,
        glm::vec3{2.0f, 0.15f, 0.0f},
        glm::vec3{0.25f, 0.15f, 2.0f});
    character.add_box(
        200,
        glm::vec3{2.0f, 0.15f, 0.0f},
        glm::vec3{0.25f, 0.15f, 2.0f});
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 40; ++tick) {
        input.input_seq = tick + 1;
        kinematic.tick(&input);
        character.tick(&input);
    }
    const float kinematic_x =
        kinematic.world.registry().get<Transform>(kinematic.entity).position.x;
    const float character_x =
        character.world.registry().get<Transform>(character.entity).position.x;
    assert(kinematic_x < 1.5f);
    assert(character_x > 3.0f);
}

void character_blocks_against_other_actor_movement_body() {
    Fixture fixture(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    fixture.add_movement_obstacle(200, 99, {2.0f, 0.0f, 0.0f});
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 30; ++tick) {
        input.input_seq = tick + 1;
        fixture.tick(&input);
    }
    const float x =
        fixture.world.registry().get<Transform>(fixture.entity).position.x;
    assert(x < 1.35f);
}

void character_blocks_upward_motion_at_ceiling() {
    Fixture fixture(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    fixture.add_box(
        200,
        glm::vec3{0.0f, 2.1f, 0.0f},
        glm::vec3{2.0f, 0.1f, 2.0f});
    fixture.world.registry().get<MovementState>(fixture.entity).ground_state =
        MovementState::GroundState::kAirborne;
    fixture.world.registry().get<Velocity>(fixture.entity).linear.y = 6.0f;
    for (int tick = 0; tick < 5; ++tick) {
        fixture.tick();
    }
    const Transform& transform =
        fixture.world.registry().get<Transform>(fixture.entity);
    assert(transform.position.y < 0.25f);
}

void character_replay_is_deterministic() {
    Fixture first(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    Fixture second(MovementState::ControllerType::kCharacter, {0.0f, 0.0f, 0.0f});
    first.add_box(200, {2.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 2.0f});
    second.add_box(200, {2.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 2.0f});
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 0.5f};
    for (std::uint32_t tick = 0; tick < 60; ++tick) {
        input.input_seq = tick + 1;
        first.tick(&input);
        second.tick(&input);
        assert(first.world.registry().get<Transform>(first.entity).position ==
               second.world.registry().get<Transform>(second.entity).position);
        assert(first.world.registry().get<Velocity>(first.entity).linear ==
               second.world.registry().get<Velocity>(second.entity).linear);
        assert(first.world.registry().get<MovementState>(first.entity).ground_state ==
               second.world.registry().get<MovementState>(second.entity).ground_state);
    }
}

void walkable_and_steep_slopes_follow_controller_policy() {
    Fixture kinematic(
        MovementState::ControllerType::kKinematic,
        {-1.0f, 0.0f, 0.0f});
    Fixture character(
        MovementState::ControllerType::kCharacter,
        {-1.0f, 0.0f, 0.0f});
    const glm::quat walkable_rotation = glm::angleAxis(
        glm::radians(20.0f), glm::vec3{0.0f, 0.0f, 1.0f});
    kinematic.add_box(
        200,
        {2.0f, 0.838f, 0.0f},
        {3.0f, 0.2f, 2.0f},
        walkable_rotation);
    character.add_box(
        200,
        {2.0f, 0.838f, 0.0f},
        {3.0f, 0.2f, 2.0f},
        walkable_rotation);
    PlayerInput input{};
    input.move = KernelVec2{1.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 25; ++tick) {
        input.input_seq = tick + 1;
        kinematic.tick(&input);
        character.tick(&input);
    }
    const Transform& kinematic_transform =
        kinematic.world.registry().get<Transform>(kinematic.entity);
    const Transform& character_transform =
        character.world.registry().get<Transform>(character.entity);
    assert(kinematic_transform.position.y > 0.4f);
    assert(character_transform.position.y > 0.4f);
    assert(kinematic.world.registry()
               .get<MovementState>(kinematic.entity)
               .ground_state == MovementState::GroundState::kGrounded);
    assert(character.world.registry()
               .get<MovementState>(character.entity)
               .ground_state == MovementState::GroundState::kGrounded);

    Fixture steep(
        MovementState::ControllerType::kKinematic,
        {0.0f, 0.0f, 0.0f});
    steep.add_box(
        200,
        {2.0f, 1.5f, 0.0f},
        {2.0f, 0.2f, 2.0f},
        glm::angleAxis(
            glm::radians(65.0f), glm::vec3{0.0f, 0.0f, 1.0f}));
    for (std::uint32_t tick = 0; tick < 30; ++tick) {
        input.input_seq = tick + 1;
        steep.tick(&input);
    }
    const Transform& steep_transform =
        steep.world.registry().get<Transform>(steep.entity);
    assert(steep_transform.position.x < 1.2f);
    assert(steep_transform.position.y < 0.4f);
}

}  // namespace

int main() {
    grounded_falls_lands_once_and_stops_requerying();
    grounded_initially_below_terrain_snaps_to_hit_position();
    kinematic_blocks_on_wall_and_queries_ground_each_tick();
    character_is_grounded_and_slides_along_wall();
    character_recovers_from_initial_penetration();
    character_steps_over_obstacle_while_kinematic_stops();
    character_blocks_against_other_actor_movement_body();
    character_blocks_upward_motion_at_ceiling();
    character_replay_is_deterministic();
    walkable_and_steep_slopes_follow_controller_policy();
    return 0;
}
