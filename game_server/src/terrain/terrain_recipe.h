// A terrain recipe as bytes (implementation plan P3): what the bundle carries
// so that every client composes the same map (plan §3.2, T1 / T2).
//
// NKTR v1, little-endian:
//   char[4] "NKTR", u16 version = 1, u16 reserved = 0
//   u64 library_hash      (terrain_library_hash: stamps + ground maps, in order)
//   u64 heights_hash      (height_grid_hash of the composed map)
//   i32 seed, i32 cells_x, i32 cells_z, u32 ground_map, u8 ground_orientation, u8[3] 0
//   terrace: i32 levels, i32 level_height_mm, i32 black_mm, i32 riser_run_mm,
//            i32 cliff_width_mm, i64 cliff_share_q16, i32 cliff_wavelength_mm
//   detail:  i32 amplitude_mm, i32 wavelength_mm
//   i64 ring_tan_q16
//   u32 placement count, then per placement:
//            u32 stamp, i32 x_cells, i32 z_cells, i32 base_cm, i32 ring_mm, u8 turns, u8 mirror, u8[2] 0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_compose.h"

namespace network_example::game_server::terrain {

// FNV-1a over every stamp's and ground map's name, size and samples, in
// library order. A recipe only means something against the library it was made
// from; a client with a different library refuses it instead of composing a
// different map.
std::uint64_t terrain_library_hash(const TerrainLibrary& library);

struct StoredRecipe {
    TerrainRecipe recipe;
    std::uint64_t library_hash = 0;
    std::uint64_t heights_hash = 0;
};

std::vector<std::uint8_t> write_recipe(const StoredRecipe& stored);
bool parse_recipe(std::span<const std::uint8_t> bytes, StoredRecipe* out, std::string* error);

}  // namespace network_example::game_server::terrain
