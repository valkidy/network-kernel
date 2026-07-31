#ifndef KERNEL_PUBLIC_KERNEL_TYPES_H_
#define KERNEL_PUBLIC_KERNEL_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_ABI_VERSION 60u

#ifndef KERNEL_RPC
#define KERNEL_RPC(metadata)
#define KERNEL_RPC_INTERNAL(metadata)
#define KERNEL_RPC_STRUCT(metadata)
#endif

#define KERNEL_BUILD_INFO_TEXT_SIZE 128u
#define KERNEL_LAN_DISCOVERY_TEXT_SIZE 128u
#define KERNEL_LAN_DISCOVERY_DEFAULT_PORT 47777u
#define KERNEL_GAMEPLAY_CATALOG_ENTRY_PATH_SIZE 128u
#define KERNEL_GAMEPLAY_CATALOG_CONTENT_NAMESPACE_SIZE 64u
#define KERNEL_GAMEPLAY_CATALOG_SHA256_SIZE 32u
#define KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE UINT32_C(1048576)
#define KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_MAX_BUNDLE_SIZE \
    KERNEL_GAMEPLAY_CATALOG_SYNC_MAX_BUNDLE_SIZE
#define KERNEL_GAMEPLAY_CATALOG_SYNC_DEFAULT_TIMEOUT_MS UINT32_C(30000)
#define KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES UINT32_C(16777216)
#define KERNEL_STATIC_COLLISION_LAYER_TERRAIN UINT32_C(1)

#define KERNEL_MAX_WEAPON_SLOTS 4u

#define KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED UINT32_C(0)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS UINT32_C(1)

#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_NONE UINT32_C(0)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARGUMENT UINT32_C(1)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_YAML UINT32_C(2)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION UINT32_C(3)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN_FIELD UINT32_C(4)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_REQUIRED_FIELD UINT32_C(5)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_FIELD_TYPE UINT32_C(6)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ENUM_VALUE UINT32_C(7)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_ID UINT32_C(8)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_TEMPLATE_NAME UINT32_C(9)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_TEMPLATE_REFERENCE UINT32_C(10)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_NUMERIC_RANGE UINT32_C(11)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_INVALID_ARCHIVE_PATH UINT32_C(12)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_DUPLICATE_ARCHIVE_ENTRY UINT32_C(13)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_MISSING_BUNDLE_ENTRY UINT32_C(14)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_KERNEL_REJECTED_CATALOG UINT32_C(15)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN UINT32_C(255)

#define KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_UNKNOWN UINT32_C(0)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_FILESYSTEM UINT32_C(1)
#define KERNEL_GAMEPLAY_CATALOG_LOAD_SOURCE_BUNDLE UINT32_C(2)

#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_UNKNOWN UINT32_C(0)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_CATALOG UINT32_C(1)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_WEAPON UINT32_C(2)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_PROJECTILE UINT32_C(3)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTOR UINT32_C(4)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_COLLIDER UINT32_C(5)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ACTION UINT32_C(6)
#define KERNEL_GAMEPLAY_CATALOG_TEMPLATE_KIND_ITEM UINT32_C(7)

#define KERNEL_CAPABILITY_CLIENT_MODE UINT64_C(0x0000000000000001)
#define KERNEL_CAPABILITY_LISTEN_SERVER_MODE UINT64_C(0x0000000000000002)
#define KERNEL_CAPABILITY_DEDICATED_SERVER_MODE UINT64_C(0x0000000000000004)
#define KERNEL_CAPABILITY_INPUT_SUBMISSION UINT64_C(0x0000000000000008)
#define KERNEL_CAPABILITY_RENDER_STATES UINT64_C(0x0000000000000010)
#define KERNEL_CAPABILITY_EVENT_POLLING UINT64_C(0x0000000000000020)
#define KERNEL_CAPABILITY_CLIENT_PREDICTION UINT64_C(0x0000000000000040)
#define KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION UINT64_C(0x0000000000000080)
#define KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN UINT64_C(0x0000000000000100)
#define KERNEL_CAPABILITY_LOCAL_PLAYER_INFO UINT64_C(0x0000000000000200)
#define KERNEL_CAPABILITY_SERVER_ENTITY_CREATE UINT64_C(0x0000000000000400)
#define KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY UINT64_C(0x0000000000000800)
#define KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE UINT64_C(0x0000000000001000)
#define KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE UINT64_C(0x0000000000002000)
#define KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE UINT64_C(0x0000000000004000)
#define KERNEL_CAPABILITY_SERVER_ENTITY_QUERY UINT64_C(0x0000000000008000)
#define KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER UINT64_C(0x0000000000010000)
#define KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE UINT64_C(0x0000000000020000)
#define KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME UINT64_C(0x0000000000040000)
#define KERNEL_CAPABILITY_RENDER_STATES_AT_TIME UINT64_C(0x0000000000080000)
#define KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG UINT64_C(0x0000000000100000)
#define KERNEL_CAPABILITY_WEAPON_METADATA_QUERY UINT64_C(0x0000000000200000)
#define KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS UINT64_C(0x0000000000800000)
#define KERNEL_CAPABILITY_HOMING_PROJECTILES UINT64_C(0x0000000002000000)
#define KERNEL_CAPABILITY_LAN_DISCOVERY UINT64_C(0x0000000004000000)
#define KERNEL_CAPABILITY_GAMEPLAY_CATALOG UINT64_C(0x0000000008000000)
#define KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH UINT64_C(0x0000000010000000)
#define KERNEL_CAPABILITY_DEBUG_RECORDS UINT64_C(0x0000000020000000)
#define KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY UINT64_C(0x0000000040000000)
#define KERNEL_CAPABILITY_BENCHMARK_STATS UINT64_C(0x0000000080000000)
#define KERNEL_CAPABILITY_NETWORK_STATS UINT64_C(0x0000000100000000)
#define KERNEL_CAPABILITY_ENTITY_LIFECYCLE_EVENTS UINT64_C(0x0000000200000000)
#define KERNEL_CAPABILITY_VISION_STATE_QUERY UINT64_C(0x0000000400000000)
#define KERNEL_CAPABILITY_GAMEPLAY_CATALOG_SYNC UINT64_C(0x0000000800000000)
#define KERNEL_CAPABILITY_CONTROL_PLANE_RPC UINT64_C(0x0000001000000000)
#define KERNEL_CAPABILITY_ACTION_TIMELINE UINT64_C(0x0000002000000000)
#define KERNEL_CAPABILITY_LOCAL_ACTION_RESULTS UINT64_C(0x0000004000000000)
#define KERNEL_CAPABILITY_REMOTE_ACTION_PRESENTATION UINT64_C(0x0000008000000000)
#define KERNEL_CAPABILITY_ACTION_INTENTS UINT64_C(0x0000010000000000)
#define KERNEL_CAPABILITY_ITEM_PROP_SYSTEM UINT64_C(0x0000020000000000)

#define KERNEL_COLLISION_LAYER_PLAYER_SIDE UINT32_C(0x00000001)
#define KERNEL_COLLISION_LAYER_HOSTILE_SIDE UINT32_C(0x00000002)
#define KERNEL_COLLISION_LAYER_PROJECTILE UINT32_C(0x00000004)
#define KERNEL_COLLISION_LAYER_AGENT_VISION UINT32_C(0x00000010)
#define KERNEL_COLLISION_LAYER_NEUTRAL UINT32_C(0x00000020)
#define KERNEL_COLLISION_LAYER_TERRAIN UINT32_C(0x00000040)
#define KERNEL_COLLISION_LAYER_STATIC_OBSTACLE UINT32_C(0x00000080)
#define KERNEL_COLLISION_MASK_NONE UINT32_C(0x00000000)
#define KERNEL_COLLISION_MASK_DAMAGEABLE \
    (KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_HOSTILE_SIDE | \
     KERNEL_COLLISION_LAYER_NEUTRAL)
#define KERNEL_COLLISION_MASK_ACTOR KERNEL_COLLISION_MASK_DAMAGEABLE
#define KERNEL_COLLISION_MASK_STATIC_WORLD \
    (KERNEL_COLLISION_LAYER_TERRAIN | \
     KERNEL_COLLISION_LAYER_STATIC_OBSTACLE)

