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
void GameServer_Destroy(GameServerHandle* game_server);

void GameServer_HandleEvent(
    GameServerHandle* game_server,
    const KernelEvent* event);

void GameServer_Tick(GameServerHandle* game_server, float delta_seconds);

uint32_t GameServer_GetEnemyCount(GameServerHandle* game_server);

void GameServer_DespawnAll(GameServerHandle* game_server, uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_API_H_
