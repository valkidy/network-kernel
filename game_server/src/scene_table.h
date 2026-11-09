#ifndef GAME_SERVER_SCENE_TABLE_H_
#define GAME_SERVER_SCENE_TABLE_H_

// The scene table's geometry and its loading step (docs/SCENE_TABLE_PLAN.md
// S3): where each slot sits, which slot a point belongs to, a level's config
// moved to its slot, and the part of a load that touches no kernel and can run
// on a worker thread.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/scene_bounds.h"
#include "game_server/src/terrain_catalog.h"
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
// are left alone. Patrol areas move with the rest; around_players patrols
// follow the players and need nothing.
void offset_scene_config(GameServerGameplayConfig* config, std::uint32_t slot);

// Where a level's catalog comes from: the bundle it was synced from (a
// terrain template's recipe is only there), or a catalog file.
struct SceneCatalogSource {
    std::shared_ptr<const std::vector<std::uint8_t>> bundle;
    std::string entry_path;
    std::string catalog_file;
    // Replaces a seed: -1 terrain's random seed (reproducing a map from the log).
    std::optional<std::int32_t> seed_override;
};

// Height of the map at (x, z), metres; the map is centred on its own origin.
float terrain_ground_height(const terrain::HeightGrid& heights, float x, float z);

// Where a point of a placed stamp lands on the map (y unset), following the
// recipe's turn and mirror.
KernelVec3 stamp_site_on_map(
    const terrain::TerrainRecipe& recipe,
    const terrain::TerrainPlacement& placement,
    const terrain::StampAsset& stamp,
    const StampSiteConfig& site);

// What a generated level derives from its map: the spawn (the map's centre,
// unless authored), the return gate and the nest sites, all on the ground.
void place_level_content(
    GameServerGameplayConfig* config,
    const terrain::HeightGrid& heights,
    const terrain::TerrainRecipe& recipe);

// A level ready to commit: its config, already moved to its slot (navmesh
// artifact included), and the collision artifact the kernel slot takes.
struct PreparedScene {
    bool ok = false;
    std::string error;
    GameServerGameplayConfig config;
    std::vector<std::uint8_t> collision_artifact;
    // A terrain level's composed heights and recipe, for a client to draw.
    terrain::HeightGrid terrain_heights;
    terrain::StoredRecipe terrain_recipe;
    // The NKTR recipe when this map was generated here rather than read from
    // the bundle: what clients must be sent. Empty otherwise.
    std::vector<std::uint8_t> recipe_bytes;
};

// Loads `scene_template` for `slot`: the template's config, its map (a
// terrain composed from its cooked recipe, or a mesh scene and its navmesh
// read from the bundle or next to the catalog file), offset to the slot.
// Touches no kernel, so it may run on a worker thread.
// A server (kServer) also gets the terrain's navmesh; a client (kClient)
// only composes.
PreparedScene prepare_scene(
    const SceneCatalogSource& source,
    const std::string& scene_template,
    std::uint32_t slot,
    TerrainRole role = TerrainRole::kServer,
    std::span<const std::uint8_t> recipe = {});

// A recipe in game messages (SCENE_RECIPE_CHUNK), and back. Reliable delivery
// is GNS's; what these handle is a transfer that restarts, is replaced by a
// newer one, or is abandoned half way.
inline constexpr std::size_t kSceneRecipeChunkBytes = 480u;

std::vector<std::vector<std::uint8_t>> encode_scene_recipe_chunks(
    std::uint32_t slot,
    std::uint32_t generation,
    std::uint32_t transfer_id,
    std::span<const std::uint8_t> recipe);

class SceneRecipeAssembler {
public:
    // False for a malformed chunk. A chunk of an older generation or transfer
    // than the one being assembled is ignored; a newer one replaces it.
    bool accept(std::span<const std::uint8_t> body);
    // The whole recipe, once every chunk of the slot's transfer has arrived.
    const std::vector<std::uint8_t>* complete(std::uint32_t slot, std::uint32_t generation) const;
    void forget(std::uint32_t slot);

private:
    struct Transfer {
        std::uint32_t generation = 0;
        std::uint32_t transfer_id = 0;
        std::uint32_t received = 0;
        std::vector<std::vector<std::uint8_t>> chunks;
        std::vector<std::uint8_t> bytes;
    };
    std::map<std::uint32_t, Transfer> transfers_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SCENE_TABLE_H_
