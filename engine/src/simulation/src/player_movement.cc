#include "simulation/public/simulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <glm/geometric.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/movement_solver.h"

namespace network_example {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kInitialGroundingSearchDistance = 10000.0f;

std::uint64_t elapsed_us(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start)
            .count());
}

physics::CollisionShapeDescriptor movement_shape(
    const ColliderInstance& collider) {
    physics::CollisionShapeDescriptor shape{};
    shape.type = physics::CollisionShapeType::kCapsule;
    shape.local_center = collider.local_center;
    shape.radius = collider.radius;
    shape.capsule_half_height = collider.capsule_half_height;
    return shape;
}

physics::CollisionQueryFilter movement_filter(
    NetId net_id,
    std::uint32_t collider_id) {
    physics::CollisionQueryFilter filter{};
    filter.collision_mask = physics::kMovementCollisionMask;
    filter.ignored_entity_net_id = net_id;
    filter.ignored_collider_id = collider_id;
    return filter;
}

struct GroundProbe {
    bool hit = false;
    bool walkable = false;
    physics::CollisionHit collision{};
};

GroundProbe probe_ground(
    physics::PhysicsWorld& physics_world,
    const ColliderInstance& collider,
    const glm::vec3& position,
    const glm::quat& rotation,
    float distance,
    float max_slope_degrees,
    bool initial_placement = false) {
    GroundProbe result{};
    if (distance <= 0.0f) {
        return result;
    }
    physics::ShapeCastRequest request{};
    request.shape = movement_shape(collider);
    request.start = position;
    request.rotation = rotation;
    request.displacement = glm::vec3{0.0f, -distance, 0.0f};
    request.filter = movement_filter(
        collider.entity_net_id, collider.collider_id);
    if (initial_placement) {
        request.filter.collision_mask =
            physics::collision_layer_bit(physics::CollisionLayer::kTerrain) |
            physics::collision_layer_bit(
                physics::CollisionLayer::kStaticObstacle);
    }
    const float walkable_normal_y =
        std::cos(glm::radians(max_slope_degrees));
    for (const physics::CollisionHit& hit :
         physics_world.shape_cast_all(request)) {
        if (!result.hit) {
            result.hit = true;
            result.collision = hit;
        }
        if (hit.normal.y >= walkable_normal_y) {
            result.hit = true;
            result.walkable = true;
            result.collision = hit;
            break;
        }
    }
    return result;
}

glm::vec3 move_kinematic_horizontal(
    physics::PhysicsWorld& physics_world,
    const ColliderInstance& collider,
    const Transform& transform,
    const glm::vec3& displacement,
    float max_slope_degrees) {
    if (glm::dot(displacement, displacement) <= 0.00000001f) {
        return transform.position;
    }
    physics::ShapeCastRequest request{};
    request.shape = movement_shape(collider);
    request.start = transform.position;
    request.rotation = transform.rotation;
    request.displacement = displacement;
    request.filter = movement_filter(
        collider.entity_net_id, collider.collider_id);
    const float walkable_normal_y =
        std::cos(glm::radians(max_slope_degrees));
    float safe_fraction = 1.0f;
    float walkable_fraction = 1.0f;
    glm::vec3 walkable_normal{0.0f, 1.0f, 0.0f};
    const glm::vec3 direction = glm::normalize(displacement);
    for (const physics::CollisionHit& hit :
         physics_world.shape_cast_all(request)) {
        const bool blocks_forward = glm::dot(direction, hit.normal) < -0.0001f;
        const bool wall_or_steep = hit.normal.y < walkable_normal_y;
        if (blocks_forward && wall_or_steep) {
            safe_fraction = std::min(
                safe_fraction, std::max(0.0f, hit.fraction - 0.001f));
        } else if (blocks_forward && hit.fraction < walkable_fraction) {
            walkable_fraction = hit.fraction;
            walkable_normal = hit.normal;
        }
    }
    if (walkable_fraction < safe_fraction && walkable_fraction < 1.0f) {
        const glm::vec3 remaining =
            displacement * (1.0f - walkable_fraction);
        const glm::vec3 along_slope = remaining -
            walkable_normal * glm::dot(remaining, walkable_normal);
        return transform.position + displacement * walkable_fraction +
            along_slope;
    }
    return transform.position + displacement * safe_fraction;
}

struct BufferedMovementResult {
    entt::entity entity = entt::null;
    NetId net_id = 0;
    PeerId owner_peer = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    MovementState movement{};
    bool physics_finalized = false;
};

}  // namespace

