#ifndef KERNEL_PUBLIC_KERNEL_TYPES_H_
#define KERNEL_PUBLIC_KERNEL_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_ABI_VERSION 15u

#define KERNEL_BUILD_INFO_TEXT_SIZE 128u
#define KERNEL_LAN_DISCOVERY_TEXT_SIZE 128u
#define KERNEL_LAN_DISCOVERY_DEFAULT_PORT 47777u

#define KERNEL_MAX_WEAPONS 7u

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
#define KERNEL_CAPABILITY_AREA_EFFECT_WEAPONS UINT64_C(0x0000000000400000)
#define KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS UINT64_C(0x0000000000800000)
#define KERNEL_CAPABILITY_BEAM_WEAPONS UINT64_C(0x0000000001000000)
#define KERNEL_CAPABILITY_HOMING_PROJECTILES UINT64_C(0x0000000002000000)
#define KERNEL_CAPABILITY_LAN_DISCOVERY UINT64_C(0x0000000004000000)
#define KERNEL_CAPABILITY_GAMEPLAY_CATALOG UINT64_C(0x0000000008000000)
#define KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH UINT64_C(0x0000000010000000)
#define KERNEL_CAPABILITY_DEBUG_RECORDS UINT64_C(0x0000000020000000)
#define KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY UINT64_C(0x0000000040000000)
#define KERNEL_CAPABILITY_BENCHMARK_STATS UINT64_C(0x0000000080000000)
#define KERNEL_CAPABILITY_NETWORK_STATS UINT64_C(0x0000000100000000)

#define KERNEL_COLLISION_LAYER_PLAYER UINT32_C(0x00000001)
#define KERNEL_COLLISION_LAYER_ENEMY UINT32_C(0x00000002)
#define KERNEL_COLLISION_LAYER_PROJECTILE UINT32_C(0x00000004)
#define KERNEL_COLLISION_LAYER_AREA_EFFECT UINT32_C(0x00000008)
#define KERNEL_COLLISION_MASK_DAMAGEABLE \
    (KERNEL_COLLISION_LAYER_PLAYER | KERNEL_COLLISION_LAYER_ENEMY)

#define KERNEL_VISUAL_FLAG_MOVING UINT32_C(0x00000001)
#define KERNEL_VISUAL_FLAG_RELOADING UINT32_C(0x00000002)
#define KERNEL_VISUAL_FLAG_DEAD UINT32_C(0x00000004)

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
    uint32_t combat_state_definition_size;
    uint32_t area_effect_mechanics_definition_size;
    uint32_t area_effect_state_size;
    uint32_t beam_mechanics_definition_size;
    uint32_t beam_state_size;
    uint32_t homing_mechanics_definition_size;
    uint32_t homing_state_size;
    uint32_t lan_discovery_server_config_size;
    uint32_t lan_discovery_query_config_size;
    uint32_t lan_discovery_result_size;
    uint64_t capability_flags;
    uint32_t gameplay_catalog_definition_size;
    uint32_t projectile_template_definition_size;
    uint32_t collider_template_definition_size;
    uint32_t collider_binding_definition_size;
    uint32_t benchmark_stats_size;
    uint32_t network_stats_size;
    uint32_t debug_record_filter_size;
    uint32_t debug_info_size;
    uint32_t collider_shape_query_size;
    uint32_t collider_shape_view_size;
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
    bool has_welcome;
    bool connected;
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
    bool compatible;
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
} KernelEventType;

typedef enum KernelDespawnReason {
    KernelDespawnReason_Destroyed = 0,
    KernelDespawnReason_OutOfRange = 1,
    KernelDespawnReason_Disconnected = 2,
} KernelDespawnReason;

typedef enum InputButton {
    InputButton_MoveJump = 1u << 0,
    InputButton_Fire = 1u << 1,
    InputButton_Reload = 1u << 2,
    InputButton_Sprint = 1u << 3,
    InputButton_Interact = 1u << 4,
    InputButton_Ability1 = 1u << 5,
    InputButton_Dodge = 1u << 6,
    InputButton_Parry = 1u << 7,
} InputButton;