/*
 * Visual flags are composable presentation hints, never gameplay authority.
 * Zero means that no flags are active, not that presentation state is unknown.
 * Published bits retain their meaning for the lifetime of the ABI.
 * Bits 0-7 are kernel core state/validity; bits 8-15 are action presentation.
 *
 * Animation state       Kernel API condition
 * --------------------  ----------------------------------------------------
 * Moving                visual_flags & KERNEL_VISUAL_FLAG_MOVING
 * Reloading             visual_flags & KERNEL_VISUAL_FLAG_RELOADING
 * Dead                  visual_flags & KERNEL_VISUAL_FLAG_DEAD
 * Aiming                visual_flags & KERNEL_VISUAL_FLAG_AIMING
 * Firing                visual_flags & KERNEL_VISUAL_FLAG_FIRING, or
 *                       action.phase == KernelActionPhase_Active
 * Windup                action.phase == KernelActionPhase_Windup
 * Recovery              action.phase == KernelActionPhase_Recovery
 * Idle                  No Moving, Reloading, Dead, or Firing flag, and
 *                       action.phase == KernelActionPhase_None
 *
 * animation_state remains available for ABI compatibility, but should not be
 * used to derive Idle, Moving, or Firing presentation.
 */
#define KERNEL_VISUAL_FLAG_MOVING UINT32_C(0x00000001)
#define KERNEL_VISUAL_FLAG_RELOADING UINT32_C(0x00000002)
#define KERNEL_VISUAL_FLAG_DEAD UINT32_C(0x00000004)
#define KERNEL_VISUAL_FLAG_HP_UNKNOWN UINT32_C(0x00000008)
#define KERNEL_VISUAL_FLAG_GROUNDED UINT32_C(0x00000010)
#define KERNEL_VISUAL_FLAG_FALLING UINT32_C(0x00000020)
#define KERNEL_VISUAL_FLAG_LANDED UINT32_C(0x00000040)
#define KERNEL_VISUAL_FLAG_AIMING UINT32_C(0x00000100)
#define KERNEL_VISUAL_FLAG_FIRING UINT32_C(0x00000200)

#define KERNEL_MAX_VISIBLE_HOSTILES 16u
#define KERNEL_MAX_VISIBLE_ALLIES 16u
#define KERNEL_MAX_VISIBLE_NEUTRALS 16u

#define KERNEL_ENTITY_COMPONENT_TRANSFORM UINT32_C(0x00000001)
#define KERNEL_ENTITY_COMPONENT_VELOCITY UINT32_C(0x00000002)
#define KERNEL_ENTITY_COMPONENT_HEALTH UINT32_C(0x00000004)
#define KERNEL_ENTITY_COMPONENT_WEAPON_STATE UINT32_C(0x00000008)
#define KERNEL_ENTITY_COMPONENT_HITBOX UINT32_C(0x00000010)
#define KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME UINT32_C(0x00000020)
#define KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME UINT32_C(0x00000040)
#define KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME UINT32_C(0x00000080)
#define KERNEL_ENTITY_COMPONENT_SERVER_ONLY UINT32_C(0x00000100)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KernelAbiInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t kernel_config_size;
    uint32_t player_input_size;
    uint32_t render_entity_state_size;
    uint32_t kernel_event_size;
    uint32_t local_player_info_size;
    uint32_t server_entity_create_info_size;
    uint32_t server_entity_state_size;
    uint32_t weapon_mechanics_definition_size;
    uint32_t projectile_mechanics_definition_size;
    uint32_t area_effect_mechanics_definition_size;
    uint32_t beam_mechanics_definition_size;
    uint32_t combat_state_definition_size;
    uint32_t homing_mechanics_definition_size;
    uint32_t homing_state_size;
    uint32_t lan_discovery_server_config_size;
    uint32_t lan_discovery_query_config_size;
    uint32_t lan_discovery_result_size;
    uint64_t capability_flags;
    uint32_t gameplay_catalog_definition_size;
    uint32_t prop_population_rule_definition_size;
    uint32_t gameplay_catalog_load_result_size;
    uint32_t gameplay_catalog_load_options_size;
    uint32_t actor_template_definition_size;
    uint32_t projectile_template_definition_size;
    uint32_t collider_template_definition_size;
    uint32_t collider_binding_definition_size;
    uint32_t benchmark_stats_size;
    uint32_t network_stats_config_size;
    uint32_t network_stats_size;
    uint32_t debug_record_filter_size;
    uint32_t debug_info_size;
    uint32_t collider_shape_query_size;
    uint32_t collider_shape_view_size;
    uint32_t agent_vision_config_size;
    uint32_t vision_state_query_size;
    uint32_t vision_state_view_size;
    uint32_t gameplay_catalog_manifest_size;
    uint32_t gameplay_catalog_sync_status_size;
    uint32_t entity_template_definition_size;
    uint32_t entity_ai_definition_size;
    uint32_t action_template_definition_size;
    uint32_t action_runtime_view_size;
    uint32_t local_action_result_size;
    uint32_t remote_action_presentation_event_size;
    uint32_t action_intent_size;
    uint32_t action_input_size;
    uint32_t item_template_definition_size;
    uint32_t gameplay_request_size;
    uint32_t gameplay_request_outcome_size;
    uint32_t item_instance_view_size;
    uint32_t inventory_container_view_size;
    uint32_t inventory_delta_size;
} KernelAbiInfo;

typedef struct KernelBuildInfo {
    uint32_t struct_size;
    char module_name[KERNEL_BUILD_INFO_TEXT_SIZE];
    char module_file_name[KERNEL_BUILD_INFO_TEXT_SIZE];
    char module_version[KERNEL_BUILD_INFO_TEXT_SIZE];
    uint32_t protocol_version;
    uint32_t snapshot_schema_version;
    uint32_t packet_schema_version;
    char git_commit[KERNEL_BUILD_INFO_TEXT_SIZE];
    char build_timestamp[KERNEL_BUILD_INFO_TEXT_SIZE];
    char build_platform[KERNEL_BUILD_INFO_TEXT_SIZE];
    char build_config[KERNEL_BUILD_INFO_TEXT_SIZE];
    char compiler_info[KERNEL_BUILD_INFO_TEXT_SIZE];
} KernelBuildInfo;

typedef struct KernelLocalPlayerInfo {
    uint32_t peer_id;
    uint32_t player_net_id;
    uint32_t has_welcome;
    uint32_t connected;
} KernelLocalPlayerInfo;

typedef struct KernelLANDiscoveryServerConfig {
    uint32_t struct_size;
    uint16_t discovery_port;
    uint16_t server_endpoint_port;
    char server_name[KERNEL_LAN_DISCOVERY_TEXT_SIZE];
} KernelLANDiscoveryServerConfig;

typedef struct KernelLANDiscoveryQueryConfig {
    uint32_t struct_size;
    uint16_t discovery_port;
    uint32_t timeout_ms;
} KernelLANDiscoveryQueryConfig;

typedef struct KernelLANDiscoveryResult {
    uint32_t struct_size;
    char server_name[KERNEL_LAN_DISCOVERY_TEXT_SIZE];
    char server_endpoint_ip[KERNEL_LAN_DISCOVERY_TEXT_SIZE];
    uint16_t server_endpoint_port;
    char module_version[KERNEL_BUILD_INFO_TEXT_SIZE];
    uint32_t protocol_version;
    uint32_t snapshot_schema_version;
    uint32_t packet_schema_version;
    char git_commit[KERNEL_BUILD_INFO_TEXT_SIZE];
    uint32_t compatible;
} KernelLANDiscoveryResult;

typedef enum KernelMode {
    KernelMode_Client = 0,
    KernelMode_ListenServer = 1,
    KernelMode_DedicatedServer = 2,
} KernelMode;

