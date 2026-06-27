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

KERNEL_RPC_STRUCT(R"json({"type":"KernelRpcEntityStateView"})json")
typedef struct KernelRpcEntityStateView {
    uint32_t struct_size;
    uint32_t net_id;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    KernelVec3 velocity;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t animation_state;
    uint32_t visual_flags;
    uint32_t valid;
    uint32_t actor_template_id;
} KernelRpcEntityStateView;

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.ping",
  "authority":"developer_read_only",
  "phase":"immediate_read_only"
})json")
bool KernelRpc_DevPing(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.list_methods",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "result_schema":{
    "type":"object",
    "properties":{"methods":{"type":"array","items":{"type":"object"}}},
    "required":["methods"],
    "additionalProperties":false
  }
})json")
bool KernelRpc_DevListMethods(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.describe_method",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "params_schema":{
    "type":"object",
    "properties":{"method":{"type":"string"}},
    "required":["method"],
    "additionalProperties":false
  },
  "result_schema":{"type":"object"}
})json")
bool KernelRpc_DevDescribeMethod(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.get_schema",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "result_schema":{
    "type":"object",
    "properties":{"schema":{"type":"object"}},
    "required":["schema"],
    "additionalProperties":false
  }
})json")
bool KernelRpc_DevGetSchema(KernelHandle* kernel);

KERNEL_RPC_INTERNAL(R"json({
  "method":"world.get_entity_state",
  "authority":"developer_read_only",
  "phase":"immediate_read_only",
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"}
  ]
})json")
bool KernelRpc_GetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelRpcEntityStateView* out_state);

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
