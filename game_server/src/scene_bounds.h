#ifndef GAME_SERVER_SCENE_BOUNDS_H_
#define GAME_SERVER_SCENE_BOUNDS_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// The x range a scene slot owns, [min_x, max_x) (docs/SCENE_TABLE_PLAN.md S3).
// Default-constructed it owns everything, which is what a game server that
// never loads a level keeps.
struct SceneBounds {
    bool bounded = false;
    float min_x = 0.0f;
    float max_x = 0.0f;

    bool contains(float x) const {
        return !bounded || (x >= min_x && x < max_x);
    }
};

// Keeps the states whose position lies in `bounds`, in order, at the front of
// `states`; returns how many. The scene's directors and controllers read only
// those, so nothing in one slot sees, counts or targets anything in another.
inline std::uint32_t keep_in_scene(
    const SceneBounds& bounds,
    KernelServerEntityState* states,
    std::uint32_t count) {
    if (!bounds.bounded) {
        return count;
    }
    std::uint32_t kept = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (bounds.contains(states[index].position.x)) {
            if (kept != index) states[kept] = states[index];
            ++kept;
        }
    }
    return kept;
}

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SCENE_BOUNDS_H_
