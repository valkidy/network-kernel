#include "simulation/public/movement_solver.h"

#include <algorithm>
#include <cmath>

namespace network_example::movement_solver {

glm::vec3 input_move_to_world(const KernelPlayerInput& input) {
    glm::vec3 move{input.move.x, 0.0f, input.move.y};
    const float length = glm::length(move);
    if (length > 1.0f) {
        move /= length;
    }
    return move;
}

void apply_player_input(
    EntitySnapshot& entity,
    const KernelPlayerInput& input,
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

glm::vec3 ground_following_velocity(
    const glm::vec3& desired_horizontal_velocity,
    physics::CharacterGroundState ground_state,
    const glm::vec3& ground_normal,
    float max_slope_degrees,
    float previous_vertical_velocity,
    float gravity_y,
    float fixed_delta_seconds) {
    glm::vec3 velocity = desired_horizontal_velocity;
    // The slope limit, not a token epsilon. Below it the division's gain is
    // unbounded, and the ground state alone does not keep the normal above it.
    const float walkable_normal_y = std::cos(
        std::clamp(max_slope_degrees, 0.0f, 89.9f) *
        3.14159265358979323846f / 180.0f);
    if (ground_state == physics::CharacterGroundState::kGrounded &&
        ground_normal.y >= walkable_normal_y && walkable_normal_y > 0.0f) {
        // Keep authored X/Z speed when Jolt projects onto walkable ground.
        const float ground_dot_horizontal =
            ground_normal.x * velocity.x + ground_normal.z * velocity.z;
        velocity.y = -ground_dot_horizontal / ground_normal.y;
    } else {
        velocity.y = previous_vertical_velocity +
            gravity_y * fixed_delta_seconds;
    }
    return velocity;
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

    const glm::vec3 velocity = ground_following_velocity(
        desired_horizontal_velocity,
        state->ground_state,
        state->ground_normal,
        config.max_slope_degrees,
        state->velocity.y,
        config.gravity.y,
        fixed_delta_seconds);
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
    // Vertical speed is what the move actually made good, where that is less.
    // Jolt hands back the velocity it was given, whatever blocked it, and off
    // walkable ground the next step adds gravity to that -- so a character held
    // in place gained 9.81 m/s every second it stood there. It happens wherever
    // the "ground" Jolt reports is not a floor: two gingerbread released on the
    // same spot report each other as ground, with a horizontal normal, and were
    // measured at -327 m/s after 33 s standing at y = 0. A ceiling stops a rise
    // the same way. Only ever toward zero: a step up or a snap down moves the
    // body further than its velocity did, and that is not speed.
    glm::vec3 resolved_velocity = result.linear_velocity;
    if (fixed_delta_seconds > 0.0f) {
        const float made_good =
            (result.position.y - state->position.y) / fixed_delta_seconds;
        if (resolved_velocity.y < 0.0f && made_good > resolved_velocity.y) {
            resolved_velocity.y = std::min(0.0f, made_good);
        } else if (resolved_velocity.y > 0.0f && made_good < resolved_velocity.y) {
            resolved_velocity.y = std::max(0.0f, made_good);
        }
    }
    state->position = result.position;
    state->velocity = resolved_velocity;
    state->ground_state = result.ground_state;
    state->ground_normal = result.ground_normal;
    state->supporting_identity = result.supporting_identity;
    return true;
}

}  // namespace network_example::movement_solver
