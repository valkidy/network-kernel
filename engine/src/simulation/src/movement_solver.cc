#include "simulation/public/movement_solver.h"

#include <cmath>

#include <spdlog/spdlog.h>

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

    // Captured before the move overwrites state, so the diagnostic below can
    // report what the formula was actually fed.
    const bool entry_grounded =
        state->ground_state == physics::CharacterGroundState::kGrounded;
    const float request_ground_normal_y = state->ground_normal.y;

    glm::vec3 velocity = desired_horizontal_velocity;
    if (state->ground_state == physics::CharacterGroundState::kGrounded &&
        state->ground_normal.y > 0.001f) {
        // Keep authored X/Z speed when Jolt projects onto walkable ground.
        const float ground_dot_horizontal =
            state->ground_normal.x * velocity.x +
            state->ground_normal.z * velocity.z;
        velocity.y = -ground_dot_horizontal / state->ground_normal.y;
    } else {
        velocity.y = state->velocity.y + config.gravity.y * fixed_delta_seconds;
    }
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
    // [CharacterMove] Diagnostic, anomalies only -- this runs for every
    // character every tick, so a heartbeat here would drown everything else.
    //
    // The velocity.y above is slope-following: it solves n . v = 0, so its gain
    // is tan(slope) and it diverges as the ground approaches vertical. The only
    // guard on it is ground_normal.y > 0.001, which leaves a 1000x window open.
    // That window is supposed to be unreachable, because Jolt only reports
    // OnGround within max_slope_degrees, which bounds n.y at cos(max_slope).
    // So an "inconsistent" line below means the assumption is false and the
    // amplifier is live; a "jump" line with a healthy normal means the position
    // was moved by Jolt itself (penetration recovery or stair walking) and the
    // formula is innocent.
    const float entry_normal_y = request_ground_normal_y;
    const float slope_limit_y = std::cos(
        config.max_slope_degrees * 3.14159265358979323846f / 180.0f);
    const float position_step_y = result.position.y - request.current_position.y;
    const bool jumped = std::abs(position_step_y) > 0.5f;
    const bool fast_vertical = std::abs(velocity.y) > 6.0f;
    const bool inconsistent = entry_grounded &&
        entry_normal_y < slope_limit_y - 0.01f;
    if (jumped || fast_vertical || inconsistent) {
        spdlog::warn(
            "[CharacterMove] net_id={} {}{}{} pos=({:.3f},{:.3f},{:.3f}) "
            "dy={:.3f} fed_vy={:.3f} out_vy={:.3f} "
            "in_ground={} in_ny={:.4f} out_ground={} out_ny={:.4f} "
            "slope_limit_ny={:.4f}",
            config.character_id,
            jumped ? "JUMP " : "",
            fast_vertical ? "FASTVY " : "",
            inconsistent ? "INCONSISTENT " : "",
            request.current_position.x,
            request.current_position.y,
            request.current_position.z,
            position_step_y,
            velocity.y,
            result.linear_velocity.y,
            static_cast<int>(state->ground_state),
            entry_normal_y,
            static_cast<int>(result.ground_state),
            result.ground_normal.y,
            slope_limit_y);
    }

    state->position = result.position;
    state->velocity = result.linear_velocity;
    state->ground_state = result.ground_state;
    state->ground_normal = result.ground_normal;
    state->supporting_identity = result.supporting_identity;
    return true;
}

}  // namespace network_example::movement_solver
