#ifndef SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_
#define SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_

#include <cstdint>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "kernel/public/kernel_types.h"
#include "physics/public/physics_world.h"
#include "sync/public/snapshot.h"

namespace network_example::movement_solver {

glm::vec3 input_move_to_world(const KernelPlayerInput& input);

void apply_player_input(
    EntitySnapshot& entity,
    const KernelPlayerInput& input,
    float fixed_delta_seconds,
    float move_speed_meters_per_second);

// Vertical velocity for one character step.
//
// While standing on walkable ground this solves n . v = 0, so the character
// follows the slope instead of leaving it. That division by ground_normal.y
// has gain tan(slope) and DIVERGES as the ground approaches vertical, so it is
// applied only when the normal is genuinely walkable, never merely because the
// ground state says grounded.
//
// Those two disagree in practice: Jolt reports OnGround while handing back a
// near-horizontal ground normal, measured live at ground_normal.y = 0.0095
// against a 0.6428 limit. Guarding on the state alone (or on a token
// ground_normal.y > 0.001) let that through at a gain of 105, turning a 2.5 m/s
// walk into 146 m/s straight up and firing the actor 750 m into the air.
//
// When they disagree the normal is believed and the character falls instead.
// That is the direction that cannot explode: the worst case is a frame of
// gravity on ground it could not have walked on anyway.
glm::vec3 ground_following_velocity(
    const glm::vec3& desired_horizontal_velocity,
    physics::CharacterGroundState ground_state,
    const glm::vec3& ground_normal,
    float max_slope_degrees,
    float previous_vertical_velocity,
    float gravity_y,
    float fixed_delta_seconds);

struct CharacterMovementConfig {
    std::uint32_t character_id = 0;
    physics::CollisionShapeDescriptor shape{};
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float max_slope_degrees = 50.0f;
    float step_height = 0.4f;
    float ground_snap_distance = 0.5f;
    physics::CollisionQueryFilter filter{};
};

struct CharacterMovementState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    physics::CharacterGroundState ground_state =
        physics::CharacterGroundState::kAirborne;
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    physics::CollisionObjectIdentity supporting_identity{};
};

bool reset_character(
    physics::PhysicsWorld& physics_world,
    const CharacterMovementConfig& config,
    std::string* error);

bool step_character(
    physics::PhysicsWorld& physics_world,
    const CharacterMovementConfig& config,
    const glm::vec3& desired_horizontal_velocity,
    float fixed_delta_seconds,
    CharacterMovementState* state,
    std::string* error);

}  // namespace network_example::movement_solver

#endif  // SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_