typedef enum KernelWeaponFireMode {
    KernelWeaponFireMode_Hitscan = 0,
    KernelWeaponFireMode_Shotgun = 1,
    KernelWeaponFireMode_Projectile = 2,
    KernelWeaponFireMode_AreaEffect = 3,
    KernelWeaponFireMode_Beam = 4,
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
    KernelProjectileDamageShape_Explosion = 1,
    KernelProjectileDamageShape_PiercingSegment = 2,
} KernelProjectileDamageShape;

typedef struct KernelVec2 {
    float x;
    float y;
} KernelVec2;

typedef struct KernelVec3 {
    float x;
    float y;
    float z;
} KernelVec3;

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

typedef struct KernelConfig {
    KernelMode mode;
    TickConfig tick;
    uint32_t max_render_states;
    uint32_t max_events;
} KernelConfig;

typedef struct PlayerInput {
    uint32_t input_seq;
    uint64_t client_action_time_us;
    uint32_t client_action_id;
    KernelVec2 move;
    KernelVec2 look_delta;
    KernelVec3 aim_dir;
    uint32_t buttons;
    uint8_t selected_weapon;
} PlayerInput;

typedef struct RenderEntityState {
    uint64_t entity_id;
    uint32_t net_id;
    uint16_t entity_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    KernelVec3 velocity;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t animation_state;
    uint32_t visual_flags;
    uint32_t spawn_tick;
    uint32_t client_action_id;
} RenderEntityState;

typedef struct KernelServerEntityCreateInfo {
    uint32_t struct_size;
    uint16_t entity_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    uint16_t animation_state;
    uint32_t visual_flags;
} KernelServerEntityCreateInfo;

typedef struct KernelServerEntityState {
    uint32_t struct_size;
    uint32_t net_id;
    uint16_t entity_type;
    uint32_t owner_peer;
    KernelVec3 position;
    KernelQuat rotation;
    KernelVec3 velocity;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t animation_state;
    uint32_t visual_flags;
    bool valid;
} KernelServerEntityState;

typedef enum KernelColliderShapeType {
    KernelColliderShapeType_Aabb = 0,
    KernelColliderShapeType_Sphere = 1,
} KernelColliderShapeType;

typedef enum KernelColliderPurpose {
    KernelColliderPurpose_Hit = 1u << 0,
    KernelColliderPurpose_Damage = 1u << 1,
    KernelColliderPurpose_Trigger = 1u << 2,
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
    KernelVec3 half_extents;
    float radius;
    uint32_t purpose_flags;
    uint32_t layer_mask;
} KernelColliderTemplateDefinition;

typedef struct KernelColliderBindingDefinition {
    uint32_t struct_size;
    uint16_t entity_type;
    uint16_t reserved0;
    uint32_t collider_template_id;
    KernelVec3 local_position;
    KernelQuat local_rotation;
} KernelColliderBindingDefinition;

typedef struct KernelProjectileTemplateDefinition {
    uint32_t struct_size;
    uint32_t projectile_template_id;
    uint8_t weapon_id;
    uint8_t motion_model;
    uint8_t sync_mode;
    uint8_t hit_response;
    uint8_t damage_shape;
    uint8_t reserved0;
    uint16_t damage;
    float speed;
    float lifetime_seconds;
    float explosion_radius;
    KernelVec3 gravity;
    uint32_t collider_template_id;
    uint32_t explosion_template_id;
    uint32_t collision_mask;
    uint32_t max_hit_count;
} KernelProjectileTemplateDefinition;

typedef struct KernelGameplayCatalogDefinition {
    uint32_t struct_size;
    uint32_t catalog_version;
    uint64_t catalog_hash;
    const KernelProjectileTemplateDefinition* projectile_templates;
    uint32_t projectile_template_count;
    const KernelColliderTemplateDefinition* collider_templates;
    uint32_t collider_template_count;
    const KernelColliderBindingDefinition* collider_bindings;
    uint32_t collider_binding_count;
} KernelGameplayCatalogDefinition;

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
    uint16_t reserved0;
    uint32_t entity_net_id;
    uint32_t purpose_mask;
} KernelColliderShapeQuery;

