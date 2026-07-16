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

glm::vec3 input_move_to_world(const PlayerInput& input);

void apply_player_input(
    EntitySnapshot& entity,
    const PlayerInput& input,
    float fixed_delta_seconds,
    float move_speed_meters_per_second);

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