typedef enum KernelEventType {
    KernelEventType_Connected = 0,
    KernelEventType_Disconnected = 1,
    KernelEventType_PlayerJoined = 2,
    KernelEventType_PlayerLeft = 3,
    KernelEventType_EntitySpawned = 4,
    KernelEventType_EntityDestroyed = 5,
    KernelEventType_FireConfirmed = 6,
    KernelEventType_HitConfirmed = 7,
    KernelEventType_DamageApplied = 8,
    KernelEventType_Explosion = 9,
    KernelEventType_MissionStateChanged = 10,
    KernelEventType_Error = 11,
    KernelEventType_ActorLanded = 12,
    KernelEventType_HealthChanged = 13,
} KernelEventType;

typedef enum KernelDespawnReason {
    KernelDespawnReason_Destroyed = 0,
    KernelDespawnReason_OutOfRange = 1,
    KernelDespawnReason_Disconnected = 2,
    KernelDespawnReason_Expired = 3,
    KernelDespawnReason_CapacityEvicted = 4,
} KernelDespawnReason;

typedef enum RenderEntityStatus {
    RenderEntityStatus_Active = 0,
    RenderEntityStatus_Predicted = 1,
    RenderEntityStatus_Stale = 2,
} RenderEntityStatus;

typedef enum KernelActorType {
    KernelActorType_Unknown = 0,
    KernelActorType_Player = 1,
    KernelActorType_Agent = 2,
} KernelActorType;

typedef enum KernelEntityType {
    KernelEntityType_Unknown = 0,
    KernelEntityType_Actor = 1,
    KernelEntityType_Prop = 2,
    KernelEntityType_Projectile = 3,
    KernelEntityType_Director = 5,
} KernelEntityType;

typedef uint64_t KernelItemInstanceId;
typedef uint64_t KernelInventoryContainerId;

typedef enum KernelItemMode {
    KernelItemMode_Fungible = 0,
    KernelItemMode_Stateful = 1,
} KernelItemMode;

typedef enum KernelItemCapabilityFlag {
    KernelItemCapability_Pickupable = 1u << 0,
    KernelItemCapability_Deployable = 1u << 1,
    KernelItemCapability_Carryable = 1u << 2,
    KernelItemCapability_Consumable = 1u << 3,
    KernelItemCapability_Throwable = 1u << 4,
    KernelItemCapability_Interactable = 1u << 5,
} KernelItemCapabilityFlag;

typedef enum KernelDomainAction {
    KernelDomainAction_None = 0,
    KernelDomainAction_Consume = 1,
    KernelDomainAction_Pickup = 2,
    KernelDomainAction_Throw = 3,
    KernelDomainAction_Place = 4,
    KernelDomainAction_Carry = 5,
    KernelDomainAction_Activate = 6,
} KernelDomainAction;

typedef enum KernelItemResidencyKind {
    KernelItemResidency_None = 0,
    KernelItemResidency_Inventory = 1,
    KernelItemResidency_World = 2,
    KernelItemResidency_Terminal = 3,
} KernelItemResidencyKind;

typedef enum KernelWorldItemMode {
    KernelWorldItemMode_Placed = 0,
    KernelWorldItemMode_Carrying = 1,
    KernelWorldItemMode_InFlight = 2,
} KernelWorldItemMode;

typedef enum KernelItemThrowMode {
    KernelItemThrowMode_None = 0,
    KernelItemThrowMode_IdentityPreserving = 1,
    KernelItemThrowMode_ConsumeAndSpawn = 2,
} KernelItemThrowMode;

typedef enum KernelPortableStateType {
    KernelPortableStateType_Uint32 = 0,
    KernelPortableStateType_Float = 1,
    KernelPortableStateType_Bool = 2,
} KernelPortableStateType;

typedef enum KernelPortableStateProjection {
    KernelPortableStateProjection_None = 0,
    KernelPortableStateProjection_HealthCurrent = 1,
} KernelPortableStateProjection;

typedef enum KernelGameplayRequestStatus {
    KernelGameplayRequestStatus_NoAction = 0,
    KernelGameplayRequestStatus_Rejected = 1,
    KernelGameplayRequestStatus_Committed = 2,
} KernelGameplayRequestStatus;

typedef enum KernelGameplayGraphOutcome {
    KernelGameplayGraphOutcome_NotSubmitted = 0,
    KernelGameplayGraphOutcome_Succeeded = 1,
    KernelGameplayGraphOutcome_FailedAfterCommit = 2,
} KernelGameplayGraphOutcome;

typedef enum KernelGameplayRequestRejectionReason {
    KernelGameplayRequestRejection_None = 0,
    KernelGameplayRequestRejection_InvalidRequest = 1,
    KernelGameplayRequestRejection_UnknownInstigator = 2,
    KernelGameplayRequestRejection_UnknownTarget = 3,
    KernelGameplayRequestRejection_UnknownItem = 4,
    KernelGameplayRequestRejection_StaleReference = 5,
    KernelGameplayRequestRejection_InvalidContext = 6,
    KernelGameplayRequestRejection_MissingCapability = 7,
    KernelGameplayRequestRejection_NotAuthorized = 8,
    KernelGameplayRequestRejection_OutOfRange = 9,
    KernelGameplayRequestRejection_LineOfSight = 10,
    KernelGameplayRequestRejection_InventoryFull = 11,
    KernelGameplayRequestRejection_InvalidQuantity = 12,
    KernelGameplayRequestRejection_InvalidPlacement = 13,
    KernelGameplayRequestRejection_Claimed = 14,
    KernelGameplayRequestRejection_Cooldown = 15,
    KernelGameplayRequestRejection_GraphRejected = 16,
} KernelGameplayRequestRejectionReason;

typedef enum KernelEntityTriggerActionType {
    KernelEntityTriggerActionType_None = 0,
    KernelEntityTriggerActionType_ApplyDamage = 1,
    KernelEntityTriggerActionType_SpawnEntity = 2,
    KernelEntityTriggerActionType_SpawnProjectile = 3,
    KernelEntityTriggerActionType_ApplyHealthChange = 4,
} KernelEntityTriggerActionType;

typedef enum KernelEntityRefSource {
    KernelEntityRefSource_Self = 0,
    KernelEntityRefSource_EventSubject = 1,
    KernelEntityRefSource_EventTarget = 2,
    KernelEntityRefSource_EventInstigator = 3,
} KernelEntityRefSource;

typedef enum KernelEventVec3Source {
    KernelEventVec3Source_Position = 0,
    KernelEventVec3Source_Direction = 1,
} KernelEventVec3Source;

typedef enum KernelActionConditionType {
    KernelActionConditionType_Always = 0,
    KernelActionConditionType_EventHasTarget = 1,
} KernelActionConditionType;

#define KERNEL_MAX_ACTION_GRAPH_ACTIONS 8

typedef struct KernelActionDefinition {
    uint8_t action_type;
    uint8_t target_source;
    uint16_t damage_amount;
    uint32_t spawn_entity_template_id;
    uint32_t spawn_projectile_template_id;
    uint8_t position_source;
    uint8_t direction_source;
    uint8_t owner_source;
    uint8_t reserved;
    uint32_t spawn_item_template_id;
    uint32_t spawn_item_quantity;
    int32_t health_change_amount;
    uint32_t condition_type;
} KernelActionDefinition;

typedef struct KernelActionTriggerDefinition {
    uint32_t struct_size;
    /* Legacy first-action mirror. action_count/actions are authoritative when
     * action_count is non-zero. */
    uint8_t action_type;
    uint8_t target_source;
    uint16_t damage_amount;
    uint32_t spawn_entity_template_id;
    uint32_t spawn_projectile_template_id;
    uint8_t position_source;
    uint8_t direction_source;
    uint8_t owner_source;
    uint8_t reserved;
    uint32_t spawn_item_template_id;
    uint32_t spawn_item_quantity;
    uint32_t action_count;
    KernelActionDefinition actions[KERNEL_MAX_ACTION_GRAPH_ACTIONS];
    int32_t health_change_amount;
    uint32_t condition_type;
} KernelActionTriggerDefinition;

#define KERNEL_MAX_PORTABLE_STATE_FIELDS 8

typedef struct KernelPortableStateFieldDefinition {
    uint32_t field_id;
    uint8_t type;
    uint8_t world_projection;
    uint16_t reserved0;
    uint32_t uint32_default;
    float float_default;
    uint32_t bool_default;
} KernelPortableStateFieldDefinition;

