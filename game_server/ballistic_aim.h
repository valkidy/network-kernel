#ifndef GAME_SERVER_BALLISTIC_AIM_H_
#define GAME_SERVER_BALLISTIC_AIM_H_

#include <cstdint>
#include <optional>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

struct BallisticAimProfile {
    bool enabled = false;
    float speed = 0.0f;
    KernelVec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t lifetime_ticks = 0;
};

struct BallisticAimSolution {
    KernelVec3 aim_direction{1.0f, 0.0f, 0.0f};
    float flight_seconds = 0.0f;
};

std::optional<BallisticAimSolution> solve_low_ballistic_aim(
    const KernelVec3& launch_position,
    const KernelVec3& target_position,
    float speed,
    const KernelVec3& gravity,
    float max_flight_seconds);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_BALLISTIC_AIM_H_
