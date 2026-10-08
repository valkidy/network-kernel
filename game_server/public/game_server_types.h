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