typedef struct KernelItemThrowDefinition {
    uint32_t struct_size;
    uint8_t mode;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t trajectory_projectile_template_id;
} KernelItemThrowDefinition;

typedef struct KernelItemUseDefinition {
    uint32_t struct_size;
    uint32_t quantity_cost;
    uint32_t charge_field_id;
    uint32_t cooldown_ticks;
    uint32_t destroy_when_empty;
} KernelItemUseDefinition;

typedef struct KernelItemTemplateDefinition {
    uint32_t struct_size;
    uint32_t item_template_id;
    uint8_t item_mode;
    uint8_t reserved0;
    uint16_t max_stack;
    uint32_t capability_flags;
    uint32_t entity_template_id;
    float interaction_range;
    uint32_t line_of_sight_required;
    uint32_t line_of_sight_blocking_mask;
    KernelItemThrowDefinition throw_policy;
    KernelItemUseDefinition use_policy;
    uint32_t portable_state_field_count;
    KernelPortableStateFieldDefinition
        portable_state_fields[KERNEL_MAX_PORTABLE_STATE_FIELDS];
    KernelActionTriggerDefinition item_used_trigger;
} KernelItemTemplateDefinition;

typedef struct KernelPropInteractionDefinition {
    uint32_t struct_size;
    uint32_t capability_flags;
    uint8_t line_of_sight_required;
    uint8_t reserved0;
    uint16_t reserved1;
    float interaction_range;
    uint32_t line_of_sight_blocking_mask;
} KernelPropInteractionDefinition;

typedef struct KernelPropPopulationRuleDefinition {
    uint32_t struct_size;
    uint32_t population_group_id;
    uint32_t max_alive;
} KernelPropPopulationRuleDefinition;

typedef struct KernelPropDefinition {
    uint32_t struct_size;
    KernelPropInteractionDefinition interaction;
    float carry_offset_x;
    float carry_offset_y;
    float carry_offset_z;
    uint32_t throw_trajectory_projectile_template_id;
    uint32_t lifetime_ticks;
    uint32_t population_group_id;
} KernelPropDefinition;

typedef enum KernelAiControllerType {
    KernelAiControllerType_None = 0,
    KernelAiControllerType_Sentry = 1,
    KernelAiControllerType_Director = 2,
} KernelAiControllerType;

typedef enum KernelEntityLifecycleEventType {
    KernelEntityLifecycleEventType_OutOfRange = 0,
    KernelEntityLifecycleEventType_Despawned = 1,
    KernelEntityLifecycleEventType_Destroyed = 2,
} KernelEntityLifecycleEventType;

typedef enum InputButton {
    InputButton_MoveJump = 1u << 0,
    InputButton_Fire = 1u << 1,
    InputButton_Sprint = 1u << 3,
    InputButton_Dodge = 1u << 6,
    InputButton_Parry = 1u << 7,
    InputButton_Aim = 1u << 8,
} InputButton;

typedef enum KernelActionBinding {
    KernelActionBinding_PrimaryFire = 0,
    KernelActionBinding_Reload = 1,
} KernelActionBinding;

typedef enum KernelActionTriggerMode {
    KernelActionTriggerMode_Press = 0,
    KernelActionTriggerMode_Hold = 1,
} KernelActionTriggerMode;

typedef enum KernelActionPhase {
    KernelActionPhase_None = 0,
    KernelActionPhase_Windup = 1,
    KernelActionPhase_Active = 2,
    KernelActionPhase_Recovery = 3,
} KernelActionPhase;

typedef enum KernelLocalActionResultType {
    KernelLocalActionResultType_Accepted = 0,
    KernelLocalActionResultType_Corrected = 1,
    KernelLocalActionResultType_Rejected = 2,
} KernelLocalActionResultType;

typedef enum KernelLocalActionResultReason {
    KernelLocalActionResultReason_None = 0,
    KernelLocalActionResultReason_InvalidActionId = 1,
    KernelLocalActionResultReason_MissingActor = 2,
    KernelLocalActionResultReason_MissingTemplate = 3,
    KernelLocalActionResultReason_Busy = 4,
    KernelLocalActionResultReason_Reloading = 5,
    KernelLocalActionResultReason_NoAmmo = 6,
    KernelLocalActionResultReason_Cancelled = 7,
    KernelLocalActionResultReason_TimedOut = 8,
    KernelLocalActionResultReason_Dead = 9,
    KernelLocalActionResultReason_WeaponChanged = 10,
    KernelLocalActionResultReason_EffectFailed = 11,
    KernelLocalActionResultReason_Cooldown = 12,
} KernelLocalActionResultReason;

typedef enum KernelRemoteActionPresentationEventType {
    KernelRemoteActionPresentationEventType_FireCommit = 0,
    KernelRemoteActionPresentationEventType_CastingCommit = 1,
    KernelRemoteActionPresentationEventType_ReloadCommit = 2,
    KernelRemoteActionPresentationEventType_HitReaction = 3,
    KernelRemoteActionPresentationEventType_DeathTrigger = 4,
} KernelRemoteActionPresentationEventType;

typedef enum KernelActionTemplateFlag {
    KernelActionTemplateFlag_CancelOnRelease = 1u << 0,
    KernelActionTemplateFlag_CancelOnDeath = 1u << 1,
    KernelActionTemplateFlag_CancelOnWeaponChange = 1u << 2,
    KernelActionTemplateFlag_CancelBeforeFirstCommit = 1u << 3,
} KernelActionTemplateFlag;

typedef enum KernelWeaponFireMode {
    KernelWeaponFireMode_Hitscan = 0,
    KernelWeaponFireMode_Shotgun = 1,
    KernelWeaponFireMode_Projectile = 2,
} KernelWeaponFireMode;

typedef enum KernelProjectileMotionModel {
    KernelProjectileMotionModel_Linear = 0,
    KernelProjectileMotionModel_Parabolic = 1,
    KernelProjectileMotionModel_Homing = 2,
} KernelProjectileMotionModel;

typedef enum KernelProjectileSyncMode {
    KernelProjectileSyncMode_LocalPredictedDeterministic = 0,
    KernelProjectileSyncMode_HybridDeterministicThenSnapshot = 1,
    KernelProjectileSyncMode_ServerSnapshotOnly = 2,
} KernelProjectileSyncMode;

typedef enum KernelMissileGuidancePhase {
    KernelMissileGuidancePhase_Boost = 0,
    KernelMissileGuidancePhase_Guided = 1,
    KernelMissileGuidancePhase_LostTarget = 2,
    KernelMissileGuidancePhase_Expired = 3,
} KernelMissileGuidancePhase;

typedef enum KernelHomingMode {
    KernelHomingMode_FireAndForget = 0,
} KernelHomingMode;

typedef enum KernelProjectileHitResponse {
    KernelProjectileHitResponse_Destroy = 0,
    KernelProjectileHitResponse_Continue = 1,
    KernelProjectileHitResponse_Bounce = 2,
    KernelProjectileHitResponse_Attach = 3,
} KernelProjectileHitResponse;

typedef enum KernelProjectileDamageShape {
    KernelProjectileDamageShape_DirectHit = 0,
    KernelProjectileDamageShape_None = 1,
    KernelProjectileDamageShape_PiercingSegment = 2,
} KernelProjectileDamageShape;

typedef enum KernelProjectileType {
    KernelProjectileType_Standard = 0,
    KernelProjectileType_AreaEffect = 1,
    KernelProjectileType_Beam = 2,
} KernelProjectileType;

typedef enum KernelProjectileDamageFalloff {
    KernelProjectileDamageFalloff_None = 0,
    KernelProjectileDamageFalloff_Linear = 1,
} KernelProjectileDamageFalloff;

typedef enum KernelProjectileCollisionQueryMode {
    /* Select overlap, sweep, or ray from collider geometry and projectile motion. */
    KernelProjectileCollisionQueryMode_Auto = 0,
    /* Test the projectile volume only at its current transform. */
    KernelProjectileCollisionQueryMode_Overlap = 1,
    /* Test the projectile volume swept from its previous to current transform. */
    KernelProjectileCollisionQueryMode_Sweep = 2,
    /* Test a thin segment/ray and ignore projectile volume extents. */
    KernelProjectileCollisionQueryMode_Ray = 3,
} KernelProjectileCollisionQueryMode;