void simulate_player_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds) {
    for (const QueuedInput& queued_input : inputs) {
        auto view = world.registry()
                        .view<NetworkIdentity, Transform, Velocity, MovementState, PlayerTag>();
        for (const entt::entity entity : view) {
            const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
            if (identity.owner_peer != queued_input.owner_peer) {
                continue;
            }
            const MovementState& movement = view.get<MovementState>(entity);
            Velocity& velocity = view.get<Velocity>(entity);
            Transform& transform = view.get<Transform>(entity);
            velocity.linear = movement_solver::input_move_to_world(queued_input.input) *
                              movement.speed_meters_per_second;
            transform.position += velocity.linear * fixed_delta_seconds;
        }
    }
}

void simulate_actor_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    MovementSimulationStats* stats,
    std::uint32_t actor_blocking_mode,
    std::vector<NetId>* physics_finalized_actor_net_ids) {
    physics::PhysicsWorld* physics_world = world.collision_world();
    if (physics_world == nullptr || fixed_delta_seconds <= 0.0f) {
        return;
    }

    auto view = world.registry().view<
        NetworkIdentity,
        EntityKind,
        Transform,
        Velocity,
        MovementState>();
    std::vector<entt::entity> actors;
    for (const entt::entity entity : view) {
        if (view.get<EntityKind>(entity).type == EntityType::kActor) {
            actors.push_back(entity);
        }
    }
    std::sort(
        actors.begin(),
        actors.end(),
        [&view](entt::entity lhs, entt::entity rhs) {
            return view.get<NetworkIdentity>(lhs).net_id <
                view.get<NetworkIdentity>(rhs).net_id;
        });

    std::unordered_map<NetId, const ColliderInstance*> movement_colliders;
    movement_colliders.reserve(actors.size());
    for (const ColliderInstance& collider :
         world.collider_registry().instances()) {
        if (collider.lifetime_ticks == 0 && collider.enabled &&
            collider.shape_type == ColliderShapeType::kCapsule &&
            (collider.purpose_flags & KernelColliderPurpose_Movement) != 0u) {
            movement_colliders.emplace(collider.entity_net_id, &collider);
        }
    }
    std::unordered_map<NetId, const QueuedInput*> latest_input_by_net_id;
    std::unordered_map<PeerId, const QueuedInput*> latest_input_by_owner;
    latest_input_by_net_id.reserve(inputs.size());
    latest_input_by_owner.reserve(inputs.size());
    for (const QueuedInput& input : inputs) {
        auto& latest = input.controlled_net_id != 0
            ? latest_input_by_net_id[input.controlled_net_id]
            : latest_input_by_owner[input.owner_peer];
        if (latest == nullptr ||
            input.input.input_seq > latest->input.input_seq) {
            latest = &input;
        }
    }

    std::vector<BufferedMovementResult> buffered;
    buffered.reserve(actors.size());
    for (const entt::entity entity : actors) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const Transform& transform = view.get<Transform>(entity);
        const Velocity& current_velocity = view.get<Velocity>(entity);
        MovementState next_movement = view.get<MovementState>(entity);
        next_movement.landed_this_tick = false;
        if (next_movement.controller_type ==
            MovementState::ControllerType::kNone) {
            continue;
        }
        const auto collider_found = movement_colliders.find(identity.net_id);
        const ColliderInstance* collider = collider_found == movement_colliders.end()
            ? nullptr
            : collider_found->second;
        if (collider == nullptr ||
            collider->collider_template_id !=
                next_movement.movement_collider_template_id) {
            continue;
        }

        glm::vec3 desired_horizontal{
            current_velocity.linear.x, 0.0f, current_velocity.linear.z};
        if (world.registry().all_of<PlayerTag>(entity)) {
            const auto by_net_id = latest_input_by_net_id.find(identity.net_id);
            const auto by_owner = latest_input_by_owner.find(identity.owner_peer);
            const QueuedInput* input = by_net_id != latest_input_by_net_id.end()
                ? by_net_id->second
                : by_owner != latest_input_by_owner.end()
                    ? by_owner->second
                    : nullptr;
            desired_horizontal = input == nullptr
                ? glm::vec3{0.0f}
                : movement_solver::input_move_to_world(input->input) *
                    next_movement.speed_meters_per_second;
            desired_horizontal.y = 0.0f;
        }

        BufferedMovementResult result{};
        result.entity = entity;
        result.net_id = identity.net_id;
        result.owner_peer = identity.owner_peer;
        result.position = transform.position;
        result.velocity = current_velocity.linear;
        result.movement = next_movement;
        const bool was_grounded = next_movement.ground_state ==
            MovementState::GroundState::kGrounded;

        if (next_movement.controller_type ==
            MovementState::ControllerType::kCharacter) {
            const Clock::time_point start = Clock::now();
            movement_solver::CharacterMovementConfig config{};
            config.character_id = identity.net_id;
            config.shape = movement_shape(*collider);
            config.gravity = next_movement.gravity;
            config.max_slope_degrees = next_movement.max_slope_degrees;
            config.step_height = next_movement.step_height;
            config.ground_snap_distance = next_movement.ground_snap_distance;
            config.filter = movement_filter(
                identity.net_id, collider->collider_id);
            if (actor_blocking_mode == KernelActorBlockingMode_Disabled) {
                config.filter.collision_mask &=
                    ~physics::collision_layer_bit(
                        physics::CollisionLayer::kActorMovement);
            }
            movement_solver::CharacterMovementState state{};
            state.position = transform.position;
            state.rotation = transform.rotation;
            state.velocity = current_velocity.linear;
            state.ground_state = next_movement.ground_state ==
                    MovementState::GroundState::kGrounded
                ? physics::CharacterGroundState::kGrounded
                : next_movement.ground_state ==
                          MovementState::GroundState::kSteepGround
                    ? physics::CharacterGroundState::kSteepGround
                    : physics::CharacterGroundState::kAirborne;
            state.ground_normal = next_movement.ground_normal;
            std::string error;
            if (movement_solver::step_character(
                    *physics_world,
                    config,
                    desired_horizontal,
                    fixed_delta_seconds,
                    &state,
                    &error)) {
                result.physics_finalized = true;
                result.position = state.position;
                result.velocity = state.velocity;
                result.movement.ground_normal = state.ground_normal;
                result.movement.supporting_entity_net_id =
                    state.supporting_identity.entity_net_id;
                result.movement.supporting_collider_id =
                    state.supporting_identity.collider_id;
                result.movement.ground_state =
                    state.ground_state ==
                            physics::CharacterGroundState::kGrounded
                        ? MovementState::GroundState::kGrounded
                        : state.ground_state ==
                                  physics::CharacterGroundState::kSteepGround
                            ? MovementState::GroundState::kSteepGround
                            : MovementState::GroundState::kAirborne;
            }
            if (stats != nullptr) {
                ++stats->character_move_count;
                stats->character_move_cost_us += elapsed_us(start);
            }
        } else {
            result.physics_finalized = true;
            const bool kinematic = next_movement.controller_type ==
                MovementState::ControllerType::kKinematic;
            const Clock::time_point start = Clock::now();
            result.position = kinematic
                ? move_kinematic_horizontal(
                    *physics_world,
                    *collider,
                    transform,
                    desired_horizontal * fixed_delta_seconds,
                    next_movement.max_slope_degrees)
                : transform.position + desired_horizontal * fixed_delta_seconds;
            result.velocity.x = desired_horizontal.x;
            result.velocity.z = desired_horizontal.z;

            bool placed_on_initial_ground = false;
            if (!next_movement.has_last_queried_position && !was_grounded) {
                glm::vec3 initial_probe_start = result.position;
                initial_probe_start.y += kInitialGroundingSearchDistance;
                const Clock::time_point initial_ground_start = Clock::now();
                GroundProbe initial_ground = probe_ground(
                    *physics_world,
                    *collider,
                    initial_probe_start,
                    transform.rotation,
                    kInitialGroundingSearchDistance * 2.0f,
                    next_movement.max_slope_degrees,
                    true);
                if (stats != nullptr) {
                    ++stats->grounded_query_count;
                    stats->grounded_query_cost_us +=
                        elapsed_us(initial_ground_start);
                }
                if (initial_ground.walkable) {
                    result.position = initial_probe_start + glm::vec3{
                        0.0f,
                        -kInitialGroundingSearchDistance * 2.0f *
                            initial_ground.collision.fraction,
                        0.0f};
                    result.velocity.y = 0.0f;
                    result.movement.ground_state =
                        MovementState::GroundState::kGrounded;
                    result.movement.ground_normal =
                        initial_ground.collision.normal;
                    result.movement.supporting_entity_net_id =
                        initial_ground.collision.identity.entity_net_id;
                    result.movement.supporting_collider_id =
                        initial_ground.collision.identity.collider_id;
                    result.movement.last_queried_position = result.position;
                    result.movement.has_last_queried_position = true;
                    placed_on_initial_ground = true;
                }
            }

            const bool unchanged_grounded = was_grounded &&
                glm::dot(
                    result.position - transform.position,
                    result.position - transform.position) <=
                    0.00000001f &&
                next_movement.has_last_queried_position &&
                glm::dot(
                    transform.position - next_movement.last_queried_position,
                    transform.position - next_movement.last_queried_position) <=
                    0.00000001f;
            if (!placed_on_initial_ground &&
                (!unchanged_grounded || kinematic)) {
                const float probe_distance = was_grounded
                    ? next_movement.ground_snap_distance
                    : next_movement.ground_probe_distance;
                const Clock::time_point ground_start = Clock::now();
                GroundProbe ground = probe_ground(
                    *physics_world,
                    *collider,
                    result.position,
                    transform.rotation,
                    probe_distance,
                    next_movement.max_slope_degrees);
                result.movement.last_queried_position = result.position;
                result.movement.has_last_queried_position = true;
                if (stats != nullptr) {
                    ++stats->grounded_query_count;
                    stats->grounded_query_cost_us += elapsed_us(ground_start);
                }
                if (ground.walkable) {
                    result.position.y -= probe_distance * ground.collision.fraction;
                    result.velocity.y = 0.0f;
                    result.movement.ground_state =
                        MovementState::GroundState::kGrounded;
                    result.movement.ground_normal = ground.collision.normal;
                    result.movement.supporting_entity_net_id =
                        ground.collision.identity.entity_net_id;
                    result.movement.supporting_collider_id =
                        ground.collision.identity.collider_id;
                } else {
                    result.movement.ground_state = ground.hit
                        ? MovementState::GroundState::kSteepGround
                        : MovementState::GroundState::kAirborne;
                }
            }

            if (result.movement.ground_state !=
                MovementState::GroundState::kGrounded) {
                result.velocity +=
                    next_movement.gravity * fixed_delta_seconds;
                const glm::vec3 vertical_displacement{
                    0.0f,
                    result.velocity.y * fixed_delta_seconds,
                    0.0f};
                if (vertical_displacement.y < 0.0f) {
                    const Clock::time_point landing_start = Clock::now();
                    GroundProbe landing = probe_ground(
                        *physics_world,
                        *collider,
                        result.position,
                        transform.rotation,
                        -vertical_displacement.y,
                        next_movement.max_slope_degrees);
                    if (stats != nullptr) {
                        ++stats->grounded_query_count;
                        stats->grounded_query_cost_us +=
                            elapsed_us(landing_start);
                    }
                    if (landing.walkable) {
                        result.position.y +=
                            vertical_displacement.y * landing.collision.fraction;
                        result.velocity.y = 0.0f;
                        result.movement.ground_state =
                            MovementState::GroundState::kGrounded;
                        result.movement.ground_normal = landing.collision.normal;
                        result.movement.supporting_entity_net_id =
                            landing.collision.identity.entity_net_id;
                        result.movement.supporting_collider_id =
                            landing.collision.identity.collider_id;
                    } else {
                        result.position += vertical_displacement;
                    }
                } else {
                    result.position += vertical_displacement;
                }
            }
            const std::uint64_t cost = elapsed_us(start);
            if (stats != nullptr) {
                if (kinematic) {
                    ++stats->kinematic_move_count;
                    stats->kinematic_move_cost_us += cost;
                }
            }
        }

        const bool is_grounded = result.movement.ground_state ==
            MovementState::GroundState::kGrounded;
        result.movement.landed_this_tick = !was_grounded && is_grounded;
        buffered.push_back(result);
    }

    for (const BufferedMovementResult& result : buffered) {
        world.registry().get<Transform>(result.entity).position = result.position;
        world.registry().get<Velocity>(result.entity).linear = result.velocity;
        world.registry().get<MovementState>(result.entity) = result.movement;
        if (result.physics_finalized &&
            physics_finalized_actor_net_ids != nullptr) {
            physics_finalized_actor_net_ids->push_back(result.net_id);
        }
        if (result.movement.landed_this_tick && events != nullptr) {
            events->push_back(KernelEvent{
                KernelEventType_ActorLanded,
                current_tick,
                result.net_id,
                result.owner_peer,
                static_cast<std::uint32_t>(result.movement.controller_type),
            });
        }
    }
}

void simulate_velocity_movement(World& world, float fixed_delta_seconds) {
    if (fixed_delta_seconds <= 0.0f) {
        return;
    }

    auto view = world.registry().view<Transform, Velocity>();
    for (const entt::entity entity : view) {
        if (world.registry().all_of<MovementState>(entity) ||
            world.registry().all_of<ProjectileTag>(entity)) {
            continue;
        }
        Transform& transform = view.get<Transform>(entity);
        const Velocity& velocity = view.get<Velocity>(entity);
        transform.position += velocity.linear * fixed_delta_seconds;
    }
}

}  // namespace network_example
