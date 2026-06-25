#ifndef SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_
#define SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "sync/public/snapshot.h"

namespace network_example::movement_solver {

glm::vec3 input_move_to_world(const PlayerInput& input);

void apply_player_input(
    EntitySnapshot& entity,
    const PlayerInput& input,
    float fixed_delta_seconds,
    float move_speed_meters_per_second);

}  // namespace network_example::movement_solver

#endif  // SIMULATION_PUBLIC_MOVEMENT_SOLVER_H_