typedef enum KernelAgentCamp {
    KernelAgentCamp_Unknown = 0,
    KernelAgentCamp_PlayerSide = 1,
    KernelAgentCamp_EnemySide = 2,
    KernelAgentCamp_Neutral = 3,
} KernelAgentCamp;

typedef enum KernelAgentRelation {
    KernelAgentRelation_Self = 0,
    KernelAgentRelation_Ally = 1,
    KernelAgentRelation_Hostile = 2,
    KernelAgentRelation_Neutral = 3,
    KernelAgentRelation_Unknown = 4,
} KernelAgentRelation;

typedef struct KernelVec2 {
    float x;
    float y;
} KernelVec2;

KERNEL_RPC_STRUCT(R"json({"type":"KernelVec3"})json")
typedef struct KernelVec3 {
    float x;
    float y;
    float z;
} KernelVec3;

typedef struct KernelVec4 {
    float x;
    float y;
    float z;
    float w;
} KernelVec4;

KERNEL_RPC_STRUCT(R"json({"type":"KernelQuat"})json")
typedef struct KernelQuat {
    float x;
    float y;
    float z;
    float w;
} KernelQuat;

typedef struct TickConfig {
    uint32_t server_tick_rate;
    uint32_t snapshot_rate;
    uint32_t history_ms;
    uint32_t max_ticks_per_update;
} TickConfig;

typedef enum KernelNetworkStatsMode {
    KernelNetworkStatsMode_Default = 0,
    KernelNetworkStatsMode_Off = 1,
    KernelNetworkStatsMode_Basic = 2,
    KernelNetworkStatsMode_Detailed = 3,
} KernelNetworkStatsMode;

typedef struct KernelNetworkStatsConfig {
    uint8_t mode;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t action_packet_budget_bytes;
    uint32_t remote_presentation_expiry_ms;
    uint32_t remote_presentation_client_budget_bytes_per_second;
    uint32_t remote_presentation_server_budget_bytes_per_second;
} KernelNetworkStatsConfig;

typedef struct KernelConfig {
    KernelMode mode;
    TickConfig tick;
    uint32_t max_render_states;
    uint32_t max_events;
    KernelNetworkStatsConfig network_stats;
} KernelConfig;

typedef struct KernelPhysicsConfig {
    uint32_t struct_size;
    uint32_t physics_simulation;
    uint32_t physics_workers;
} KernelPhysicsConfig;

typedef enum KernelActorBlockingMode {
    KernelActorBlockingMode_Disabled = 0,
    KernelActorBlockingMode_Predicted = 1,
} KernelActorBlockingMode;

typedef struct KernelSessionRulesConfig {
    uint32_t struct_size;
    uint32_t actor_blocking_mode;
} KernelSessionRulesConfig;

typedef struct KernelStaticCollisionSceneConfig {
    uint32_t struct_size;
    const uint8_t* artifact_bytes;
    uint32_t artifact_size;
    uint32_t scene_id;
    uint32_t collider_id;
    uint32_t collision_layer;
} KernelStaticCollisionSceneConfig;

typedef struct KernelGameplayCatalogLoadOptions {
    uint32_t struct_size;
    const KernelStaticCollisionSceneConfig* static_collision_scene;
    uint32_t* out_static_scene_rejected;
} KernelGameplayCatalogLoadOptions;

typedef struct KernelActionIntent {
    uint32_t action_instance_id;
    uint16_t binding_id;
    uint8_t flags;
    uint8_t reserved;
} KernelActionIntent;

typedef struct KernelActionInput {
    uint32_t action_instance_id;
    uint8_t held;
    uint8_t flags;
    uint16_t reserved;
} KernelActionInput;

typedef struct KernelPlayerInput {
    uint32_t input_seq;
    uint64_t client_action_time_us;
    KernelVec2 move;
    KernelVec2 look_delta;
    KernelVec3 aim_dir;
    uint32_t buttons;
    uint8_t selected_weapon;
    KernelActionIntent action_intent;
    KernelActionInput action_input;
} KernelPlayerInput;

typedef struct KernelLocalActionResult {
    uint32_t action_instance_id;
    uint16_t confirmed_commit_count;
    uint8_t result;
    uint8_t reason;
    uint32_t authoritative_tick;
} KernelLocalActionResult;

typedef struct KernelRemoteActionPresentationEvent {
    uint32_t actor_net_id;
    uint32_t action_template_id;
    uint32_t action_instance_id;
    uint16_t first_commit_index;
    uint16_t commit_count;
    uint8_t event_type;
    uint8_t flags;
    uint16_t server_tick_delta;
} KernelRemoteActionPresentationEvent;

typedef struct KernelActionRuntimeView {
    uint32_t struct_size;
    uint32_t action_template_id;
    uint32_t action_instance_id;
    uint8_t phase;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t start_tick;
    uint32_t commit_count;
} KernelActionRuntimeView;

typedef struct RenderEntityState {
    uint64_t entity_id;
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
    uint32_t spawn_tick;
    uint32_t action_instance_id;
    uint32_t status;
    uint32_t projectile_template_id;
    uint32_t collider_template_id;
    uint32_t actor_template_id;
    KernelActionRuntimeView action;
    KernelVec3 aim_direction;
    uint32_t item_template_id;
    KernelItemInstanceId item_instance_id;
    uint8_t world_item_mode;
    uint8_t reserved_item0;
    uint16_t reserved_item1;
    uint32_t carrier_entity_id;
} RenderEntityState;

KERNEL_RPC_STRUCT(R"json({"type":"KernelServerEntityCreateInfo"})json")
typedef struct KernelServerEntityCreateInfo {
    uint32_t struct_size;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    uint16_t animation_state;
    uint32_t visual_flags;
    uint32_t actor_template_id;
    uint32_t entity_template_id;
} KernelServerEntityCreateInfo;

KERNEL_RPC_STRUCT(R"json({"type":"KernelServerEntityActivateInfo"})json")
typedef struct KernelServerEntityActivateInfo {
    uint32_t struct_size;
    uint32_t subject_net_id;
    uint32_t instigator_net_id;
    uint32_t target_net_id;
    uint32_t action_instance_id;
    uint64_t request_id;
} KernelServerEntityActivateInfo;

typedef struct KernelGameplayRequest {
    uint32_t struct_size;
    uint32_t requester_peer;
    uint64_t request_id;
    uint32_t instigator_net_id;
    uint8_t domain_action;
    uint8_t reserved0;
    uint16_t reserved1;
    KernelItemInstanceId selected_item_instance_id;
    uint32_t target_net_id;
    uint32_t requested_quantity;
    KernelVec3 placement_position;
    KernelVec3 throw_direction;
} KernelGameplayRequest;

typedef struct KernelGameplayRequestOutcome {
    uint32_t struct_size;
    uint32_t requester_peer;
    uint64_t request_id;
    uint8_t status;
    uint8_t graph_outcome;
    uint8_t domain_action;
    uint8_t rejection_reason;
    KernelItemInstanceId item_instance_id;
    uint32_t prop_entity_id;
    uint32_t committed_quantity;
} KernelGameplayRequestOutcome;

typedef struct KernelItemInstanceView {
    uint32_t struct_size;
    KernelItemInstanceId item_instance_id;
    uint32_t item_template_id;
    uint32_t quantity;
    uint8_t residency;
    uint8_t world_mode;
    uint16_t slot;
    KernelInventoryContainerId inventory_container_id;
    uint32_t prop_entity_id;
    uint32_t carrier_entity_id;
    uint32_t terminal;
    uint32_t next_use_tick;
    uint32_t portable_state_field_count;
    KernelPortableStateFieldDefinition
        portable_state_fields[KERNEL_MAX_PORTABLE_STATE_FIELDS];
} KernelItemInstanceView;

typedef struct KernelInventoryContainerView {
    uint32_t struct_size;
    KernelInventoryContainerId inventory_container_id;
    uint32_t owner_entity_id;
    uint32_t slot_capacity;
    uint32_t occupied_slot_count;
    uint64_t revision;
    uint8_t sync_state;
    uint8_t reserved0;
    uint16_t reserved1;
} KernelInventoryContainerView;

