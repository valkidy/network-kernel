#ifndef KERNEL_PUBLIC_KERNEL_API_H_
#define KERNEL_PUBLIC_KERNEL_API_H_

#include <stdbool.h>
#include <stdint.h>

#include "kernel/public/kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KernelHandle KernelHandle;
typedef struct KernelLANDiscoveryHandle KernelLANDiscoveryHandle;
typedef uint64_t KernelRpcRequestId;

/*
 * C ABI ownership and lifetime rules:
 * - Kernel_Create returns an opaque handle owned by the caller.
 * - Kernel_Destroy releases that handle; passing NULL is allowed.
 * - Callers own all input and output buffers passed to this API.
 * - Output buffer contents are copied during the call; the kernel does not
 *   retain caller buffer pointers after a function returns.
 * - Handles are single-thread owned unless a future ABI revision states
 *   otherwise.
 * - No C++ exceptions cross this ABI. Failures return NULL, false, or 0.
 */

bool Kernel_GetAbiInfo(KernelAbiInfo* out_info, uint32_t out_info_size);
bool Kernel_GetBuildInfo(KernelBuildInfo* out_info, uint32_t out_info_size);
bool Kernel_GetLocalPlayerInfo(
    KernelHandle* kernel,
    KernelLocalPlayerInfo* out_info);

KernelLANDiscoveryHandle* Kernel_LANDiscovery_Create(void);
void Kernel_LANDiscovery_Destroy(KernelLANDiscoveryHandle* discovery);

bool Kernel_LANDiscovery_StartServer(
    KernelLANDiscoveryHandle* discovery,
    const KernelLANDiscoveryServerConfig* config);

void Kernel_LANDiscovery_StopServer(KernelLANDiscoveryHandle* discovery);

bool Kernel_LANDiscovery_Query(
    KernelLANDiscoveryHandle* discovery,
    const KernelLANDiscoveryQueryConfig* config);

uint32_t Kernel_LANDiscovery_PollResults(
    KernelLANDiscoveryHandle* discovery,
    KernelLANDiscoveryResult* out_results,
    uint32_t max_results);

void Kernel_LANDiscovery_ClearResults(KernelLANDiscoveryHandle* discovery);

KernelHandle* Kernel_Create(const KernelConfig* config);
void Kernel_Destroy(KernelHandle* kernel);

bool Kernel_SetPhysicsConfig(
    KernelHandle* kernel,
    const KernelPhysicsConfig* config);

bool Kernel_SetSessionRules(
    KernelHandle* kernel,
    const KernelSessionRulesConfig* config);

bool Kernel_SetStaticCollisionScene(
    KernelHandle* kernel,
    const KernelStaticCollisionSceneConfig* config);

bool Kernel_InvokeRpcCommand(
    KernelHandle* kernel,
    const char* request_json,
    uint32_t request_json_size,
    KernelRpcRequestId* out_request_id);

bool Kernel_PollRpcResponse(
    KernelHandle* kernel,
    KernelRpcRequestId request_id,
    char* out_response_json,
    uint32_t response_json_capacity,
    uint32_t* out_response_json_size);

bool Kernel_StartClient(KernelHandle* kernel, const char* address);
bool Kernel_StartClientCatalogSync(
    KernelHandle* kernel,
    const char* address,
    const KernelGameplayCatalogSyncClientConfig* config);
bool Kernel_StartListenServer(KernelHandle* kernel, uint16_t port);
bool Kernel_StartDedicatedServer(KernelHandle* kernel, uint16_t port);

bool Kernel_SetGameplayCatalogSyncBundle(
    KernelHandle* kernel,
    const KernelGameplayCatalogSyncServerConfig* config,
    KernelGameplayCatalogManifest* out_manifest);

bool Kernel_GetGameplayCatalogSyncStatus(
    KernelHandle* kernel,
    KernelGameplayCatalogSyncStatus* out_status);

bool Kernel_RequestGameplayCatalogBundle(KernelHandle* kernel);

bool Kernel_CopyGameplayCatalogBundle(
    KernelHandle* kernel,
    uint8_t* out_bundle,
    uint32_t out_capacity,
    uint32_t* out_bundle_size);

bool Kernel_ContinueClientHandshake(KernelHandle* kernel);

void Kernel_Update(KernelHandle* kernel, float delta_seconds);

