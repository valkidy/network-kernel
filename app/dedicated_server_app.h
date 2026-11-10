#ifndef APP_DEDICATED_SERVER_APP_H_
#define APP_DEDICATED_SERVER_APP_H_

#include <cstdint>

int RunDedicatedServer(
    std::uint16_t port,
    const char* gameplay_catalog_path,
    const char* gameplay_catalog_bundle_path,
    const char* gameplay_catalog_entry_path,
    const char* gameplay_catalog_content_namespace,
    std::uint32_t physics_simulation,
    std::uint32_t physics_workers,
    std::uint32_t actor_blocking,
    const char* netcode_preset,
    std::uint32_t max_players,
    const char* level,
    // The scene template slot 0 starts on when loading a catalog file (a
    // bundle picks it by entry path); -1 for the catalog's own start_scene.
    std::int32_t start_scene = -1);

#endif  // APP_DEDICATED_SERVER_APP_H_
