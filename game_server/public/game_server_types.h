#ifndef GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
#define GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_types.h"

#define GAME_SERVER_ABI_VERSION 5u

#define GAME_SERVER_CAPABILITY_ENEMY_MANAGER UINT64_C(0x0000000000000001)
#define GAME_SERVER_CAPABILITY_EVENT_HANDLING UINT64_C(0x0000000000000002)
#define GAME_SERVER_CAPABILITY_DESPAWN_ALL UINT64_C(0x0000000000000004)
#define GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY UINT64_C(0x0000000000000008)
#define GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY UINT64_C(0x0000000000000010)
#define GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE UINT64_C(0x0000000000000020)
/* The loadout game messages below. */
#define GAME_SERVER_CAPABILITY_LOADOUT_MESSAGES UINT64_C(0x0000000000000040)
/* GameServer_GetTerrainInfo / CopyTerrainHeights / CopyTerrainPlacements /
 * ReleaseTerrain: a generated terrain level's map, for the client to draw. */
#define GAME_SERVER_CAPABILITY_TERRAIN_QUERY UINT64_C(0x0000000000000080)
/* GameServer_GetStaticSceneMeshInfo / CopyStaticSceneMesh: a mesh level's
 * baked static collision scene as triangles, for the client to draw. */
#define GAME_SERVER_CAPABILITY_STATIC_SCENE_MESH_QUERY UINT64_C(0x0000000000000100)
/* The scene table (docs/SCENE_TABLE_PLAN.md S4): the SCENE_* game messages
 * below, GameServer_ClientLoadSceneSlot / ClientUnloadSceneSlot,
 * GameServer_GetSceneTemplateCount / GetSceneTemplate, and the
 * per-slot GameServer_GetSceneTerrainInfo / CopySceneTerrainHeights /
 * CopySceneTerrainPlacements / GetSceneStaticMeshInfo / CopySceneStaticMesh. */
#define GAME_SERVER_CAPABILITY_SCENE_SLOTS UINT64_C(0x0000000000000200)

/*
 * Game messages game_server sends and reads (KernelGameMessage::message_type;
 * see Kernel_SendGameMessage). Bodies are little-endian and packed, with no
 * padding. A message whose body does not have exactly the layout below is
 * answered with GAME_SERVER_LOADOUT_RESULT_MALFORMED or ignored.
 *
 * LOADOUT_OFFERS, server -> client. Sent when the player goes inside a
 * loadout camp (a camp is a building: activating it takes the player in, and
 * again brings it out; coming out sends nothing). The client opens the UI
 * from its shelter state (ui_id 2); this fills it.
 *   u32 camp_net_id
 *   u8  slot_capacity        how many item options the player may pick
 *   u8  option_count         item options, 1..32
 *   option_count x { u32 item_template_id; u16 quantity; }
 *   u8  weapon_option_count  weapon options, 0..32
 *   weapon_option_count x { u32 item_template_id; u8 category; }
 *   u8  current_count        the player's current item picks, 0 = the default
 *   current_count x { u32 item_template_id; u16 quantity; }
 *   u8  current_weapon_count the player's current weapon picks
 *   current_weapon_count x u32 item_template_id
 *
 * LOADOUT_SELECT, client -> server. Picks by index into the camp's lists. An
 * item index may repeat, each pick filling one inventory slot; at most one
 * weapon per category. No picks at all restores the default; item picks with
 * no weapon picks leave the player unarmed.
 *   u32 camp_net_id
 *   u8  pick_count           0..slot_capacity
 *   pick_count x u8 option_index
 *   u8  weapon_pick_count    0..4
 *   weapon_pick_count x u8 weapon_option_index
 *
 * LOADOUT_RESULT, server -> client. The answer to every LOADOUT_SELECT.
 *   u32 camp_net_id
 *   u8  result               GAME_SERVER_LOADOUT_RESULT_*
 *   u8  pick_count           item picks applied (0 unless APPLIED)
 *   u8  weapon_pick_count    weapon picks applied (0 unless APPLIED)
 *
 * An applied loadout replaces the player's inventory and weapons at once and
 * is what every later respawn gives them. It lasts until they leave.
 */
#define GAME_SERVER_MESSAGE_LOADOUT_OFFERS 1u
#define GAME_SERVER_MESSAGE_LOADOUT_SELECT 2u
#define GAME_SERVER_MESSAGE_LOADOUT_RESULT 3u

