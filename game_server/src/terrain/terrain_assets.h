// Terrain asset formats (implementation plan P1.2). Both are written by the
// Unity authoring tools in unity-terrain-example (claude/stamp-export) or cooked
// from their output; the runtime never decodes PNG.
//
// NKHF v1 -- a heightfield on the 0.3 m grid (StampExporter.cs), little-endian:
//   0  char[4] "NKHF"
//   4  u16     version = 1
//   6  u16     flags: bit 0 = border on the water line (a stamp)
//   8  u32     cells_x        (samples_x = cells_x + 1)
//   12 u32     cells_z
//   16 u32     spacing_mm = 300
//   20 u32     reserved = 0
//   24 i16[samples_z][samples_x]  cm; row j is z = j * 0.3 m, column i is x.
// A stamp (flags bit 0) stores heights relative to its own border (border = 0)
// and must have every border sample at 0. A map dump (flags 0) stores
// absolute heights.
//
// NKGM v1 -- a painted ground height map on the 1.2 m grid, cooked from
// GroundMapExporter.cs's 16-bit PNG (tools/terrain/ground_png_to_nkgm.py):
//   0  char[4] "NKGM"
//   4  u16     version = 1
//   6  u16     flags = 0
//   8  u32     samples_x
//   12 u32     samples_z
//   16 u32     spacing_mm = 1200
//   20 u32     reserved = 0
//   24 u16[samples_z][samples_x]  gray (0 black .. 65535 white); row j is
//                                 z = j * 1.2 m (south first, unlike the PNG).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_types.h"

namespace network_example::game_server::terrain {

struct StampAsset {
    std::string name;
    std::int32_t cells_x = 0;
    std::int32_t cells_z = 0;
    std::vector<std::int16_t> cm;  // (cells_z + 1) rows of (cells_x + 1)

    std::int32_t samples_x() const { return cells_x + 1; }
    std::int16_t at(std::int32_t i, std::int32_t j) const {
        return cm[static_cast<std::size_t>(j) * static_cast<std::size_t>(samples_x()) +
                  static_cast<std::size_t>(i)];
    }
};

struct GroundMapAsset {
    std::string name;
    std::int32_t samples_x = 0;
    std::int32_t samples_z = 0;
    std::vector<std::uint16_t> gray;
};

// A stamp: flags bit 0 set, edges a multiple of 12 m, every border sample 0.
bool parse_stamp(std::span<const std::uint8_t> bytes, std::string name, StampAsset* out, std::string* error);

// Any NKHF heightfield (a stamp or a map dump) as a grid of absolute cm.
bool parse_height_grid(std::span<const std::uint8_t> bytes, HeightGrid* out, std::string* error);

std::vector<std::uint8_t> write_nkhf(const HeightGrid& grid, std::uint16_t flags);

bool parse_ground_map(std::span<const std::uint8_t> bytes, std::string name, GroundMapAsset* out, std::string* error);

std::vector<std::uint8_t> write_nkgm(const GroundMapAsset& map);

}  // namespace network_example::game_server::terrain
