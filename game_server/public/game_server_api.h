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

/*
 * Static scene mesh queries (GAME_SERVER_CAPABILITY_STATIC_SCENE_MESH_QUERY),
 * keyed by kernel like the terrain ones: a catalog whose map is a baked mesh
 * (not a terrain) keeps that mesh for these. False when the kernel's last
 * catalog load had none, or after GameServer_ReleaseTerrain, which frees this
 * copy too. The triangles are taken out of the artifact on the first call.
 */
bool GameServer_GetStaticSceneMeshInfo(
    KernelHandle* kernel,
    GameServerStaticSceneMeshInfo* out_info);
/* position_capacity counts floats (3 per vertex), index_capacity indices (3
 * per triangle); both must hold the whole mesh. */
bool GameServer_CopyStaticSceneMesh(
    KernelHandle* kernel,
    float* out_positions,
    uint32_t position_capacity,
    uint32_t* out_indices,
    uint32_t index_capacity);

/*
 * The levels the catalog last loaded into `kernel` offers, by ascending id,
 * for a level list. A client has them as soon as its catalog is loaded.
 * GetSceneTemplate is false past the end or for a short struct_size.
 */
uint32_t GameServer_GetSceneTemplateCount(KernelHandle* kernel);
bool GameServer_GetSceneTemplate(
    KernelHandle* kernel,
    uint32_t index,
    GameServerSceneTemplateInfo* out_info);

/*
 * The scene table on a client (GAME_SERVER_CAPABILITY_SCENE_SLOTS;
 * docs/SCENE_TABLE_PLAN.md S4), driven by GAME_SERVER_MESSAGE_SCENE_STATE.
 *
 * GameServer_ClientLoadSceneSlot loads `scene_template` (by name) from the
 * synced bundle into level slot `slot` (1 and up): composes its terrain or
 * reads its mesh, at the slot's offset, and keeps it for the per-slot queries
 * below. With GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION it also adds the
 * slot's collision to the kernel (a pure client; a listen host's server side
 * has already done that). It runs on the calling thread: the same thread as
 * Kernel_Update when the flag is set. False, with nothing changed, for slot
 * 0 or past the table, an unknown template, or a kernel that refuses the
 * slot. GameServer_ClientUnloadSceneSlot forgets the slot and, with the same
 * flag, takes its collision out of the kernel.
 */
bool GameServer_ClientLoadSceneSlot(
    KernelHandle* kernel,
    const uint8_t* bundle_bytes,
    uint32_t bundle_size,
    const char* entry_path,
    const char* scene_template,
    uint32_t slot,
    uint32_t flags);
bool GameServer_ClientUnloadSceneSlot(KernelHandle* kernel, uint32_t slot, uint32_t flags);

/*
 * Loading without stopping the game (preferred over ClientLoadSceneSlot).
 *
 * Give every SCENE_RECIPE_CHUNK to ClientAcceptSceneMessage (true when it was
 * one). For a slot whose source is RECIPE, call ClientPumpSceneRecipe each
 * frame until it stops returning PENDING: it asks the server for the recipe,
 * asks again if it does not arrive, checks it against recipe_hash and gives
 * up after several tries.
 *
 * ClientBeginLoadSceneSlot composes on a worker thread (FROM_RECIPE to use
 * the fetched recipe); ClientPollSceneSlotLoad reports it and, on the call
 * that finds it finished, does what must happen on the Kernel_Update thread.
 */
bool GameServer_ClientAcceptSceneMessage(KernelHandle* kernel, const KernelGameMessage* message);
uint32_t GameServer_ClientPumpSceneRecipe(
    KernelHandle* kernel,
    uint32_t slot,
    uint32_t generation,
    uint64_t recipe_hash,
    float delta_seconds);
bool GameServer_ClientBeginLoadSceneSlot(
    KernelHandle* kernel,
    const uint8_t* bundle_bytes,
    uint32_t bundle_size,
    const char* entry_path,
    const char* scene_template,
    uint32_t slot,
    uint32_t generation,
    uint32_t flags);
uint32_t GameServer_ClientPollSceneSlotLoad(KernelHandle* kernel, uint32_t slot, uint32_t generation);

/*
 * The terrain and mesh queries above, for any scene slot: slot 0 is what they
 * already return, a level slot what GameServer_ClientLoadSceneSlot loaded
 * into it. Positions are world space (the slot's offset included).
 * GameServer_ReleaseTerrain frees every slot's copy.
 */
bool GameServer_GetSceneTerrainInfo(
    KernelHandle* kernel, uint32_t slot, GameServerTerrainInfo* out_info);
bool GameServer_CopySceneTerrainHeights(
    KernelHandle* kernel,
    uint32_t slot,
    uint32_t x0,
    uint32_t z0,
    uint32_t width,
    uint32_t depth,
    int16_t* out_cm,
    uint32_t out_count);
uint32_t GameServer_CopySceneTerrainPlacements(
    KernelHandle* kernel,
    uint32_t slot,
    GameServerTerrainPlacement* out_placements,
    uint32_t capacity);
bool GameServer_GetSceneStaticMeshInfo(
    KernelHandle* kernel, uint32_t slot, GameServerStaticSceneMeshInfo* out_info);
bool GameServer_CopySceneStaticMesh(
    KernelHandle* kernel,
    uint32_t slot,
    float* out_positions,
    uint32_t position_capacity,
    uint32_t* out_indices,
    uint32_t index_capacity);

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_API_H_