void Kernel_SubmitInput(
    KernelHandle* kernel,
    uint32_t local_player_id,
    const PlayerInput* input);

bool Kernel_LoadGameplayCatalog(
    KernelHandle* kernel,
    const KernelGameplayCatalogDefinition* catalog,
    const KernelGameplayCatalogLoadOptions* options);

bool Kernel_LoadGameplayCatalogFromMemory(
    KernelHandle* kernel,
    const uint8_t* bundle_bytes,
    uint32_t bundle_size,
    const char* entry_path,
    KernelGameplayCatalogLoadResult* out_result);

uint32_t Kernel_GetRenderStates(
    KernelHandle* kernel,
    RenderEntityState* out_states,
    uint32_t max_states);

uint32_t Kernel_GetRenderStatesAtTime(
    KernelHandle* kernel,
    uint64_t client_render_time_us,
    RenderEntityState* out_states,
    uint32_t max_states);

uint32_t Kernel_PollEvents(
    KernelHandle* kernel,
    KernelEvent* out_events,
    uint32_t max_events);

uint32_t Kernel_PollEntityLifecycleEvents(
    KernelHandle* kernel,
    KernelEntityLifecycleEvent* out_events,
    uint32_t max_events);

uint32_t Kernel_PollLocalActionResults(
    KernelHandle* kernel,
    KernelLocalActionResult* out_results,
    uint32_t max_results);

uint32_t Kernel_PollRemoteActionPresentationEvents(
    KernelHandle* kernel,
    KernelRemoteActionPresentationEvent* out_events,
    uint32_t max_events);

bool Kernel_GetBenchmarkStats(
    KernelHandle* kernel,
    KernelBenchmarkStats* out_stats);

bool Kernel_GetNetworkStats(
    KernelHandle* kernel,
    KernelNetworkStats* out_stats);

uint32_t Kernel_PollDebugRecords(
    KernelHandle* kernel,
    const KernelDebugRecordFilter* filter,
    KernelDebugInfo* out_records,
    uint32_t max_records);

uint32_t Kernel_QueryColliderShapes(
    KernelHandle* kernel,
    const KernelColliderShapeQuery* query,
    KernelColliderShapeView* out_shapes,
    uint32_t max_shapes);

/*
 * Queries runtime perception state on host/server kernels and template-derived
 * visual-debug state on pure clients when replicated actors carry an actor
 * template id with vision config.
 */
uint32_t Kernel_QueryVisionState(
    KernelHandle* kernel,
    const KernelVisionStateQuery* query,
    KernelVisionStateView* out_states,
    uint32_t max_states);

uint32_t Kernel_GetProjectileTemplates(
    KernelHandle* kernel,
    KernelProjectileTemplateDefinition* out_templates,
    uint32_t max_templates);

bool Kernel_GetActionTemplate(
    KernelHandle* kernel,
    uint32_t action_template_id,
    KernelActionTemplateDefinition* out_definition);

uint32_t Kernel_GetActorTemplates(
    KernelHandle* kernel,
    KernelActorTemplateDefinition* out_templates,
    uint32_t max_templates);

uint32_t Kernel_GetColliderTemplates(
    KernelHandle* kernel,
    KernelColliderTemplateDefinition* out_templates,
    uint32_t max_templates);

uint32_t Kernel_GetColliderBindings(
    KernelHandle* kernel,
    KernelColliderBindingDefinition* out_bindings,
    uint32_t max_bindings);

KERNEL_RPC(R"json({
  "method":"world.create_entity",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"create_info","type":"KernelServerEntityCreateInfo","passing":"const_ptr"}
  ]
})json")
bool Kernel_ServerCreateEntity(
    KernelHandle* kernel,
    const KernelServerEntityCreateInfo* create_info,
    uint32_t* out_net_id);

bool Kernel_ServerCreateInventoryContainer(
    KernelHandle* kernel,
    uint32_t owner_entity_id,
    uint32_t slot_capacity,
    KernelInventoryContainerId* out_container_id);

bool Kernel_ServerCreateInventoryItem(
    KernelHandle* kernel,
    uint32_t item_template_id,
    uint32_t quantity,
    KernelInventoryContainerId container_id,
    KernelItemInstanceId* out_item_instance_id);