typedef struct KernelColliderShapeView {
    uint32_t struct_size;
    uint32_t entity_net_id;
    uint16_t entity_type;
    uint16_t reserved0;
    uint32_t collider_template_id;
    uint8_t shape_type;
    uint8_t reserved1;
    uint16_t reserved2;
    KernelVec3 world_center;
    KernelVec3 half_extents;
    float radius;
    uint32_t purpose_flags;
    uint32_t layer_mask;
} KernelColliderShapeView;

typedef struct KernelHomingMechanicsDefinition {
    uint32_t struct_size;
    uint8_t homing_mode;
    uint8_t sync_mode;
    uint16_t reserved0;
    uint32_t boost_ticks;
    float lock_on_range;
    float lose_target_range;
    float lock_cone_degrees;
    float max_turn_rate_degrees_per_second;
    float acceleration;
    float max_speed;
} KernelHomingMechanicsDefinition;

typedef struct KernelProjectileMechanicsDefinition {
    uint32_t struct_size;
    uint8_t motion_model;
    uint8_t hit_response;
    uint8_t damage_shape;
    uint8_t reserved0;
    float speed;
    float lifetime_seconds;
    float explosion_radius;
    KernelVec3 gravity;
    uint32_t collision_mask;
    uint32_t max_hit_count;
    KernelHomingMechanicsDefinition homing;
} KernelProjectileMechanicsDefinition;

typedef struct KernelAreaEffectMechanicsDefinition {
    uint32_t struct_size;
    float radius;
    uint16_t damage_per_interval;
    uint32_t damage_interval_ticks;
    uint32_t lifetime_ticks;
    float spawn_distance;
    uint32_t collision_mask;
} KernelAreaEffectMechanicsDefinition;

typedef struct KernelBeamMechanicsDefinition {
    uint32_t struct_size;
    float length;
    float radius;
    uint16_t damage_per_second;
    uint32_t lifetime_ticks;
    uint32_t collision_mask;
} KernelBeamMechanicsDefinition;

typedef struct KernelWeaponMechanicsDefinition {
    uint32_t struct_size;
    uint8_t weapon_id;
    uint8_t fire_mode;
    uint16_t magazine_size;
    uint16_t damage;
    uint32_t cooldown_ticks;
    uint32_t reload_ticks;
    float max_range;
    uint8_t pellet_count;
    float pellet_spread;
    KernelProjectileMechanicsDefinition projectile;
    KernelAreaEffectMechanicsDefinition area_effect;
    KernelBeamMechanicsDefinition beam;
} KernelWeaponMechanicsDefinition;

typedef struct KernelBeamState {
    uint32_t struct_size;
    uint32_t net_id;
    uint32_t owner_peer;
    uint32_t shooter_net_id;
    KernelVec3 origin;
    KernelVec3 direction;
    float length;
    float radius;
    uint16_t damage_per_second;
    uint32_t expire_tick;
    uint8_t source_code;
    uint32_t collision_mask;
    bool valid;
} KernelBeamState;

typedef struct KernelAreaEffectState {
    uint32_t struct_size;
    uint32_t net_id;
    uint32_t owner_peer;
    float radius;
    uint16_t damage_per_interval;
    uint32_t damage_interval_ticks;
    uint32_t expire_tick;
    uint8_t source_code;
    uint32_t collision_mask;
    bool valid;
} KernelAreaEffectState;

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
    float max_turn_rate_degrees_per_second;
    float acceleration;
    float max_speed;
    bool valid;
} KernelHomingState;

typedef struct KernelCombatStateDefinition {
    uint32_t struct_size;
    uint16_t hp;
    uint16_t max_hp;
    uint8_t active_weapon_id;
    float move_speed_meters_per_second;
    KernelVec3 hitbox_center;
    KernelVec3 hitbox_half_extents;
    uint16_t ammo[KERNEL_MAX_WEAPONS];
    uint16_t reserve_ammo[KERNEL_MAX_WEAPONS];
} KernelCombatStateDefinition;

typedef struct KernelEvent {
    KernelEventType type;
    uint32_t tick;
    uint32_t net_id;
    uint32_t peer_id;
    uint32_t code;
    uint64_t event_time_us;
    uint64_t presentation_time_us;
} KernelEvent;

#ifdef __cplusplus
}
#endif

#endif  // KERNEL_PUBLIC_KERNEL_TYPES_H_
