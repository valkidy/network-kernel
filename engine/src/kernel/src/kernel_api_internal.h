#ifndef KERNEL_SRC_KERNEL_API_INTERNAL_H_
#define KERNEL_SRC_KERNEL_API_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KernelCommandSource {
    KernelCommandSource_Internal = 0,
    KernelCommandSource_PlayerInput = 1,
    KernelCommandSource_AI = 2,
    KernelCommandSource_ControlPlane = 3,
    KernelCommandSource_Test = 4,
} KernelCommandSource;

typedef enum KernelEntityLifecycleCommandType {
    KernelEntityLifecycleCommandType_Destroy = 1,
    /*
     * Reserved for future async create support. Current enqueue lifecycle
     * implementation rejects Create because create needs result/net_id handling.
     */
    KernelEntityLifecycleCommandType_Create = 2,
} KernelEntityLifecycleCommandType;

typedef struct KernelEntityLifecycleCommand {
    uint32_t struct_size;
    uint32_t command_type;
    uint32_t net_id;
    uint32_t reason;
    /*
     * Reserved for future KernelEntityLifecycleCommandType_Create support.
     * Current version only supports Destroy and ignores create_info.
     */
    KernelServerEntityCreateInfo create_info;
} KernelEntityLifecycleCommand;

/*
 * Internal/experimental game_server migration bridge. Enqueues a lifecycle
 * command for the next simulation tick. Current version supports Destroy only;
 * Create is reserved for future async result/net_id handling.
 */
bool Kernel_ServerEnqueueEntityLifecycle(
    KernelHandle* kernel,
    uint32_t command_source,
    const KernelEntityLifecycleCommand* command);

/*
 * Internal/experimental game_server migration bridge. Enqueues non-lifecycle
 * entity mutation commands for the next simulation tick.
 */
bool Kernel_ServerEnqueueEntityTransform(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation);

bool Kernel_ServerEnqueueEntityVelocity(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const KernelVec3* velocity);

bool Kernel_ServerEnqueueEntityState(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags);

/*
 * Internal/experimental game_server migration bridge. Enqueues authoritative
 * entity input for the next simulation tick.
 */
bool Kernel_ServerEnqueueEntityInput(
    KernelHandle* kernel,
    uint32_t command_source,
    uint32_t net_id,
    const PlayerInput* input);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // KERNEL_SRC_KERNEL_API_INTERNAL_H_
