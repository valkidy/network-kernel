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

/*
 * Game messages game_server sends and reads (KernelGameMessage::message_type;
 * see Kernel_SendGameMessage). Bodies are little-endian and packed, with no
 * padding. A message whose body does not have exactly the layout below is
 * answered with GAME_SERVER_LOADOUT_RESULT_MALFORMED or ignored.
 *
 * LOADOUT_OFFERS, server -> client. Sent when the player activates a loadout
 * camp; it is also the client's cue to open the loadout UI.
 *   u32 camp_net_id
 *   u8  slot_capacity        how many options the player may pick
 *   u8  option_count         1..32
 *   option_count x { u32 item_template_id; u16 quantity; }
 *   u8  current_count        the player's current loadout, 0 = the default
 *   current_count x { u32 item_template_id; u16 quantity; }
 *
 * LOADOUT_SELECT, client -> server. The options picked, by index into the
 * camp's option list; an index may repeat. Each pick fills one inventory slot.
 *   u32 camp_net_id
 *   u8  pick_count           0..slot_capacity; 0 restores the default
 *   pick_count x u8 option_index
 *
 * LOADOUT_RESULT, server -> client. The answer to every LOADOUT_SELECT.
 *   u32 camp_net_id
 *   u8  result               GAME_SERVER_LOADOUT_RESULT_*
 *   u8  pick_count           how many picks were applied (0 unless APPLIED)
 *
 * An applied loadout replaces the player's inventory at once and is what every
 * later respawn gives them. It lasts until they leave the session.
 */
#define GAME_SERVER_MESSAGE_LOADOUT_OFFERS 1u
#define GAME_SERVER_MESSAGE_LOADOUT_SELECT 2u
#define GAME_SERVER_MESSAGE_LOADOUT_RESULT 3u

#define GAME_SERVER_LOADOUT_RESULT_APPLIED 0u
#define GAME_SERVER_LOADOUT_RESULT_MALFORMED 1u
#define GAME_SERVER_LOADOUT_RESULT_NOT_A_CAMP 2u
#define GAME_SERVER_LOADOUT_RESULT_OUT_OF_RANGE 3u
#define GAME_SERVER_LOADOUT_RESULT_TOO_MANY_PICKS 4u
#define GAME_SERVER_LOADOUT_RESULT_BAD_OPTION 5u
#define GAME_SERVER_LOADOUT_RESULT_DEAD 6u
#define GAME_SERVER_LOADOUT_RESULT_APPLY_FAILED 7u

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
