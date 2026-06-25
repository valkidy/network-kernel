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

}  // namespace network_example::movement_solver
