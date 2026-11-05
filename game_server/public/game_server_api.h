#ifndef GAME_SERVER_PUBLIC_GAME_SERVER_API_H_
#define GAME_SERVER_PUBLIC_GAME_SERVER_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "game_server/public/game_server_types.h"
#include "kernel/public/kernel_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameServerHandle GameServerHandle;

/*
 * Game server C ABI ownership and lifetime rules:
 * - GameServer_Create returns an opaque handle owned by the caller.
 * - The caller must keep the KernelHandle alive longer than GameServerHandle.
 * - GameServer_Destroy releases the handle; passing NULL is allowed.
 * - Events are copied during GameServer_HandleEvent; the game server does not
 *   retain caller buffer pointers after a function returns.
 * - Handles are single-thread owned unless a future ABI revision states
 *   otherwise.
 * - No C++ exceptions cross this ABI. Failures return NULL, false, or 0.
 */

bool GameServer_GetAbiInfo(
    GameServerAbiInfo* out_info,
    uint32_t out_info_size);

GameServerHandle* GameServer_Create(KernelHandle* kernel);
GameServerHandle* GameServer_CreateWithWeaponTemplateDirectory(
    KernelHandle* kernel,
    const char* template_directory);
GameServerHandle* GameServer_CreateWithGameplayCatalogFromMemory(
    KernelHandle* kernel,
    const uint8_t* bundle_bytes,
    uint32_t bundle_size,
    const char* entry_path,
    KernelGameplayCatalogLoadResult* out_result);
void GameServer_Destroy(GameServerHandle* game_server);

void GameServer_HandleEvent(
    GameServerHandle* game_server,
    const KernelEvent* event);

void GameServer_Tick(GameServerHandle* game_server, float delta_seconds);

uint32_t GameServer_GetEnemyCount(GameServerHandle* game_server);

bool GameServer_QueryWeaponTemplate(
    GameServerHandle* game_server,
    uint8_t weapon_id,
    GameServerWeaponTemplateInfo* out_info);

void GameServer_DespawnAll(GameServerHandle* game_server, uint32_t reason);

/*
 * Terrain queries (GAME_SERVER_CAPABILITY_TERRAIN_QUERY), keyed by the kernel
 * the terrain level's catalog was loaded into. False / 0 when that kernel has
 * no terrain (its last catalog load had none, or ReleaseTerrain was called).
 * Loading another catalog into the kernel replaces what these return; call
 * GameServer_ReleaseTerrain before Kernel_Destroy to free the copy.
 */
bool GameServer_GetTerrainInfo(KernelHandle* kernel, GameServerTerrainInfo* out_info);
/* A rectangle of heights (cm), rows south first; out_count must be at least
 * width * depth and the rectangle inside the map. */
bool GameServer_CopyTerrainHeights(
    KernelHandle* kernel,
    uint32_t x0,
    uint32_t z0,
    uint32_t width,
    uint32_t depth,
    int16_t* out_cm,
    uint32_t out_count);
/* Copies up to capacity placements (each struct_size set by the caller is
 * honoured as sizeof(GameServerTerrainPlacement)); returns how many the map
 * has, so a call with capacity 0 sizes the buffer. */
uint32_t GameServer_CopyTerrainPlacements(
    KernelHandle* kernel,
    GameServerTerrainPlacement* out_placements,
    uint32_t capacity);
void GameServer_ReleaseTerrain(KernelHandle* kernel);

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_API_H_
