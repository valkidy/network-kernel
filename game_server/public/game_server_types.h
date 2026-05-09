#ifndef GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
#define GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_

#include <stdint.h>

#define GAME_SERVER_ABI_VERSION 1u

#define GAME_SERVER_CAPABILITY_ENEMY_MANAGER UINT64_C(0x0000000000000001)
#define GAME_SERVER_CAPABILITY_EVENT_HANDLING UINT64_C(0x0000000000000002)
#define GAME_SERVER_CAPABILITY_DESPAWN_ALL UINT64_C(0x0000000000000004)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameServerAbiInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t capability_flags;
} GameServerAbiInfo;

#ifdef __cplusplus
}
#endif

#endif  // GAME_SERVER_PUBLIC_GAME_SERVER_TYPES_H_
