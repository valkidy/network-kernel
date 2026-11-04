// A composed heightfield as the kernel's static collision scene
// (implementation plan P1.7): square Jolt HeightFieldShape tiles in one
// StaticCompoundShape, serialized with SaveWithChildren and wrapped in the
// 88 byte NKMJOLT1 header that PhysicsWorld::load_static_scene validates.
// load_static_scene restores any Jolt shape (sRestoreWithChildren), so this
// needs no new kernel export: it goes through KernelStaticCollisionSceneConfig
// like a baked joltmesh.
//
// Tiles: tile_cells x tile_cells cells (48 m by default), neighbours sharing
// their edge row of samples; the last tile in a row or column is shorter when
// the map edge is not a multiple of the tile. Jolt rounds a tile's sample count
// up to its block size and fills the padding with no-collision samples, so a
// 161-sample tile needs no special handling (T7).
//
// Coordinates: sample (i, j) lands at world x = i * 0.3 m - origin, z likewise,
// with origin = half the map edge -- the map centre is the world origin (plan
// §9). The heights are the grid's cm / 100.
//
// Unlike compose, this is float work inside Jolt and is not part of the hash.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_types.h"

namespace network_example::game_server::terrain {

struct TerrainArtifactOptions {
    std::int32_t tile_cells = 160;          // 48 m
    std::uint32_t block_size = 8;           // Jolt HeightFieldShapeSettings::mBlockSize, 2-8; 8 is smallest (1.64 B/sample at 1 km)
    std::uint32_t bits_per_sample = 8;      // ... ::mBitsPerSample, 1-16; 8 keeps the surface within 1.7 cm of the int16 heights
};

struct TerrainArtifactStats {
    std::uint32_t tiles = 0;
    std::uint64_t payload_bytes = 0;
};

// World position of sample 0 (the map's low corner): minus half the map edge.
float terrain_world_origin_x(const HeightGrid& grid);
float terrain_world_origin_z(const HeightGrid& grid);

bool build_static_scene_artifact(const HeightGrid& grid, const TerrainArtifactOptions& options,
    std::vector<std::uint8_t>* artifact, TerrainArtifactStats* stats, std::string* error);

}  // namespace network_example::game_server::terrain