typedef enum KernelInventorySyncState {
    KernelInventorySyncState_NotAvailable = 0,
    KernelInventorySyncState_Syncing = 1,
    KernelInventorySyncState_Ready = 2,
    KernelInventorySyncState_Desynced = 3,
} KernelInventorySyncState;

typedef enum KernelInventoryDeltaType {
    KernelInventoryDeltaType_Add = 0,
    KernelInventoryDeltaType_Update = 1,
    KernelInventoryDeltaType_Remove = 2,
    KernelInventoryDeltaType_Move = 3,
} KernelInventoryDeltaType;

typedef enum KernelInventoryChangeFlag {
    KernelInventoryChange_Quantity = 1u << 0,
    KernelInventoryChange_Cooldown = 1u << 1,
    KernelInventoryChange_PortableState = 1u << 2,
    KernelInventoryChange_All =
        KernelInventoryChange_Quantity |
        KernelInventoryChange_Cooldown |
        KernelInventoryChange_PortableState,
} KernelInventoryChangeFlag;

typedef struct KernelInventoryDelta {
    uint32_t struct_size;
    KernelInventoryContainerId inventory_container_id;
    uint64_t revision;
    uint8_t type;
    uint8_t reserved0;
    uint16_t slot;
    uint16_t previous_slot;
    uint16_t changed_fields;
    KernelItemInstanceView item;
} KernelInventoryDelta;

typedef struct KernelServerEntityState {
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
    uint8_t active_weapon_slot;
    uint8_t weapon_slot_count;
    uint16_t reserved0;
    uint32_t weapon_ids[KERNEL_MAX_WEAPON_SLOTS];
    uint16_t ammo[KERNEL_MAX_WEAPON_SLOTS];
    // reserve_magazines counts spare full magazines, not spare bullets.
    // UINT16_MAX is allowed as an authored practical maximum for match lengths
    // that should not exhaust reserves in normal play. It is not a sentinel:
    // reload logic decrements it like any other count and must not special-case 65535.
    uint16_t reserve_magazines[KERNEL_MAX_WEAPON_SLOTS];
    uint32_t is_reloading;
    uint32_t reload_remaining_ticks;
    KernelActionRuntimeView action;
    KernelVec3 aim_direction;
    uint32_t item_template_id;
    KernelItemInstanceId item_instance_id;
    uint8_t world_item_mode;
    uint8_t reserved_item0;
    uint16_t reserved_item1;
    uint32_t carrier_entity_id;
} KernelServerEntityState;

typedef enum KernelColliderShapeType {
    KernelColliderShapeType_Aabb = 0,
    KernelColliderShapeType_Sphere = 1,
    KernelColliderShapeType_OrientedBox = 2,
    KernelColliderShapeType_Segment = 3,
    KernelColliderShapeType_Cone = 4,
    KernelColliderShapeType_Capsule = 5,
} KernelColliderShapeType;

typedef enum KernelColliderPurpose {
    KernelColliderPurpose_Hit = 1u << 0,
    KernelColliderPurpose_Damage = 1u << 1,
    KernelColliderPurpose_Trigger = 1u << 2,
    KernelColliderPurpose_Vision = 1u << 3,
    KernelColliderPurpose_Movement = 1u << 4,
} KernelColliderPurpose;

typedef enum KernelDebugRecordType {
    KernelDebugRecordType_Hit = 1u << 0,
    KernelDebugRecordType_Projectile = 1u << 1,
} KernelDebugRecordType;

#define KERNEL_DEBUG_WILDCARD_U8 UINT8_C(0xff)

typedef struct KernelColliderTemplateDefinition {
    uint32_t struct_size;
    uint32_t template_id;
    uint8_t shape_type;
    uint8_t reserved0;
    uint16_t reserved1;
    KernelVec3 center;
    KernelVec4 shape_params;
    uint32_t purpose_flags;
    uint32_t layer_mask;
    uint32_t lifetime_ticks;
} KernelColliderTemplateDefinition;

typedef struct KernelColliderBindingDefinition {
    uint32_t struct_size;
    uint16_t entity_type;
    uint16_t reserved0;
    uint32_t collider_template_id;
    KernelVec3 local_position;
    KernelQuat local_rotation;
} KernelColliderBindingDefinition;

typedef struct KernelAgentVisionConfig {
    uint32_t struct_size;
    uint8_t camp;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t vision_collider_template_id;
    uint32_t max_visible_hostiles;
    uint32_t max_visible_allies;
    uint32_t max_visible_neutrals;
    KernelVec3 local_origin;
    KernelVec3 local_forward;
} KernelAgentVisionConfig;

typedef struct KernelHomingMechanicsDefinition {
    uint32_t struct_size;
    uint8_t homing_mode;
    uint8_t sync_mode;
    uint16_t reserved0;
    uint32_t boost_ticks;
    float lock_on_range;
    float lose_target_range;
    float lock_cone_degrees;
    float max_turn_degrees_per_tick;
    float acceleration;
    float max_speed;
} KernelHomingMechanicsDefinition;

typedef struct KernelAreaEffectMechanicsDefinition {
    uint32_t struct_size;
    float radius;
    uint16_t damage_per_interval;
    uint16_t reserved0;
    uint32_t damage_interval_ticks;
    uint32_t lifetime_ticks;
    float spawn_distance;
    uint32_t collision_mask;
} KernelAreaEffectMechanicsDefinition;

typedef struct KernelBeamMechanicsDefinition {
    uint32_t struct_size;
    float length;
    float radius;
    uint16_t damage_per_tick;
    uint16_t reserved0;
    uint32_t lifetime_ticks;
    uint32_t collision_mask;
} KernelBeamMechanicsDefinition;

typedef struct KernelProjectileMechanicsDefinition {
    uint32_t struct_size;
    uint8_t projectile_type;
    uint8_t motion_model;
    uint8_t hit_response;
    uint8_t damage_shape;
    uint8_t sync_mode;
    uint8_t damage_falloff;
    uint16_t damage;
    float speed;
    uint32_t lifetime_ticks;
    KernelVec3 gravity;
    uint32_t collider_template_id;
    uint32_t collision_mask;
    uint32_t max_hit_count;
    KernelHomingMechanicsDefinition homing;
    KernelAreaEffectMechanicsDefinition area_effect;
    KernelBeamMechanicsDefinition beam;
    KernelActionTriggerDefinition projectile_impact_trigger;
    KernelActionTriggerDefinition expired_trigger;
    uint8_t collision_query_mode;
    uint8_t reserved0;
    uint16_t reserved1;
} KernelProjectileMechanicsDefinition;

typedef struct KernelProjectileTemplateDefinition {
    uint32_t struct_size;
    uint32_t projectile_template_id;
    uint8_t weapon_id;
    uint8_t reserved0;
    uint16_t reserved1;
    KernelProjectileMechanicsDefinition mechanics;
} KernelProjectileTemplateDefinition;

typedef struct KernelActorTemplateDefinition {
    uint32_t struct_size;
    uint32_t actor_template_id;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t collider_template_id;
    KernelAgentVisionConfig vision;
} KernelActorTemplateDefinition;

typedef struct KernelActionTemplateDefinition {
    /*
     * Action templates contain gameplay policy only.
     * If server-selectable presentation is needed in the future, prefer an
     * abstract presentation_profile_id that clients map to local assets.
     * The native kernel must not bind action templates to Animator state IDs,
     * AnimationClip IDs, Unity-specific fields, or Unity Engine dependencies.
     */
    uint32_t struct_size;
    uint32_t action_template_id;
    uint8_t trigger_mode;
    uint8_t flags;
    uint16_t ammo_cost_per_commit;
    uint32_t commit_offset_ticks;
    uint32_t commit_interval_ticks;
    uint32_t max_commit_count;
    uint32_t recovery_ticks;
    uint32_t hold_input_timeout_ticks;
} KernelActionTemplateDefinition;

typedef struct KernelEntityAiDefinition KernelEntityAiDefinition;
typedef struct KernelEntityTemplateDefinition KernelEntityTemplateDefinition;

