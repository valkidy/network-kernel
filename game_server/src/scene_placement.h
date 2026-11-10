#ifndef GAME_SERVER_SCENE_PLACEMENT_H_
#define GAME_SERVER_SCENE_PLACEMENT_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain/terrain_types.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// What a scene's ground is, for laying things out against it: a composed
// terrain's heights (the map centred on its own origin), or the triangles of a
// mesh scene's collision artifact. Exactly one is used; heights win.
struct SceneGround {
    const terrain::HeightGrid* heights = nullptr;
    std::span<const std::uint8_t> mesh_artifact;
};

// Finishes what a scene template authors against its ground
// (docs/LEVEL_AUTHORING_TOOLS_PLAN.md G1/G2), in the scene's own coordinates:
// - every `grounded` scene prop's y becomes the ground under it;
// - resolved_player_barriers becomes the authored player_barriers plus, with
//   map_edge_barrier, walls along the map's rectangle.
// False, with *error, for a grounded prop with no ground under it, a mesh that
// does not read, or more barriers than a slot takes. Run it wherever the
// scene's collision is prepared, server and client alike, so both ends lay out
// the same walls.
bool finish_scene_placement(
    GameServerGameplayConfig* config,
    const SceneGround& ground,
    std::string* error);

// Hands config.resolved_player_barriers to the kernel for `slot` (nothing to do
// when there are none). After the slot's static scene is registered. Inline so
// only its callers -- which link the kernel already -- reference the export;
// the catalog loaders that use finish_scene_placement link without a kernel.
inline bool register_player_barriers(
    KernelHandle* kernel,
    std::uint32_t slot,
    const GameServerGameplayConfig& config) {
    if (config.resolved_player_barriers.empty()) {
        return true;
    }
    KernelStaticScenePlayerBarriersConfig barriers{};
    barriers.struct_size = sizeof(barriers);
    barriers.slot = slot;
    barriers.boxes = config.resolved_player_barriers.data();
    barriers.box_count = static_cast<std::uint32_t>(config.resolved_player_barriers.size());
    return Kernel_SetStaticScenePlayerBarriers(kernel, &barriers);
}

// The map-edge walls alone, for a rectangle and a list of ground points
// (x, y, z); exposed for tests.
std::vector<KernelPlayerBarrierBox> map_edge_barriers(
    const MapEdgeBarrierConfig& rule,
    float min_x,
    float min_z,
    float max_x,
    float max_z,
    const std::vector<KernelVec3>& ground_points);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SCENE_PLACEMENT_H_