bool Kernel_ServerCreateWorldItem(
    KernelHandle* kernel,
    uint32_t item_template_id,
    uint32_t quantity,
    const KernelVec3* position,
    KernelItemInstanceId* out_item_instance_id,
    uint32_t* out_prop_entity_id);

bool Kernel_ServerSubmitGameplayRequest(
    KernelHandle* kernel,
    const KernelGameplayRequest* request);

bool Kernel_SubmitGameplayRequest(
    KernelHandle* kernel,
    const KernelGameplayRequest* request);

bool Kernel_GetItemInstance(
    KernelHandle* kernel,
    KernelItemInstanceId item_instance_id,
    KernelItemInstanceView* out_view);

bool Kernel_GetInventoryContainer(
    KernelHandle* kernel,
    KernelInventoryContainerId container_id,
    KernelInventoryContainerView* out_view);

uint32_t Kernel_CopyInventorySlots(
    KernelHandle* kernel,
    KernelInventoryContainerId container_id,
    KernelItemInstanceView* out_items,
    uint32_t max_items);

uint32_t Kernel_PollGameplayRequestOutcomes(
    KernelHandle* kernel,
    KernelGameplayRequestOutcome* out_outcomes,
    uint32_t max_outcomes);

uint32_t Kernel_PollInventoryDeltas(
    KernelHandle* kernel,
    KernelInventoryContainerId container_id,
    KernelInventoryDelta* out_deltas,
    uint32_t max_deltas);

KERNEL_RPC(R"json({
  "method":"world.destroy_entity",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"reason","type":"uint32_t","passing":"value"}
  ]
})json")
bool Kernel_ServerDestroyEntity(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t reason);

KERNEL_RPC(R"json({
  "method":"world.set_transform",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"position","type":"KernelVec3","passing":"const_ptr"},
    {"name":"rotation","type":"KernelQuat","passing":"const_ptr"}
  ]
})json")
bool Kernel_ServerSetEntityTransform(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* position,
    const KernelQuat* rotation);

KERNEL_RPC(R"json({
  "method":"world.set_velocity",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"velocity","type":"KernelVec3","passing":"const_ptr"}
  ]
})json")
bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity);

KERNEL_RPC(R"json({
  "method":"world.set_entity_state",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"animation_state","type":"uint16_t","passing":"value"},
    {"name":"visual_flags","type":"uint32_t","passing":"value"}
  ]
})json")
bool Kernel_ServerSetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t animation_state,
    uint32_t visual_flags);

KERNEL_RPC(R"json({
  "method":"world.set_entity_health",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "audit":true,
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"hp","type":"uint16_t","passing":"value"}
  ]
})json")
bool Kernel_ServerSetEntityHealth(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t hp);

bool Kernel_ServerSubmitEntityInput(
    KernelHandle* kernel,
    uint32_t net_id,
    const PlayerInput* input);

bool Kernel_ServerSetEntityCombatState(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelCombatStateDefinition* combat_state);

bool Kernel_ServerSetEntityActorTemplate(
    KernelHandle* kernel,
    uint32_t net_id,
    uint32_t actor_template_id);

bool Kernel_ServerSetEntityVisionConfig(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelAgentVisionConfig* vision_config);

bool Kernel_ServerClearEntityVisionConfig(
    KernelHandle* kernel,
    uint32_t net_id);

bool Kernel_ServerSetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelWeaponMechanicsDefinition* weapon_mechanics);

bool Kernel_ServerClearEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id);

bool Kernel_ServerGetEntityWeaponMechanics(
    KernelHandle* kernel,
    uint32_t net_id,
    uint8_t weapon_id,
    KernelWeaponMechanicsDefinition* out_weapon_mechanics);

bool Kernel_ServerGetHomingState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelHomingState* out_state);

bool Kernel_ServerValidateMechanicsConfig(
    const KernelWeaponMechanicsDefinition* weapon_mechanics);

bool Kernel_ServerGetEntityState(
    KernelHandle* kernel,
    uint32_t net_id,
    KernelServerEntityState* out_state);

uint32_t Kernel_ServerQueryEntities(
    KernelHandle* kernel,
    uint16_t entity_type_filter,
    KernelServerEntityState* out_states,
    uint32_t max_states);

#ifdef __cplusplus
}
#endif

#endif  // KERNEL_PUBLIC_KERNEL_API_H_
