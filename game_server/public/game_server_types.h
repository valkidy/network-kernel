#ifndef GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
#define GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_types.h"

#define GAME_SERVER_ABI_VERSION 4u

#define GAME_SERVER_CAPABILITY_ENEMY_MANAGER UINT64_C(0x0000000000000001)
#define GAME_SERVER_CAPABILITY_EVENT_HANDLING UINT64_C(0x0000000000000002)
#define GAME_SERVER_CAPABILITY_DESPAWN_ALL UINT64_C(0x0000000000000004)
#define GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_DIRECTORY UINT64_C(0x0000000000000008)
#define GAME_SERVER_CAPABILITY_WEAPON_TEMPLATE_QUERY UINT64_C(0x0000000000000010)
#define GAME_SERVER_CAPABILITY_GAMEPLAY_CATALOG_BUNDLE UINT64_C(0x0000000000000020)

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
