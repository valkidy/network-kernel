#ifndef GAME_SERVER_SCENE_TABLE_H_
#define GAME_SERVER_SCENE_TABLE_H_

// The scene table's geometry and its loading step (docs/SCENE_TABLE_PLAN.md
// S3): where each slot sits, which slot a point belongs to, a level's config
// moved to its slot, and the part of a load that touches no kernel and can run
// on a worker thread.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_bounds.h"
#include "kernel/public/kernel_types.h"

namespace network_example::game_server {

// Slots sit along +x, kSceneSlotSpacing apart (Q1): more than the largest map
// (1440 m) plus the relevance distance, so nothing in one slot is ever seen,
// hit or targeted from another.
constexpr float kSceneSlotSpacing = 4096.0f;
constexpr std::uint32_t kSceneSlotCount = KERNEL_STATIC_SCENE_SLOT_COUNT;

KernelVec3 scene_slot_offset(std::uint32_t slot);
SceneBounds scene_slot_bounds(std::uint32_t slot);
// The slot whose range holds x, or kSceneSlotCount past the table.
std::uint32_t scene_slot_of(float x);

// Moves a level's config to `slot`: every world-space position it carries --
// player spawn, scene props and items, agent routes, playable bounds, the
// mission's spawn points and its world-space routes and guard anchors, world
// rule spawn points and anchors -- gains the slot's offset, and scene_offset
// records it. Positions stored relative to something (a world rule's route,
// which is offsets from each agent's spawn; a spawner's, from its carrier)
// are left alone. Patrols are cleared: a level runs what its template's
// directors spawn, not the catalog's world patrols.
void offset_scene_config(GameServerGameplayConfig* config, std::uint32_t slot);

// Where a level's catalog comes from: the bundle it was synced from (a
// terrain template's recipe is only there), or a catalog file.
struct SceneCatalogSource {
    std::shared_ptr<const std::vector<std::uint8_t>> bundle;
    std::string entry_path;
    std::string catalog_file;
};

// A level ready to commit: its config, already moved to its slot (navmesh
// artifact included), and the collision artifact the kernel slot takes.
struct PreparedScene {
    bool ok = false;
    std::string error;
    GameServerGameplayConfig config;
    std::vector<std::uint8_t> collision_artifact;
};

// Loads `scene_template` for `slot`: the template's config, its map (a
// terrain composed from its cooked recipe, or a mesh scene and its navmesh
// read from the bundle or next to the catalog file), offset to the slot.
// Touches no kernel, so it may run on a worker thread.
PreparedScene prepare_scene(
    const SceneCatalogSource& source,
    const std::string& scene_template,
    std::uint32_t slot);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SCENE_TABLE_H_
