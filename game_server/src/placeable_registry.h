#ifndef GAME_SERVER_PLACEABLE_REGISTRY_H_
#define GAME_SERVER_PLACEABLE_REGISTRY_H_

#include <cstdint>
#include <span>
#include <string>

#include "game_server/src/gameplay_config.h"

namespace network_example::game_server {

// FNV-1a 64 over a bundle's bytes: which bundle a registry was made from.
std::uint64_t bundle_bytes_hash(std::span<const std::uint8_t> bundle);

// What a level editor may place (docs/LEVEL_AUTHORING_TOOLS_PLAN.md §3.3, P4):
// every prop entity template the catalog defines -- the only kind scene_props
// accepts -- by name, with its id at this catalog version, whether it lands on
// the ground by default, and the box its collider fills, for drawing it before
// any model exists. Names are what an editor stores; ids are renumbered when
// branches merge, so they only tie this file to the bundle it came from.
//
// Deterministic: sorted by name, fixed float formatting, so two builds of the
// same bundle write the same bytes.
std::string placeable_registry_json(
    const GameServerGameplayConfig& config,
    std::uint64_t bundle_hash);

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PLACEABLE_REGISTRY_H_