#define GAME_SERVER_LOADOUT_RESULT_APPLIED 0u
#define GAME_SERVER_LOADOUT_RESULT_MALFORMED 1u
#define GAME_SERVER_LOADOUT_RESULT_NOT_A_CAMP 2u
/* The player is not inside the camp the pick names. */
#define GAME_SERVER_LOADOUT_RESULT_OUT_OF_RANGE 3u
#define GAME_SERVER_LOADOUT_RESULT_TOO_MANY_PICKS 4u
#define GAME_SERVER_LOADOUT_RESULT_BAD_OPTION 5u
#define GAME_SERVER_LOADOUT_RESULT_DEAD 6u
#define GAME_SERVER_LOADOUT_RESULT_APPLY_FAILED 7u
#define GAME_SERVER_LOADOUT_RESULT_CATEGORY_TAKEN 8u

/*
 * The scene table's messages (docs/SCENE_TABLE_PLAN.md §5). Same encoding as
 * the loadout messages: little-endian, packed.
 *
 * SCENE_STATE, server -> client. The whole state, sent to a player when they
 * join and to everyone whenever it changes; the client reconciles to it
 * rather than following steps, so a late joiner needs nothing else.
 *   u8  flow                 GAME_SERVER_SCENE_FLOW_*
 *   u8  level_slot           the party's level slot, 0 for none
 *   u32 level_generation
 *   u8  outcome              GAME_SERVER_SCENE_OUTCOME_*: how the last level ended
 *   u8  gate_open            1 while a gate is open (camp -> level in
 *                            GATE_OPEN, level -> camp in RETURN_OPEN)
 *   f32 gate_x, gate_y, gate_z, gate_radius   world space
 *   u32 owner_net_id         the player who may open and abort levels: a
 *                            listen host's own player, or on a dedicated
 *                            server the connected player with the lowest
 *                            peer id; 0 with nobody connected
 *   u16 enemies_defeated     towards the level's objective
 *   u16 enemies_target       0 when the level has no such objective
 *   u8  slot_count           level slots the client must have loaded, 0..7
 *   slot_count x {
 *     u8  slot
 *     u32 generation
 *     u16 template_id        its scene template's id
 *     u8  source             GAME_SERVER_SCENE_SOURCE_*: in the bundle, or a
 *                            recipe the client must first fetch
 *     u64 recipe_hash        heights hash of that recipe; 0 for the bundle
 *     u8  name_length        1..32
 *     name_length bytes      the template's name, no terminator
 *   }
 *   A slot is listed once the server has it ready. A client loads each listed
 *   slot it does not have at that generation (BeginLoad, after PumpSceneRecipe
 *   for a recipe), answers SCENE_READY once it has, and unloads any it has
 *   that is not listed.
 *
 * SCENE_READY, client -> server. The slot's collision is in this client's
 * prediction world (art may still be loading).
 *   u8  slot
 *   u32 generation
 *
 * SCENE_SELECT, client -> server. Asks for a level. Obeyed only from the
 * owner (see owner_net_id), and only while the party is in the camp.
 *   u16 template_id
 *
 * SCENE_ABORT, client -> server. Ends the party's level at once: everyone in
 * it comes back to the camp alive and it unloads (outcome ABORTED). Obeyed
 * only from the owner. Empty body.
 *
 * SCENE_STATE is also re-sent every few seconds, and until a send succeeds.
 *
 * SCENE_RECIPE_CHUNK, server -> client. One piece of a slot's recipe. Hand
 * these to GameServer_ClientAcceptSceneMessage; nothing else reads them.
 *   u8  slot; u32 generation; u32 transfer_id; u16 chunk_index; u16 chunk_count
 *   the rest of the body: that chunk's bytes
 *
 * SCENE_RECIPE_REQUEST, client -> server. Send me this slot's recipe, whole.
 * GameServer_ClientPumpSceneRecipe sends it, and again if it does not arrive.
 *   u8  slot; u32 generation
 *
 * SCENE_LOAD_FAILED, client -> server. This client could not build the slot.
 * It stays out of the level; the party does not wait for it.
 *   u8  slot; u32 generation; u8 reason
 */
#define GAME_SERVER_MESSAGE_SCENE_STATE 4u
#define GAME_SERVER_MESSAGE_SCENE_READY 5u
#define GAME_SERVER_MESSAGE_SCENE_SELECT 6u
#define GAME_SERVER_MESSAGE_SCENE_ABORT 7u
#define GAME_SERVER_MESSAGE_SCENE_RECIPE_CHUNK 8u
#define GAME_SERVER_MESSAGE_SCENE_RECIPE_REQUEST 9u
#define GAME_SERVER_MESSAGE_SCENE_LOAD_FAILED 10u

#define GAME_SERVER_SCENE_SOURCE_BUNDLE 0u
#define GAME_SERVER_SCENE_SOURCE_RECIPE 1u

/* GameServer_ClientPumpSceneRecipe and PollSceneSlotLoad. */
#define GAME_SERVER_SCENE_LOAD_PENDING 0u
#define GAME_SERVER_SCENE_LOAD_READY 1u
#define GAME_SERVER_SCENE_LOAD_FAILED 2u