typedef struct KernelGameplayCatalogDefinition {
    uint32_t struct_size;
    uint32_t catalog_version;
    uint64_t catalog_hash;
    const KernelActorTemplateDefinition* actor_templates;
    uint32_t actor_template_count;
    const KernelProjectileTemplateDefinition* projectile_templates;
    uint32_t projectile_template_count;
    const KernelColliderTemplateDefinition* collider_templates;
    uint32_t collider_template_count;
    const KernelColliderBindingDefinition* collider_bindings;
    uint32_t collider_binding_count;
    const KernelEntityTemplateDefinition* entity_templates;
    uint32_t entity_template_count;
    const KernelActionTemplateDefinition* action_templates;
    uint32_t action_template_count;
    const KernelItemTemplateDefinition* item_templates;
    uint32_t item_template_count;
    const KernelPropPopulationRuleDefinition* prop_population_rules;
    uint32_t prop_population_rule_count;
} KernelGameplayCatalogDefinition;

typedef struct KernelGameplayCatalogLoadResult {
    uint32_t struct_size;
    uint32_t status;
    uint32_t catalog_version;
    uint64_t catalog_hash;
    uint32_t projectile_template_count;
    uint32_t collider_template_count;
    uint32_t collider_binding_count;
    uint32_t error_code;
    uint32_t source_kind;
    uint32_t template_kind;
    uint32_t template_id;
    uint32_t field_id;
    int32_t line;
    int32_t column;
    char path[128];
    char field[64];
    char diagnostic[256];
} KernelGameplayCatalogLoadResult;

typedef enum KernelGameplayCatalogSyncState {
    KernelGameplayCatalogSyncState_Idle = 0,
    KernelGameplayCatalogSyncState_Connecting = 1,
    KernelGameplayCatalogSyncState_FetchingManifest = 2,
    KernelGameplayCatalogSyncState_ManifestReady = 3,
    KernelGameplayCatalogSyncState_Downloading = 4,
    KernelGameplayCatalogSyncState_BundleReady = 5,
    KernelGameplayCatalogSyncState_Handshaking = 6,
    KernelGameplayCatalogSyncState_Ready = 7,
    KernelGameplayCatalogSyncState_Failed = 8,
    KernelGameplayCatalogSyncState_Disconnected = 9,
} KernelGameplayCatalogSyncState;

typedef enum KernelGameplayCatalogSyncError {
    KernelGameplayCatalogSyncError_None = 0,
    KernelGameplayCatalogSyncError_Unsupported = 1,
    KernelGameplayCatalogSyncError_BundleUnavailable = 2,
    KernelGameplayCatalogSyncError_VersionMismatch = 3,
    KernelGameplayCatalogSyncError_InvalidManifest = 4,
    KernelGameplayCatalogSyncError_BundleTooLarge = 5,
    KernelGameplayCatalogSyncError_InvalidBundle = 6,
    KernelGameplayCatalogSyncError_Timeout = 7,
    KernelGameplayCatalogSyncError_Disconnected = 8,
    KernelGameplayCatalogSyncError_InvalidState = 9,
    KernelGameplayCatalogSyncError_Transport = 10,
} KernelGameplayCatalogSyncError;

typedef struct KernelGameplayCatalogManifest {
    uint32_t struct_size;
    uint32_t catalog_version;
    uint64_t catalog_hash;
    uint32_t bundle_size;
    uint8_t bundle_sha256[KERNEL_GAMEPLAY_CATALOG_SHA256_SIZE];
    char entry_path[KERNEL_GAMEPLAY_CATALOG_ENTRY_PATH_SIZE];
    char content_namespace[KERNEL_GAMEPLAY_CATALOG_CONTENT_NAMESPACE_SIZE];
} KernelGameplayCatalogManifest;

typedef struct KernelGameplayCatalogSyncServerConfig {
    uint32_t struct_size;
    const uint8_t* bundle_bytes;
    uint32_t bundle_size;
    const char* entry_path;
    const char* content_namespace;
} KernelGameplayCatalogSyncServerConfig;

typedef struct KernelGameplayCatalogSyncClientConfig {
    uint32_t struct_size;
    uint32_t max_bundle_size;
    uint32_t timeout_ms;
} KernelGameplayCatalogSyncClientConfig;

typedef struct KernelGameplayCatalogSyncStatus {
    uint32_t struct_size;
    KernelGameplayCatalogSyncState state;
    KernelGameplayCatalogSyncError error;
    uint32_t received_bundle_size;
    KernelGameplayCatalogManifest manifest;
} KernelGameplayCatalogSyncStatus;

typedef struct KernelBenchmarkStats {
    uint32_t struct_size;
    uint32_t catalog_version;
    uint64_t catalog_hash;
    uint32_t total_entity_count;
    uint32_t projectile_count;
    uint32_t event_spawn_projectile_count;
    uint32_t snapshot_only_projectile_count;
    uint32_t hybrid_projectile_count;
    float event_spawn_ratio;
    float snapshot_only_ratio;
    float hybrid_ratio;
    uint64_t render_solver_cost_us;
    uint64_t projectile_solver_cost_us;
    uint64_t hybrid_correction_cost_us;
    uint64_t grounded_query_count;
    uint64_t grounded_query_cost_us;
    uint64_t kinematic_move_count;
    uint64_t kinematic_move_cost_us;
    uint64_t character_move_count;
    uint64_t character_move_cost_us;
} KernelBenchmarkStats;

typedef struct KernelNetworkStats {
    uint32_t struct_size;
    uint64_t snapshot_bytes_sent;
    uint64_t event_bytes_sent;
    uint64_t reliable_bytes_sent;
    uint64_t unreliable_bytes_sent;
    uint32_t packet_count_sent;
    uint32_t average_packet_size;
    uint32_t max_packet_size;
    uint64_t packet_serialization_cost_us;
    uint64_t packet_deserialization_cost_us;
    uint64_t rtt_us;
    uint64_t jitter_us;
    float loss_ratio;
    uint32_t replication_metadata_timeout_count;
    uint32_t replication_stale_snapshot_drop_count;
    uint32_t collection_mode;
    uint32_t reserved0;
    uint64_t input_bytes_sent;
    uint64_t presentation_bytes_sent;
    uint64_t session_bytes_sent;
    uint64_t local_action_result_bytes_sent;
    uint64_t remote_action_presentation_bytes_sent;
    uint64_t local_action_results_generated;
    uint64_t local_action_results_sent;
    uint64_t local_action_results_accepted;
    uint64_t local_action_results_corrected;
    uint64_t local_action_results_rejected;
    uint64_t local_action_result_server_duplicates_suppressed;
    uint64_t local_action_result_client_duplicates_dropped;
    uint64_t local_action_results_timed_out;
    uint64_t local_action_result_latency_sample_count;
    uint64_t local_action_result_latency_us_total;
    uint64_t local_action_result_latency_us_max;
    uint64_t local_action_result_batch_count;
    uint64_t local_action_result_batch_record_count;
    uint32_t average_local_action_result_batch_size;
    uint32_t max_local_action_result_batch_size;
    uint64_t remote_presentation_records_generated;
    uint64_t remote_presentation_records_sent;
    uint64_t remote_presentation_batch_count;
    uint64_t remote_presentation_batch_record_count;
    uint64_t remote_presentation_relevance_filtered;
    uint64_t remote_presentation_budget_dropped;
    uint64_t remote_presentation_stale_dropped;
    uint64_t remote_presentation_duplicate_dropped;
    uint32_t average_remote_presentation_batch_size;
    uint32_t max_remote_presentation_batch_size;
    uint64_t zero_action_instance_attempts;
    uint64_t action_instance_collisions;
    uint64_t inventory_delta_bytes_sent;
    uint64_t inventory_snapshot_bytes_sent;
    uint64_t prop_state_bytes_sent;
    uint64_t inventory_resync_request_count;
    uint64_t inventory_revision_gap_count;
} KernelNetworkStats;

typedef struct KernelHitDebugInfo {
    uint32_t source_net_id;
    uint32_t target_net_id;
    uint8_t weapon_id;
    uint8_t reserved0;
    uint16_t reserved1;
    KernelVec3 position;
} KernelHitDebugInfo;

