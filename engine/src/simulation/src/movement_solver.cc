#include "simulation/public/movement_solver.h"

namespace network_example::movement_solver {

glm::vec3 input_move_to_world(const PlayerInput& input) {
    glm::vec3 move{input.move.x, 0.0f, input.move.y};
    const float length = glm::length(move);
    if (length > 1.0f) {
        move /= length;
    }
    return move;
}

void apply_player_input(
    EntitySnapshot& entity,
    const PlayerInput& input,
    float fixed_delta_seconds,
    float move_speed_meters_per_second) {
    entity.velocity = input_move_to_world(input) * move_speed_meters_per_second;
    entity.position += entity.velocity * fixed_delta_seconds;
}

bool reset_character(
    physics::PhysicsWorld& physics_world,
    const CharacterMovementConfig& config,
    std::string* error) {
    physics_world.remove_character(config.character_id);
    physics::CharacterDescriptor descriptor{};
    descriptor.character_id = config.character_id;
    descriptor.shape = config.shape;
    descriptor.max_slope_degrees = config.max_slope_degrees;
    return physics_world.upsert_character(descriptor, error);
}

bool step_character(
    physics::PhysicsWorld& physics_world,
    const CharacterMovementConfig& config,
    const glm::vec3& desired_horizontal_velocity,
    float fixed_delta_seconds,
    CharacterMovementState* state,
    std::string* error) {
    if (state == nullptr) {
        if (error != nullptr) {
            *error = "missing character movement state";
        }
        return false;
    }
    physics::CharacterDescriptor descriptor{};
    descriptor.character_id = config.character_id;
    descriptor.shape = config.shape;
    descriptor.max_slope_degrees = config.max_slope_degrees;
    if (!physics_world.upsert_character(descriptor, error)) {
        return false;
    }

    glm::vec3 velocity = desired_horizontal_velocity;
    velocity.y = state->ground_state == physics::CharacterGroundState::kGrounded
        ? 0.0f
        : state->velocity.y + config.gravity.y * fixed_delta_seconds;
    physics::CharacterMoveRequest request{};
    request.character_id = config.character_id;
    request.current_position = state->position;
    request.current_rotation = state->rotation;
    request.linear_velocity = velocity;
    request.gravity = config.gravity;
    request.delta_seconds = fixed_delta_seconds;
    request.step_height = config.step_height;
    request.ground_snap_distance = config.ground_snap_distance;
    request.filter = config.filter;
    physics::CharacterMoveResult result{};
    if (!physics_world.move_character(request, &result, error)) {
        return false;
    }
    state->position = result.position;
    state->velocity = result.linear_velocity;
    state->ground_state = result.ground_state;
    state->ground_normal = result.ground_normal;
    state->supporting_identity = result.supporting_identity;
    return true;
}

}  // namespace network_example::movement_solver