#define GAME_SERVER_SCENE_FLOW_CAMP 0u
#define GAME_SERVER_SCENE_FLOW_LOADING 1u
#define GAME_SERVER_SCENE_FLOW_GATE_OPEN 2u
#define GAME_SERVER_SCENE_FLOW_RETURN_OPEN 3u

#define GAME_SERVER_SCENE_OUTCOME_NONE 0u
#define GAME_SERVER_SCENE_OUTCOME_COMPLETED 1u
#define GAME_SERVER_SCENE_OUTCOME_FAILED 2u
/* The level never loaded. */
#define GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED 3u
/* The owner ended it, nobody entered in time, or everyone left. */
#define GAME_SERVER_SCENE_OUTCOME_ABORTED 4u

/* GameServer_ClientLoadSceneSlot flags. */
/* Add the slot's collision to the kernel (Kernel_AddStaticSceneSlot). A pure
 * client sets it; a listen host does not, because its server side already
 * added the slot to both of its worlds. */
#define GAME_SERVER_SCENE_LOAD_REGISTER_COLLISION 0x1u
/* Compose from the recipe PumpSceneRecipe fetched, not from the bundle. */
#define GAME_SERVER_SCENE_LOAD_FROM_RECIPE 0x2u

#ifdef __cplusplus
extern "C" {
#endif

/* One level a catalog offers (GameServer_GetSceneTemplate). */
typedef struct GameServerSceneTemplateInfo {
    uint32_t struct_size;
    uint32_t template_id;
    char name[33];
    char display_name[64];
    char description[256];
} GameServerSceneTemplateInfo;

typedef struct GameServerAbiInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t weapon_template_info_size;
    uint32_t gameplay_catalog_load_result_size;
    uint64_t capability_flags;
} GameServerAbiInfo;

typedef struct GameServerWeaponTemplateInfo {
    uint32_t struct_size;
    uint8_t weapon_id;
    uint8_t fire_mode;
    char name[64];
    KernelWeaponMechanicsDefinition mechanics;
    uint32_t valid;
} GameServerWeaponTemplateInfo;

/*
 * A generated terrain level (catalog `terrain:`), as composed when the catalog
 * was loaded into a kernel -- by Kernel_LoadGameplayCatalogFromMemory on a
 * client, or GameServer_CreateWithGameplayCatalogFromMemory on a Host. The
 * same heights the kernel collides with, so the client draws what players walk
 * on.
 *
 * The heightfield is samples_x * samples_z heights in cm on a cell_mm grid:
 * sample (i, j) is at world x = origin_x + i * cell_mm / 1000, z = origin_z +
 * j * cell_mm / 1000 (the map is centred on the world origin). Rows run south
 * to north (j = 0 first), columns west to east. Kernel world axes: no flip.
 * A collision mesh built from it should split each cell along the (i, j) ->
 * (i + 1, j + 1) diagonal, as the kernel's heightfield does.
 */
typedef struct GameServerTerrainInfo {
    uint32_t struct_size;
    uint32_t samples_x;
    uint32_t samples_z;
    uint32_t cell_mm;
    float origin_x;
    float origin_z;
    int32_t min_height_cm;
    int32_t max_height_cm;
    uint64_t heights_hash;
    int32_t seed;
    uint32_t placement_count;
} GameServerTerrainInfo;

/*
 * One stamp the recipe placed: which template, where its footprint lies in the
 * world (after turning; square for now), the height its border sits at, and
 * how it was turned -- quarter turns as TerrainMapComposer.Compose counts them,
 * mirror = the stamp's own x flipped before the turn. Enough to put the stamp's
 * render prefab (buildings, props) where its collision is.
 */
typedef struct GameServerTerrainPlacement {
    uint32_t struct_size;
    uint32_t stamp_template_id;
    float min_x;
    float min_z;
    float size_x;
    float size_z;
    float base_y;
    uint8_t turns;
    uint8_t mirror;
    uint8_t reserved[2];
} GameServerTerrainPlacement;

/*
 * A mesh level's static collision scene (catalog static_collision_scene
 * entry_path: a baked .joltmesh), as the kernel collides with it: welded
 * vertices in world space (kernel axes, no flip) and three indices per
 * triangle, in the order that is the front face (cross(b - a, c - a) points
 * out of the surface). Bounds are of the vertices.
 */
typedef struct GameServerStaticSceneMeshInfo {
    uint32_t struct_size;
    uint32_t vertex_count;
    uint32_t triangle_count;
    float min_x;
    float min_y;
    float min_z;
    float max_x;
    float max_y;
    float max_z;
} GameServerStaticSceneMeshInfo;

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