typedef struct KernelProjectileDebugInfo {
    uint32_t projectile_net_id;
    uint32_t owner_net_id;
    uint32_t owner_peer;
    uint8_t weapon_id;
    uint8_t motion_model;
    uint8_t sync_mode;
    uint8_t reserved0;
    KernelVec3 position;
    KernelVec3 velocity;
} KernelProjectileDebugInfo;

typedef struct KernelDebugInfo {
    uint32_t struct_size;
    uint32_t tick;
    uint8_t record_type;
    uint8_t flags;
    uint16_t reserved0;
    union {
        KernelHitDebugInfo hit;
        KernelProjectileDebugInfo projectile;
    } data;
} KernelDebugInfo;

typedef struct KernelDebugRecordFilter {
    uint32_t struct_size;
    uint32_t record_type_mask;
    uint32_t source_net_id;
    uint32_t target_net_id;
    uint32_t projectile_net_id;
    uint8_t weapon_id;
    uint8_t motion_model;
    uint8_t sync_mode;
    uint8_t reserved0;
    uint32_t min_tick;
    uint32_t max_tick;
} KernelDebugRecordFilter;

typedef struct KernelColliderShapeQuery {
    uint32_t struct_size;
    uint16_t entity_type_filter;
    uint16_t actor_type_filter;
    uint32_t entity_net_id;
    uint32_t purpose_mask;
} KernelColliderShapeQuery;

typedef struct KernelColliderShapeView {
    uint32_t struct_size;
    uint32_t entity_net_id;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t collider_template_id;
    uint8_t shape_type;
    uint8_t reserved1;
    uint16_t reserved2;
    KernelVec3 world_center;
    KernelVec4 shape_params;
    uint32_t purpose_flags;
    uint32_t layer_mask;
    uint32_t collider_id;
    uint32_t owner_net_id;
    KernelQuat world_rotation;
    KernelVec3 segment_start;
    KernelVec3 segment_end;
    uint32_t lifetime_ticks;
    uint32_t remaining_ticks;
    uint32_t has_resolved_damage;
} KernelColliderShapeView;

typedef struct KernelVisionStateQuery {
    uint32_t struct_size;
    uint16_t entity_type_filter;
    uint16_t actor_type_filter;
    uint32_t agent_net_id;
} KernelVisionStateQuery;

typedef struct KernelVisionStateView {
    uint32_t struct_size;
    uint32_t agent_net_id;
    uint16_t entity_type;
    uint8_t camp;
    uint8_t actor_type;
    KernelVec3 vision_origin;
    KernelVec3 vision_forward;
    uint32_t vision_collider_template_id;
    uint32_t resolved_collider_template_id;
    uint32_t visible_hostiles[KERNEL_MAX_VISIBLE_HOSTILES];
    uint32_t visible_hostile_count;
    uint32_t visible_allies[KERNEL_MAX_VISIBLE_ALLIES];
    uint32_t visible_ally_count;
    uint32_t visible_neutrals[KERNEL_MAX_VISIBLE_NEUTRALS];
    uint32_t visible_neutral_count;
    uint32_t current_target_candidate;
    uint8_t relation_to_current_target;
    uint8_t reserved1;
    uint16_t reserved2;
    uint32_t last_seen_target;
    KernelVec3 last_known_target_position;
    float time_since_last_seen_target;
    uint32_t valid;
} KernelVisionStateView;

typedef struct KernelWeaponMechanicsDefinition {
    uint32_t struct_size;
    uint8_t weapon_id;
    uint8_t fire_mode;
    uint16_t magazine_size;
    uint16_t reserve_magazines;
    uint16_t damage;
    float max_range;
    uint8_t pellet_count;
    float pellet_spread;
    uint32_t projectile_template_id;
    uint32_t segment_collider_template_id;
    uint32_t fire_action_template_id;
    uint32_t reload_action_template_id;
} KernelWeaponMechanicsDefinition;

typedef struct KernelHomingState {
    uint32_t struct_size;
    uint32_t net_id;
    uint32_t owner_peer;
    uint32_t shooter_net_id;
    uint32_t target_net_id;
    uint8_t homing_mode;
    uint8_t sync_mode;
    uint8_t guidance_phase;
    uint8_t reserved0;
    uint32_t boost_ticks;
    uint32_t guidance_start_tick;
    float lock_on_range;
    float lose_target_range;
    float lock_cone_degrees;
    float max_turn_degrees_per_tick;
    float acceleration;
    float max_speed;
    uint32_t valid;
} KernelHomingState;

typedef struct KernelCombatStateDefinition {
    uint32_t struct_size;
    uint16_t hp;
    uint16_t max_hp;
    uint8_t active_weapon_slot;
    uint8_t weapon_slot_count;
    uint16_t reserved0;
    uint32_t collider_template_id;
    float move_speed_meters_per_second;
    KernelVec3 hitbox_center;
    KernelVec3 hitbox_half_extents;
    uint32_t weapon_ids[KERNEL_MAX_WEAPON_SLOTS];
    uint16_t ammo[KERNEL_MAX_WEAPON_SLOTS];
    // reserve_magazines counts spare full magazines, not spare bullets.
    // UINT16_MAX is allowed as an authored practical maximum for match lengths
    // that should not exhaust reserves in normal play. It is not a sentinel:
    // reload logic decrements it like any other count and must not special-case 65535.
    uint16_t reserve_magazines[KERNEL_MAX_WEAPON_SLOTS];
} KernelCombatStateDefinition;

typedef enum KernelMovementControllerType {
    KernelMovementControllerType_None = 0,
    KernelMovementControllerType_Grounded = 1,
    KernelMovementControllerType_Kinematic = 2,
    KernelMovementControllerType_Character = 3,
} KernelMovementControllerType;

typedef struct KernelMovementDefinition {
    uint32_t struct_size;
    uint8_t controller_type;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t movement_collider_template_id;
    KernelVec3 gravity;
    float max_slope_degrees;
    float step_height;
    float ground_probe_distance;
    float ground_snap_distance;
} KernelMovementDefinition;

struct KernelEntityAiDefinition {
    uint32_t struct_size;
    uint32_t controller_type;
    uint32_t ai_profile_id;
    uint32_t tick_interval;
    uint32_t blackboard_id;
    uint32_t spawn_target_count;
    uint32_t spawn_entity_template_id;
    uint32_t spawn_actor_template_id;
    KernelVec3 spawn_position;
    float spawn_radius;
    uint32_t spawn_seed;
};

struct KernelEntityTemplateDefinition {
    uint32_t struct_size;
    uint32_t entity_template_id;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t actor_template_id;
    uint32_t component_flags;
    uint32_t collider_template_id;
    uint16_t animation_state;
    uint16_t reserved0;
    uint32_t visual_flags;
    KernelCombatStateDefinition combat;
    KernelAgentVisionConfig vision;
    KernelEntityAiDefinition ai;
    KernelMovementDefinition movement;
    KernelActionTriggerDefinition activated_trigger;
    KernelActionTriggerDefinition collision_trigger;
    KernelActionTriggerDefinition health_depleted_trigger;
    KernelActionTriggerDefinition destroy_entity_trigger;
    KernelPropDefinition prop;
    uint32_t collision_trigger_mask;
};

typedef struct KernelEvent {
    KernelEventType type;
    uint32_t tick;
    uint32_t net_id;
    uint32_t peer_id;
    uint32_t code;
    uint64_t event_time_us;
    uint64_t presentation_time_us;
    int32_t health_delta;
} KernelEvent;

typedef struct KernelEntityLifecycleEvent {
    KernelEntityLifecycleEventType type;
    uint32_t tick;
    uint32_t net_id;
    uint32_t reason;
    uint16_t entity_type;
    uint16_t actor_type;
    uint32_t owner_peer;
} KernelEntityLifecycleEvent;

#ifdef __cplusplus
}

static_assert(sizeof(KernelActionIntent) == 8, "KernelActionIntent ABI must stay 8 bytes");
static_assert(sizeof(KernelActionInput) == 8, "KernelActionInput ABI must stay 8 bytes");
#endif

#endif  // KERNEL_PUBLIC_KERNEL_TYPES_H_
