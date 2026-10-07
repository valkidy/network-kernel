#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

bool all_digits(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            return false;
        }
    }
    return true;
}

bool has_version_revision_suffix(const char* value) {
    const std::string text = value == nullptr ? "" : value;
    constexpr const char* kPrefix = "0.7.0+r";
    if (text.rfind(kPrefix, 0) != 0 || text.size() == std::strlen(kPrefix)) {
        return false;
    }
    return all_digits(text.c_str() + std::strlen(kPrefix));
}

KernelProjectileTemplateDefinition projectile_template(
    std::uint32_t template_id,
    std::uint8_t weapon_id,
    std::uint8_t projectile_type = KernelProjectileType_Standard) {
    KernelProjectileTemplateDefinition projectile_template{};
    projectile_template.struct_size = sizeof(projectile_template);
    projectile_template.projectile_template_id = template_id;
    projectile_template.weapon_id = weapon_id;
    projectile_template.mechanics.struct_size =
        sizeof(KernelProjectileMechanicsDefinition);
    projectile_template.mechanics.projectile_type = projectile_type;
    projectile_template.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    projectile_template.mechanics.sync_mode =
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    projectile_template.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    projectile_template.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    projectile_template.mechanics.damage = 5;
    projectile_template.mechanics.speed = 35.0f;
    projectile_template.mechanics.lifetime_ticks = 75;
    projectile_template.mechanics.gravity = KernelVec3{0.0f, 0.0f, 0.0f};
    projectile_template.mechanics.collider_template_id = 10;
    projectile_template.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    projectile_template.mechanics.max_hit_count = 1;
    return projectile_template;
}

void server_set_entity_health_updates_hp_only() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);

    KernelColliderTemplateDefinition collider_template{};
    collider_template.struct_size = sizeof(collider_template);
    collider_template.template_id = 10;
    collider_template.shape_type = KernelColliderShapeType_Aabb;
    collider_template.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider_template.shape_params = KernelVec4{0.25f, 0.25f, 0.25f, 0.0f};
    collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider_template.purpose_flags = KernelColliderPurpose_Damage;
    std::array<KernelColliderTemplateDefinition, 1> collider_templates = {
        collider_template,
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 33;
    catalog.catalog_hash = 0x33445566ull;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(collider_templates.size());
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
    KernelGameplayCatalogLoadOptions invalid_options{};
    require(!Kernel_LoadGameplayCatalog(kernel, &catalog, &invalid_options));
    std::uint32_t static_scene_rejected = 1u;
    KernelGameplayCatalogLoadOptions options{};
    options.struct_size = sizeof(options);
    options.out_static_scene_rejected = &static_scene_rejected;
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, &options));
    require(static_scene_rejected == 0u);
    require(Kernel_StartDedicatedServer(kernel, 8042));

    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 1;
    create_info.actor_type = KernelActorType_Agent;
    create_info.position = KernelVec3{1.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(combat_state);
    combat_state.hp = 240;
    combat_state.max_hp = 240;
    combat_state.collider_template_id = 10;
    combat_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat_state.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    require(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat_state));

    require(Kernel_ServerSetEntityHealth(kernel, net_id, 123));
    require(!Kernel_ServerSetEntityHealth(kernel, 0xffffffffu, 1));
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    require(state.hp == 123);
    require(state.max_hp == 240);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    KernelAbiInfo abi_info{};
    require(Kernel_GetAbiInfo(&abi_info, sizeof(abi_info)));
    require(abi_info.struct_size == sizeof(KernelAbiInfo));
    require(abi_info.abi_version == KERNEL_ABI_VERSION);
    require(abi_info.kernel_config_size == sizeof(KernelConfig));
    require(abi_info.player_input_size == sizeof(KernelPlayerInput));
    require(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    require(abi_info.kernel_event_size == sizeof(KernelEvent));
    require(abi_info.server_entity_create_info_size ==
           sizeof(KernelServerEntityCreateInfo));
    require(abi_info.server_entity_state_size == sizeof(KernelServerEntityState));
    require(abi_info.item_template_definition_size ==
           sizeof(KernelItemTemplateDefinition));
    require(abi_info.gameplay_request_size == sizeof(KernelGameplayRequest));
    require(abi_info.gameplay_request_outcome_size ==
           sizeof(KernelGameplayRequestOutcome));
    require(abi_info.item_instance_view_size == sizeof(KernelItemInstanceView));
    require(abi_info.inventory_container_view_size ==
           sizeof(KernelInventoryContainerView));
    require(abi_info.inventory_delta_size == sizeof(KernelInventoryDelta));
    require(abi_info.bone_local_transform_size ==
           sizeof(KernelBoneLocalTransform));
    require(abi_info.skeleton_render_state_size ==
           sizeof(KernelSkeletonRenderState));
    require(abi_info.skeleton_render_state_result_size ==
           sizeof(KernelSkeletonRenderStateResult));
    require(abi_info.skeleton_asset_definition_size ==
           sizeof(KernelSkeletonAssetDefinition));
    require(abi_info.skeleton_binding_definition_size ==
           sizeof(KernelSkeletonBindingDefinition));
    require(abi_info.skeleton_leg_definition_size ==
           sizeof(KernelSkeletonLegDefinition));
    require(abi_info.status_effect_view_size == sizeof(KernelStatusEffectView));
    // Fetched again under require: the call above sits inside assert(), which
    // an opt build compiles out along with the call itself.
    KernelAbiInfo weapon_abi_info{};
    require(Kernel_GetAbiInfo(&weapon_abi_info, sizeof(weapon_abi_info)));
    require(weapon_abi_info.local_weapon_state_size ==
            sizeof(KernelLocalWeaponState));
    require((weapon_abi_info.capability_flags &
             KERNEL_CAPABILITY_LOCAL_WEAPON_STATE) != 0u);
    require((abi_info.capability_flags &
            KERNEL_CAPABILITY_ITEM_PROP_SYSTEM) != 0u);
    require(abi_info.weapon_mechanics_definition_size ==
           sizeof(KernelWeaponMechanicsDefinition));
    require(abi_info.projectile_mechanics_definition_size ==
           sizeof(KernelProjectileMechanicsDefinition));
    require(abi_info.homing_mechanics_definition_size ==
           sizeof(KernelHomingMechanicsDefinition));
    require(abi_info.homing_state_size == sizeof(KernelHomingState));
    require(abi_info.lan_discovery_server_config_size ==
           sizeof(KernelLANDiscoveryServerConfig));
    require(abi_info.lan_discovery_query_config_size ==
           sizeof(KernelLANDiscoveryQueryConfig));
    require(abi_info.lan_discovery_result_size ==
           sizeof(KernelLANDiscoveryResult));
    require(abi_info.combat_state_definition_size ==
           sizeof(KernelCombatStateDefinition));
    require(abi_info.gameplay_catalog_definition_size ==
           sizeof(KernelGameplayCatalogDefinition));
    require(
        abi_info.prop_population_rule_definition_size ==
        sizeof(KernelPropPopulationRuleDefinition));
    require(abi_info.gameplay_catalog_load_result_size ==
           sizeof(KernelGameplayCatalogLoadResult));
    require(abi_info.gameplay_catalog_load_options_size ==
           sizeof(KernelGameplayCatalogLoadOptions));
    require(abi_info.actor_template_definition_size ==
           sizeof(KernelActorTemplateDefinition));
    require(abi_info.projectile_template_definition_size ==
           sizeof(KernelProjectileTemplateDefinition));
    require(abi_info.collider_template_definition_size ==
           sizeof(KernelColliderTemplateDefinition));
    require(abi_info.collider_binding_definition_size ==
           sizeof(KernelColliderBindingDefinition));
    require(abi_info.benchmark_stats_size == sizeof(KernelBenchmarkStats));
    require(
        abi_info.network_stats_config_size ==
        sizeof(KernelNetworkStatsConfig));
    require(abi_info.network_stats_size == sizeof(KernelNetworkStats));
    require(abi_info.debug_record_filter_size == sizeof(KernelDebugRecordFilter));
    require(abi_info.debug_info_size == sizeof(KernelDebugInfo));
    require(abi_info.collider_shape_query_size == sizeof(KernelColliderShapeQuery));
    require(abi_info.collider_shape_view_size == sizeof(KernelColliderShapeView));
    require(abi_info.agent_vision_config_size == sizeof(KernelAgentVisionConfig));
    require(abi_info.vision_state_query_size == sizeof(KernelVisionStateQuery));
    require(abi_info.vision_state_view_size == sizeof(KernelVisionStateView));
    require(
        abi_info.gameplay_catalog_manifest_size ==
        sizeof(KernelGameplayCatalogManifest));
    require(
        abi_info.gameplay_catalog_sync_status_size ==
        sizeof(KernelGameplayCatalogSyncStatus));
    require(abi_info.action_template_definition_size ==
           sizeof(KernelActionTemplateDefinition));
    require(abi_info.action_runtime_view_size == sizeof(KernelActionRuntimeView));
    require(abi_info.local_action_result_size == sizeof(KernelLocalActionResult));
    require(
        abi_info.remote_action_presentation_event_size ==
        sizeof(KernelRemoteActionPresentationEvent));
    require(abi_info.action_intent_size == sizeof(KernelActionIntent));
    require(abi_info.action_input_size == sizeof(KernelActionInput));
    require((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_MODE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_DEDICATED_SERVER_MODE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_INPUT_SUBMISSION) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_POLLING) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_CLIENT_PREDICTION) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SNAPSHOT_INTERPOLATION) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_HITSCAN) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_PLAYER_INFO) != 0);
    require(
        (abi_info.capability_flags & KERNEL_CAPABILITY_GAMEPLAY_CATALOG_SYNC) !=
        0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_DESTROY) != 0);
    require(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_TRANSFORM_WRITE) !=
        0);
    require(
        (abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_VELOCITY_WRITE) !=
        0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_STATE_WRITE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_ENTITY_QUERY) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_RELEVANCE_FILTER) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_RENDER_STATES_AT_TIME) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_WEAPON_METADATA_QUERY) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_HOMING_PROJECTILES) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LAN_DISCOVERY) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_GAMEPLAY_CATALOG) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_PROJECTILE_SPAWN_BATCH) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_DEBUG_RECORDS) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_BENCHMARK_STATS) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_NETWORK_STATS) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_VISION_STATE_QUERY) != 0);
    require(abi_info.local_player_info_size == sizeof(KernelLocalPlayerInfo));
    require(
        (abi_info.capability_flags &
         KERNEL_CAPABILITY_SERVER_ENTITY_MOVEMENT_MASK_WRITE) != 0);
    // Rejected before it can reach a world: an unknown movement layer bit, and
    // a null handle.
    require(!Kernel_ServerSetEntityMovementCollisionMask(nullptr, 1u, 0u));
    require(KERNEL_ABI_VERSION == 72u);
    require(Kernel_QueryStatusEffects(nullptr, 1u, nullptr, 0u) == 0u);
    require((abi_info.capability_flags &
            KERNEL_CAPABILITY_SKELETON_RENDER_STATES) != 0u);
    require((abi_info.capability_flags &
            KERNEL_CAPABILITY_SKELETON_BIND_POSE) != 0u);
    require(Kernel_GetSkeletonBindPose(
               nullptr, 1u, UINT64_C(1), nullptr, 0u) == 0u);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_CONTROL_PLANE_RPC) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_ACTION_TIMELINE) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_ACTION_RESULTS) != 0);
    require(
        (abi_info.capability_flags &
         KERNEL_CAPABILITY_REMOTE_ACTION_PRESENTATION) != 0);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_ACTION_INTENTS) != 0);
    require(sizeof(KernelActionIntent) == 8u);
    require(sizeof(KernelActionInput) == 8u);
    require(sizeof(KernelLocalActionResult) == 12u);
    require(sizeof(KernelRemoteActionPresentationEvent) == 36u);
    require(sizeof(KernelVec4) == 16u);
    require((abi_info.capability_flags & KERNEL_CAPABILITY_ENTITY_LIFECYCLE_EVENTS) != 0);
    require(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED == 0u);
    require(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS == 1u);
    require(KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNSUPPORTED_CATALOG_VERSION == 3u);
    require(KERNEL_MAX_WEAPON_SLOTS == 4u);
    require(KernelColliderShapeType_Cone == 4u);
    require(KernelColliderPurpose_Vision == (1u << 3));
    require(KERNEL_COLLISION_LAYER_AGENT_VISION == 0x00000010u);
    require(KERNEL_COLLISION_LAYER_NEUTRAL == 0x00000020u);
    require((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_NEUTRAL) != 0u);
    require(KERNEL_LAN_DISCOVERY_DEFAULT_PORT == 47777u);
    require(offsetof(KernelPlayerInput, client_action_time_us) > offsetof(KernelPlayerInput, input_seq));
    require(offsetof(KernelPlayerInput, action_intent) >
           offsetof(KernelPlayerInput, client_action_time_us));
    require(offsetof(KernelEvent, event_time_us) > offsetof(KernelEvent, code));
    require(offsetof(KernelEvent, presentation_time_us) > offsetof(KernelEvent, event_time_us));
    require(offsetof(RenderEntityState, entity_id) == 0u);
    require(offsetof(RenderEntityState, net_id) > offsetof(RenderEntityState, entity_id));
    require(offsetof(RenderEntityState, actor_type) > offsetof(RenderEntityState, entity_type));
    require(offsetof(RenderEntityState, hp) > offsetof(RenderEntityState, velocity));
    require(offsetof(RenderEntityState, max_hp) > offsetof(RenderEntityState, hp));
    require(offsetof(RenderEntityState, status) > offsetof(RenderEntityState, action_instance_id));
    require(offsetof(RenderEntityState, template_id) >
           offsetof(RenderEntityState, status));
    require(offsetof(RenderEntityState, collider_template_id) >
           offsetof(RenderEntityState, template_id));
    require(offsetof(KernelNetworkStats, replication_metadata_timeout_count) >
           offsetof(KernelNetworkStats, loss_ratio));
    require(offsetof(KernelNetworkStats, replication_stale_snapshot_drop_count) >
           offsetof(KernelNetworkStats, replication_metadata_timeout_count));
    require(offsetof(KernelCombatStateDefinition, collider_template_id) >
           offsetof(KernelCombatStateDefinition, active_weapon_slot));
    require(offsetof(KernelServerEntityState, active_weapon_slot) >
           offsetof(KernelServerEntityState, actor_template_id));
    require(offsetof(KernelServerEntityState, weapon_ids) >
           offsetof(KernelServerEntityState, active_weapon_slot));
    require(offsetof(KernelServerEntityState, ammo) >
           offsetof(KernelServerEntityState, weapon_ids));
    require(offsetof(KernelServerEntityState, reserve_magazines) >
           offsetof(KernelServerEntityState, ammo));
    require(offsetof(KernelServerEntityState, is_reloading) >
           offsetof(KernelServerEntityState, reserve_magazines));
    require(offsetof(KernelServerEntityState, reload_remaining_ticks) >
           offsetof(KernelServerEntityState, is_reloading));
    require(offsetof(KernelServerEntityState, action) >
           offsetof(KernelServerEntityState, reload_remaining_ticks));
    require(offsetof(RenderEntityState, action) >
           offsetof(RenderEntityState, collider_template_id));
    require(sizeof(RenderEntityState) == 144u);
    require(offsetof(KernelWeaponMechanicsDefinition, reserve_magazines) >
           offsetof(KernelWeaponMechanicsDefinition, magazine_size));
    require(offsetof(KernelWeaponMechanicsDefinition, damage) >
           offsetof(KernelWeaponMechanicsDefinition, reserve_magazines));
    require(offsetof(KernelWeaponMechanicsDefinition, fire_action_template_id) >
           offsetof(KernelWeaponMechanicsDefinition, segment_collider_template_id));
    require(offsetof(KernelColliderTemplateDefinition, shape_params) >
           offsetof(KernelColliderTemplateDefinition, center));
    require(offsetof(KernelAgentVisionConfig, vision_collider_template_id) >
           offsetof(KernelAgentVisionConfig, camp));
    require(KernelActorType_Player == 1u);
    require(KernelActorType_Agent == 2u);
    require(RenderEntityStatus_Active == 0u);
    require(RenderEntityStatus_Predicted == 1u);
    require(RenderEntityStatus_Stale == 2u);
    require((KERNEL_VISUAL_FLAG_HP_UNKNOWN & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    require(KERNEL_VISUAL_FLAG_AIMING == 0x00000100u);
    require(KERNEL_VISUAL_FLAG_FIRING == 0x00000200u);
    require(InputButton_Aim == (1u << 8));
    require(sizeof(KernelEntityLifecycleEvent) > 0u);
    require(!Kernel_GetAbiInfo(nullptr, sizeof(abi_info)));
    require(!Kernel_GetAbiInfo(&abi_info, sizeof(abi_info) - 1));
    require(Kernel_PollLocalActionResults(nullptr, nullptr, 0) == 0u);
    require(Kernel_PollRemoteActionPresentationEvents(nullptr, nullptr, 0) == 0u);

    KernelBuildInfo build_info{};
    require(Kernel_GetBuildInfo(&build_info, sizeof(build_info)));
    require(build_info.struct_size == sizeof(KernelBuildInfo));
    require(std::strcmp(build_info.module_name, "network_kernel") == 0);
    require(build_info.module_file_name[0] != '\0');
    require(has_version_revision_suffix(build_info.module_version));
    require(build_info.protocol_version == 3u);
    require(build_info.snapshot_schema_version == 28u);
    require(build_info.packet_schema_version == 28u);
    require(build_info.git_commit[0] != '\0');
    require(std::strcmp(build_info.git_commit, "unknown") != 0);
    require(std::strcmp(build_info.module_version, build_info.git_commit) != 0);
    require(all_digits(build_info.build_timestamp));
    require(build_info.build_platform[0] != '\0');
    require(build_info.build_config[0] != '\0');
    require(build_info.compiler_info[0] != '\0');
    require(!Kernel_GetBuildInfo(nullptr, sizeof(build_info)));
    require(!Kernel_GetBuildInfo(&build_info, sizeof(build_info) - 1));

    require(Kernel_Create(nullptr) == nullptr);
    KernelPhysicsConfig physics_config{};
    physics_config.struct_size = sizeof(physics_config);
    require(!Kernel_SetPhysicsConfig(nullptr, &physics_config));
    require(!Kernel_SetStaticCollisionScene(nullptr, nullptr));

    KernelConfig physics_kernel_config{};
    physics_kernel_config.mode = KernelMode_DedicatedServer;
    physics_kernel_config.tick.server_tick_rate = 30;
    physics_kernel_config.tick.snapshot_rate = 15;
    KernelHandle* physics_kernel = Kernel_Create(&physics_kernel_config);
    require(physics_kernel != nullptr);
    KernelPhysicsConfig invalid_physics_config = physics_config;
    invalid_physics_config.struct_size = sizeof(invalid_physics_config) - 1;
    require(!Kernel_SetPhysicsConfig(physics_kernel, &invalid_physics_config));
    invalid_physics_config = physics_config;
    invalid_physics_config.physics_simulation = 2;
    require(!Kernel_SetPhysicsConfig(physics_kernel, &invalid_physics_config));
    physics_config.physics_workers = 2;
    require(Kernel_SetPhysicsConfig(physics_kernel, &physics_config));
    KernelStaticCollisionSceneConfig invalid_scene{};
    invalid_scene.struct_size = sizeof(invalid_scene);
    require(!Kernel_SetStaticCollisionScene(physics_kernel, &invalid_scene));
    Kernel_Destroy(physics_kernel);

    physics_kernel = Kernel_Create(&physics_kernel_config);
    require(physics_kernel != nullptr);
    physics_config.physics_simulation = 1;
    physics_config.physics_workers = 0;
    require(Kernel_SetPhysicsConfig(physics_kernel, &physics_config));
    require(!Kernel_StartDedicatedServer(physics_kernel, 7899));
    Kernel_Destroy(physics_kernel);
    physics_config.physics_simulation = 0;
    KernelRpcRequestId rpc_request_id = 0;
    const char rpc_request[] =
        R"({"jsonrpc":"2.0","id":1,"method":"dev.ping","params":{}})";
    require(!Kernel_InvokeRpcCommand(
        nullptr,
        rpc_request,
        sizeof(rpc_request) - 1,
        &rpc_request_id));
    std::uint32_t rpc_response_size = 0;
    require(!Kernel_PollRpcResponse(
        nullptr,
        rpc_request_id,
        nullptr,
        0,
        &rpc_response_size));
    require(!Kernel_StartClient(nullptr, "127.0.0.1:9"));
    require(!Kernel_StartListenServer(nullptr, 7777));
    require(!Kernel_StartDedicatedServer(nullptr, 7777));
    KernelLANDiscoveryHandle* discovery = Kernel_LANDiscovery_Create();
    require(discovery != nullptr);
    Kernel_LANDiscovery_Destroy(discovery);
    Kernel_LANDiscovery_Destroy(nullptr);
    require(!Kernel_LANDiscovery_StartServer(nullptr, nullptr));
    Kernel_LANDiscovery_StopServer(nullptr);
    require(!Kernel_LANDiscovery_Query(nullptr, nullptr));
    require(Kernel_LANDiscovery_PollResults(nullptr, nullptr, 0) == 0);
    Kernel_LANDiscovery_ClearResults(nullptr);
    Kernel_Update(nullptr, 1.0f / 30.0f);
    Kernel_SubmitPlayerInput(nullptr, 1, nullptr);
    require(!Kernel_LoadGameplayCatalog(nullptr, nullptr, nullptr));
    require(Kernel_GetRenderStates(nullptr, nullptr, 0) == 0);
    require(Kernel_GetRenderStatesAtTime(nullptr, 0, nullptr, 0) == 0);
    KernelSkeletonRenderStateResult null_skeleton_result{};
    null_skeleton_result.struct_size = sizeof(null_skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
               nullptr, nullptr, 0, nullptr, 0, &null_skeleton_result) == 0u);
    require(null_skeleton_result.status ==
           KERNEL_SKELETON_RENDER_STATUS_INVALID_ARGUMENT);
    require(Kernel_PollEvents(nullptr, nullptr, 0) == 0);
    require(Kernel_PollEntityLifecycleEvents(nullptr, nullptr, 0) == 0);
    KernelBenchmarkStats benchmark_stats{};
    benchmark_stats.struct_size = sizeof(benchmark_stats);
    require(!Kernel_GetBenchmarkStats(nullptr, &benchmark_stats));
    KernelNetworkStats network_stats{};
    network_stats.struct_size = sizeof(network_stats);
    require(!Kernel_GetNetworkStats(nullptr, &network_stats));
    KernelDebugRecordFilter debug_filter{};
    debug_filter.struct_size = sizeof(debug_filter);
    std::array<KernelDebugInfo, 4> debug_records{};
    for (KernelDebugInfo& debug_record : debug_records) {
        debug_record.struct_size = sizeof(KernelDebugInfo);
    }
    require(Kernel_PollDebugRecords(
               nullptr,
               &debug_filter,
               debug_records.data(),
               static_cast<std::uint32_t>(debug_records.size())) == 0);
    KernelColliderShapeQuery collider_query{};
    collider_query.struct_size = sizeof(collider_query);
    std::array<KernelColliderShapeView, 4> collider_shapes{};
    for (KernelColliderShapeView& shape : collider_shapes) {
        shape.struct_size = sizeof(KernelColliderShapeView);
    }
    require(Kernel_QueryColliderShapes(
               nullptr,
               &collider_query,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 0);
    KernelVisionStateQuery vision_query{};
    vision_query.struct_size = sizeof(vision_query);
    std::array<KernelVisionStateView, 4> vision_states{};
    for (KernelVisionStateView& vision_state : vision_states) {
        vision_state.struct_size = sizeof(KernelVisionStateView);
    }
    require(Kernel_QueryVisionState(
               nullptr,
               &vision_query,
               vision_states.data(),
               static_cast<std::uint32_t>(vision_states.size())) == 0);
    require(Kernel_GetProjectileTemplates(nullptr, nullptr, 0) == 0);
    KernelActionTemplateDefinition null_action_template{};
    null_action_template.struct_size = sizeof(null_action_template);
    require(!Kernel_GetActionTemplate(nullptr, 1, &null_action_template));
    require(Kernel_GetColliderTemplates(nullptr, nullptr, 0) == 0);
    require(Kernel_GetColliderBindings(nullptr, nullptr, 0) == 0);
    KernelLocalPlayerInfo local_info{};
    require(!Kernel_GetLocalPlayerInfo(nullptr, &local_info));
    require(!Kernel_GetLocalPlayerInfo(nullptr, nullptr));
    KernelLocalWeaponState local_weapon{};
    local_weapon.struct_size = sizeof(local_weapon);
    require(!Kernel_GetLocalWeaponState(nullptr, &local_weapon));
    require(!Kernel_GetLocalWeaponState(nullptr, nullptr));
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = 1;
    create_info.actor_type = KernelActorType_Agent;
    create_info.position = KernelVec3{1.0f, 0.0f, 0.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t created_net_id = 0;
    require(!Kernel_ServerCreateEntity(nullptr, &create_info, &created_net_id));
    require(!Kernel_ServerDestroyEntity(nullptr, 1, KernelDespawnReason_Destroyed));
    require(!Kernel_ServerSetEntityTransform(
        nullptr,
        1,
        &create_info.position,
        &create_info.rotation));
    require(!Kernel_ServerSetEntityVelocity(nullptr, 1, &create_info.position));
    require(!Kernel_ServerSetEntityState(nullptr, 1, 2, 3));
    require(!Kernel_ServerSetEntityHealth(nullptr, 1, 2));
    KernelPlayerInput server_entity_input{};
    require(!Kernel_ServerSubmitEntityInput(nullptr, 1, &server_entity_input));
    KernelCombatStateDefinition combat_state{};
    combat_state.struct_size = sizeof(combat_state);
    require(!Kernel_ServerSetEntityCombatState(nullptr, 1, &combat_state));
    KernelWeaponMechanicsDefinition weapon_mechanics{};
    weapon_mechanics.struct_size = sizeof(weapon_mechanics);
    require(!Kernel_ServerValidateMechanicsConfig(nullptr));
    require(!Kernel_ServerSetEntityWeaponMechanics(nullptr, 1, &weapon_mechanics));
    require(!Kernel_ServerClearEntityWeaponMechanics(nullptr, 1, 0));
    require(!Kernel_ServerGetEntityWeaponMechanics(nullptr, 1, 0, &weapon_mechanics));
    KernelHomingState homing_state{};
    homing_state.struct_size = sizeof(homing_state);
    require(!Kernel_ServerGetHomingState(nullptr, 1, &homing_state));
    KernelServerEntityState server_state{};
    server_state.struct_size = sizeof(server_state);
    require(!Kernel_ServerGetEntityState(nullptr, 1, &server_state));
    require(Kernel_ServerQueryEntities(nullptr, 0, &server_state, 1) == 0);
    KernelAgentVisionConfig vision_config{};
    vision_config.struct_size = sizeof(vision_config);
    require(!Kernel_ServerSetEntityVisionConfig(nullptr, 1, &vision_config));
    require(!Kernel_ServerClearEntityVisionConfig(nullptr, 1));

    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;

    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    KernelProjectileTemplateDefinition projectile_template =
        ::projectile_template(3, 3);
    KernelProjectileTemplateDefinition area_projectile_template =
        ::projectile_template(4, 4, KernelProjectileType_AreaEffect);
    area_projectile_template.mechanics.area_effect.struct_size =
        sizeof(KernelAreaEffectMechanicsDefinition);
    area_projectile_template.mechanics.area_effect.radius = 2.5f;
    area_projectile_template.mechanics.area_effect.damage_per_interval = 7;
    area_projectile_template.mechanics.area_effect.damage_interval_ticks = 3;
    area_projectile_template.mechanics.area_effect.lifetime_ticks = 9;
    area_projectile_template.mechanics.area_effect.collision_mask =
        KERNEL_COLLISION_MASK_DAMAGEABLE;
    KernelProjectileTemplateDefinition beam_projectile_template =
        ::projectile_template(5, 5, KernelProjectileType_Beam);
    beam_projectile_template.mechanics.beam.struct_size =
        sizeof(KernelBeamMechanicsDefinition);
    beam_projectile_template.mechanics.beam.length = 6.0f;
    beam_projectile_template.mechanics.beam.radius = 0.25f;
    beam_projectile_template.mechanics.beam.damage_per_tick = 1;
    beam_projectile_template.mechanics.beam.lifetime_ticks = 2;
    beam_projectile_template.mechanics.beam.collision_mask =
        KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    KernelProjectileTemplateDefinition homing_projectile_template =
        ::projectile_template(6, 6);
    homing_projectile_template.mechanics.motion_model =
        KernelProjectileMotionModel_Homing;
    homing_projectile_template.mechanics.homing.struct_size =
        sizeof(KernelHomingMechanicsDefinition);
    homing_projectile_template.mechanics.homing.homing_mode =
        KernelHomingMode_FireAndForget;
    homing_projectile_template.mechanics.homing.sync_mode =
        KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
    homing_projectile_template.mechanics.homing.boost_ticks = 1;
    homing_projectile_template.mechanics.homing.lock_on_range = 25.0f;
    homing_projectile_template.mechanics.homing.lose_target_range = 30.0f;
    homing_projectile_template.mechanics.homing.lock_cone_degrees = 75.0f;
    homing_projectile_template.mechanics.homing.max_turn_degrees_per_tick =
        12.0f;
    homing_projectile_template.mechanics.homing.acceleration = 20.0f;
    homing_projectile_template.mechanics.homing.max_speed = 40.0f;
    std::array<KernelProjectileTemplateDefinition, 4> projectile_templates = {
        projectile_template,
        area_projectile_template,
        beam_projectile_template,
        homing_projectile_template,
    };
    KernelColliderTemplateDefinition collider_template{};
    collider_template.struct_size = sizeof(collider_template);
    collider_template.template_id = 10;
    collider_template.shape_type = KernelColliderShapeType_Aabb;
    collider_template.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider_template.shape_params = KernelVec4{0.25f, 0.25f, 0.25f, 0.0f};
    collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider_template.purpose_flags = KernelColliderPurpose_Damage;
    KernelColliderTemplateDefinition vision_collider_template{};
    vision_collider_template.struct_size = sizeof(vision_collider_template);
    vision_collider_template.template_id = 12;
    vision_collider_template.shape_type = KernelColliderShapeType_Cone;
    vision_collider_template.center = KernelVec3{0.0f, 0.0f, 0.0f};
    vision_collider_template.shape_params = KernelVec4{10.0f, 90.0f, 0.0f, 0.0f};
    vision_collider_template.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    vision_collider_template.purpose_flags = KernelColliderPurpose_Vision;
    std::array<KernelColliderTemplateDefinition, 2> initial_collider_templates = {
        collider_template,
        vision_collider_template,
    };
    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = 2;
    actor_template.entity_type = 1;
    actor_template.actor_type = KernelActorType_Agent;
    actor_template.collider_template_id = 10;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    actor_template.vision.vision_collider_template_id = 12;
    actor_template.vision.local_origin = KernelVec3{0.0f, 1.5f, 0.0f};
    actor_template.vision.local_forward = KernelVec3{-1.0f, 0.0f, 0.0f};
    KernelActionTemplateDefinition rocket_action{};
    rocket_action.struct_size = sizeof(rocket_action);
    rocket_action.action_template_id = 1001;
    rocket_action.trigger_mode = KernelActionTriggerMode_Press;
    rocket_action.flags = KernelActionTemplateFlag_CancelOnDeath |
                          KernelActionTemplateFlag_CancelOnWeaponChange |
                          KernelActionTemplateFlag_CancelBeforeFirstCommit;
    rocket_action.ammo_cost_per_commit = 1;
    rocket_action.commit_offset_ticks = 3;
    rocket_action.commit_interval_ticks = 30;
    rocket_action.max_commit_count = 1;
    rocket_action.recovery_ticks = 8;

    KernelActionTemplateDefinition rifle_action{};
    rifle_action.struct_size = sizeof(rifle_action);
    rifle_action.action_template_id = 1002;
    rifle_action.trigger_mode = KernelActionTriggerMode_Hold;
    rifle_action.flags = KernelActionTemplateFlag_CancelOnRelease |
                         KernelActionTemplateFlag_CancelOnDeath |
                         KernelActionTemplateFlag_CancelOnWeaponChange;
    rifle_action.ammo_cost_per_commit = 1;
    rifle_action.commit_interval_ticks = 3;
    rifle_action.recovery_ticks = 4;
    rifle_action.hold_input_timeout_ticks = 6;

    KernelActionTemplateDefinition beam_action = rifle_action;
    beam_action.action_template_id = 1003;
    beam_action.commit_offset_ticks = 5;
    beam_action.commit_interval_ticks = 1;
    beam_action.recovery_ticks = 8;
    KernelActionTemplateDefinition reload_action{};
    reload_action.struct_size = sizeof(reload_action);
    reload_action.action_template_id = 1004;
    reload_action.trigger_mode = KernelActionTriggerMode_Press;
    reload_action.flags = KernelActionTemplateFlag_CancelOnDeath |
                          KernelActionTemplateFlag_CancelOnWeaponChange |
                          KernelActionTemplateFlag_CancelBeforeFirstCommit;
    reload_action.commit_offset_ticks = 30;
    reload_action.max_commit_count = 1;
    KernelActionTemplateDefinition instant_fire_action = rocket_action;
    instant_fire_action.action_template_id = 1005;
    instant_fire_action.commit_offset_ticks = 0;
    instant_fire_action.recovery_ticks = 0;
    std::array<KernelActionTemplateDefinition, 5> action_templates = {
        rocket_action,
        rifle_action,
        beam_action,
        reload_action,
        instant_fire_action,
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 3;
    catalog.catalog_hash = 0x1122334455667788ull;
    catalog.actor_templates = &actor_template;
    catalog.actor_template_count = 1;
    catalog.projectile_templates = projectile_templates.data();
    catalog.projectile_template_count =
        static_cast<std::uint32_t>(projectile_templates.size());
    catalog.collider_templates = initial_collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(initial_collider_templates.size());
    catalog.action_templates = action_templates.data();
    catalog.action_template_count =
        static_cast<std::uint32_t>(action_templates.size());
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
    KernelActionTemplateDefinition queried_action{};
    queried_action.struct_size = sizeof(queried_action);
    require(Kernel_GetActionTemplate(kernel, 1001, &queried_action));
    require(queried_action.trigger_mode == KernelActionTriggerMode_Press);
    require(queried_action.commit_offset_ticks == 3);
    require(queried_action.commit_interval_ticks == 30);
    queried_action = KernelActionTemplateDefinition{};
    queried_action.struct_size = sizeof(queried_action);
    require(Kernel_GetActionTemplate(kernel, 1002, &queried_action));
    require(queried_action.trigger_mode == KernelActionTriggerMode_Hold);
    require(queried_action.commit_interval_ticks == 3);
    require(queried_action.hold_input_timeout_ticks == 6);
    queried_action = KernelActionTemplateDefinition{};
    queried_action.struct_size = sizeof(queried_action);
    require(Kernel_GetActionTemplate(kernel, 1003, &queried_action));
    require(queried_action.commit_offset_ticks == 5);
    require(queried_action.commit_interval_ticks == 1);
    require(queried_action.recovery_ticks == 8);
    require(!Kernel_GetActionTemplate(kernel, 0, &queried_action));
    require(!Kernel_GetActionTemplate(kernel, 9999, &queried_action));
    require(!Kernel_GetActionTemplate(kernel, 1001, nullptr));
    queried_action.struct_size = sizeof(queried_action) - 1;
    require(!Kernel_GetActionTemplate(kernel, 1001, &queried_action));

    KernelGameplayCatalogDefinition null_action_catalog = catalog;
    null_action_catalog.action_templates = nullptr;
    require(!Kernel_LoadGameplayCatalog(kernel, &null_action_catalog, nullptr));
    const auto rejects_action_templates =
        [&](std::array<KernelActionTemplateDefinition, 5> definitions) {
            KernelGameplayCatalogDefinition rejected_catalog = catalog;
            rejected_catalog.action_templates = definitions.data();
            return !Kernel_LoadGameplayCatalog(
                kernel,
                &rejected_catalog,
                nullptr);
        };
    auto invalid_actions = action_templates;
    invalid_actions[0].struct_size = sizeof(KernelActionTemplateDefinition) - 1;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[0].action_template_id = 0;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[1].action_template_id = invalid_actions[0].action_template_id;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[0].trigger_mode = 2;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[0].flags = 0x80;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[0].max_commit_count = 0;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[0].hold_input_timeout_ticks = 1;
    require(rejects_action_templates(invalid_actions));
    invalid_actions = action_templates;
    invalid_actions[1].hold_input_timeout_ticks = 0;
    require(rejects_action_templates(invalid_actions));
    require(Kernel_GetActorTemplates(kernel, nullptr, 0) == 1);
    require(Kernel_GetProjectileTemplates(kernel, nullptr, 0) == 4);
    require(Kernel_GetColliderTemplates(kernel, nullptr, 0) == 2);
    require(Kernel_GetColliderBindings(kernel, nullptr, 0) == 0);
    std::array<KernelActorTemplateDefinition, 1> read_actor_templates{};
    std::array<KernelProjectileTemplateDefinition, 4> read_projectile_templates{};
    std::array<KernelColliderTemplateDefinition, 2> read_collider_templates{};
    require(Kernel_GetActorTemplates(
               kernel,
               read_actor_templates.data(),
               static_cast<std::uint32_t>(read_actor_templates.size())) == 1);
    require(Kernel_GetProjectileTemplates(
               kernel,
               read_projectile_templates.data(),
               static_cast<std::uint32_t>(read_projectile_templates.size())) == 4);
    require(Kernel_GetColliderTemplates(
               kernel,
               read_collider_templates.data(),
               static_cast<std::uint32_t>(read_collider_templates.size())) == 2);
    std::array<KernelColliderBindingDefinition, 1> read_collider_bindings{};
    require(Kernel_GetColliderBindings(
               kernel,
               read_collider_bindings.data(),
               static_cast<std::uint32_t>(read_collider_bindings.size())) == 0);
    require(read_actor_templates[0].actor_template_id == 2);
    require(read_actor_templates[0].collider_template_id == 10);
    require(read_actor_templates[0].vision.vision_collider_template_id == 12);
    require(read_actor_templates[0].vision.local_origin.y == 1.5f);
    require(read_projectile_templates[0].projectile_template_id == 3);
    require(read_projectile_templates[0].mechanics.collider_template_id == 10);
    require(read_projectile_templates[1].mechanics.projectile_type ==
           KernelProjectileType_AreaEffect);
    require(read_projectile_templates[2].mechanics.projectile_type ==
           KernelProjectileType_Beam);
    require(read_projectile_templates[3].mechanics.motion_model ==
           KernelProjectileMotionModel_Homing);
    require(read_collider_templates[0].template_id == 10);
    require(read_collider_templates[0].shape_params.x == 0.25f);
    require(read_collider_templates[1].template_id == 12);
    require(read_collider_templates[1].shape_type == KernelColliderShapeType_Cone);
    require(read_collider_templates[1].shape_params.x == 10.0f);
    require(read_collider_templates[1].shape_params.y == 90.0f);
    KernelColliderBindingDefinition rejected_binding{};
    rejected_binding.struct_size = sizeof(rejected_binding);
    rejected_binding.entity_type = 1;
    rejected_binding.collider_template_id = 10;
    KernelGameplayCatalogDefinition rejected_binding_catalog = catalog;
    rejected_binding_catalog.collider_bindings = &rejected_binding;
    rejected_binding_catalog.collider_binding_count = 1;
    require(!Kernel_LoadGameplayCatalog(
        kernel,
        &rejected_binding_catalog,
        nullptr));
    benchmark_stats = KernelBenchmarkStats{};
    benchmark_stats.struct_size = sizeof(benchmark_stats);
    require(Kernel_GetBenchmarkStats(kernel, &benchmark_stats));
    require(benchmark_stats.catalog_version == 3);
    require(benchmark_stats.catalog_hash == 0x1122334455667788ull);
    require(Kernel_GetNetworkStats(kernel, &network_stats));
    require(Kernel_PollDebugRecords(
               kernel,
               &debug_filter,
               debug_records.data(),
               static_cast<std::uint32_t>(debug_records.size())) == 0);
    require(Kernel_GetLocalPlayerInfo(kernel, &local_info));
    require(local_info.peer_id == 0);
    require(local_info.player_net_id == 0);
    require(local_info.has_welcome == 0u);
    require(local_info.connected == 0u);
    require(!Kernel_GetLocalPlayerInfo(kernel, nullptr));
    require(!Kernel_StartClient(kernel, nullptr));
    require(!Kernel_StartClient(kernel, ""));
    KernelGameplayCatalogSyncClientConfig sync_client_config{};
    sync_client_config.struct_size = sizeof(sync_client_config);
    require(!Kernel_StartClientCatalogSync(kernel, nullptr, &sync_client_config));
    require(!Kernel_StartClientCatalogSync(kernel, "", &sync_client_config));
    require(!Kernel_StartClientCatalogSync(kernel, "127.0.0.1:7777", nullptr));
    KernelGameplayCatalogSyncStatus sync_status{};
    sync_status.struct_size = sizeof(sync_status);
    require(Kernel_GetGameplayCatalogSyncStatus(kernel, &sync_status));
    require(sync_status.state == KernelGameplayCatalogSyncState_Idle);
    require(!Kernel_GetGameplayCatalogSyncStatus(kernel, nullptr));
    require(!Kernel_RequestGameplayCatalogBundle(kernel));
    require(!Kernel_CopyGameplayCatalogBundle(kernel, nullptr, 0, nullptr));
    require(!Kernel_ContinueClientHandshake(kernel));

    const std::array<std::uint8_t, 4> sync_bundle = {1, 2, 3, 4};
    KernelGameplayCatalogSyncServerConfig sync_server_config{};
    sync_server_config.struct_size = sizeof(sync_server_config);
    sync_server_config.bundle_bytes = sync_bundle.data();
    sync_server_config.bundle_size =
        static_cast<std::uint32_t>(sync_bundle.size());
    sync_server_config.entry_path = "gameplay_catalog.yaml";
    sync_server_config.content_namespace = nullptr;
    KernelGameplayCatalogManifest manifest{};
    manifest.struct_size = sizeof(manifest);
    require(Kernel_SetGameplayCatalogSyncBundle(
        kernel,
        &sync_server_config,
        &manifest));
    require(manifest.bundle_size == sync_bundle.size());
    require(std::strcmp(manifest.entry_path, "gameplay_catalog.yaml") == 0);
    require(std::strcmp(manifest.content_namespace, "default") == 0);
    require(manifest.catalog_version == 3);
    require(manifest.catalog_hash == 0x1122334455667788ull);
    KernelGameplayCatalogSyncServerConfig invalid_sync_server_config =
        sync_server_config;
    invalid_sync_server_config.entry_path = "../gameplay_catalog.yaml";
    manifest.struct_size = sizeof(manifest);
    require(!Kernel_SetGameplayCatalogSyncBundle(
        kernel,
        &invalid_sync_server_config,
        &manifest));
    invalid_sync_server_config = sync_server_config;
    invalid_sync_server_config.content_namespace = "../production";
    manifest.struct_size = sizeof(manifest);
    require(!Kernel_SetGameplayCatalogSyncBundle(
        kernel,
        &invalid_sync_server_config,
        &manifest));
    require(!Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    require(Kernel_StartDedicatedServer(kernel, 8043));

    require(Kernel_ServerCreateEntity(kernel, &create_info, &created_net_id));
    require(created_net_id != 0);
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    require(server_state.hp == 0);
    require(server_state.max_hp == 0);

    KernelCombatStateDefinition sparse_weapon_state{};
    sparse_weapon_state.struct_size = sizeof(sparse_weapon_state);
    sparse_weapon_state.hp = 240;
    sparse_weapon_state.max_hp = 240;
    sparse_weapon_state.active_weapon_slot = 0;
    sparse_weapon_state.weapon_slot_count = KERNEL_MAX_WEAPON_SLOTS;
    sparse_weapon_state.weapon_ids[0] = 99;
    sparse_weapon_state.weapon_ids[1] = 255;
    sparse_weapon_state.weapon_ids[2] = 1;
    sparse_weapon_state.weapon_ids[3] = 7;
    sparse_weapon_state.collider_template_id = 10;
    sparse_weapon_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    sparse_weapon_state.hitbox_half_extents =
        KernelVec3{0.4f, 0.8f, 0.4f};
    require(Kernel_ServerSetEntityCombatState(
        kernel, created_net_id, &sparse_weapon_state));
    sparse_weapon_state.weapon_slot_count =
        KERNEL_MAX_WEAPON_SLOTS + 1u;
    require(!Kernel_ServerSetEntityCombatState(
        kernel, created_net_id, &sparse_weapon_state));

    combat_state.hp = 240;
    combat_state.max_hp = 240;
    combat_state.active_weapon_slot = 0;
    combat_state.weapon_slot_count = 4;
    combat_state.weapon_ids[0] = 3;
    combat_state.weapon_ids[1] = 4;
    combat_state.weapon_ids[2] = 5;
    combat_state.weapon_ids[3] = 6;
    combat_state.collider_template_id = 10;
    combat_state.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat_state.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat_state.ammo[0] = 3;
    combat_state.reserve_magazines[0] = 6;
    combat_state.ammo[1] = 1;
    combat_state.reserve_magazines[1] = 1;
    combat_state.ammo[2] = 2;
    combat_state.reserve_magazines[2] = 2;
    combat_state.ammo[3] = 2;
    combat_state.reserve_magazines[3] = 2;
    require(!Kernel_ServerSetEntityCombatState(kernel, created_net_id, nullptr));
    require(Kernel_ServerSetEntityCombatState(kernel, created_net_id, &combat_state));
    vision_config = KernelAgentVisionConfig{};
    vision_config.struct_size = sizeof(vision_config);
    vision_config.camp = KernelAgentCamp_EnemySide;
    vision_config.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    vision_config.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    vision_config.max_visible_neutrals = KERNEL_MAX_VISIBLE_NEUTRALS;
    vision_config.vision_collider_template_id = 12;
    require(Kernel_ServerSetEntityVisionConfig(kernel, created_net_id, &vision_config));

    KernelServerEntityCreateInfo player_create_info = create_info;
    player_create_info.entity_type = 1;
    player_create_info.actor_type = KernelActorType_Player;
    player_create_info.position = KernelVec3{5.0f, 0.0f, 0.0f};
    std::uint32_t visible_player_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player_create_info, &visible_player_net_id));
    KernelAgentVisionConfig player_vision_config{};
    player_vision_config.struct_size = sizeof(player_vision_config);
    player_vision_config.camp = KernelAgentCamp_PlayerSide;
    require(Kernel_ServerSetEntityVisionConfig(
        kernel,
        visible_player_net_id,
        &player_vision_config));
    Kernel_Update(kernel, 1.0f / 30.0f);

    require(Kernel_QueryVisionState(kernel, nullptr, vision_states.data(), 1) == 1);
    require(Kernel_QueryVisionState(kernel, &vision_query, nullptr, 1) == 0);
    require(Kernel_QueryVisionState(kernel, &vision_query, vision_states.data(), 0) == 0);
    require(Kernel_QueryVisionState(
               kernel,
               &vision_query,
               vision_states.data(),
               static_cast<std::uint32_t>(vision_states.size())) == 1);
    require(vision_states[0].valid != 0u);
    require(vision_states[0].agent_net_id == created_net_id);
    require(vision_states[0].entity_type == 1);
    require(vision_states[0].actor_type == KernelActorType_Agent);
    require(vision_states[0].camp == KernelAgentCamp_EnemySide);
    require(vision_states[0].vision_collider_template_id == 12);
    require(vision_states[0].resolved_collider_template_id == 10);
    require(vision_states[0].visible_hostile_count == 1);
    require(vision_states[0].visible_hostiles[0] == visible_player_net_id);
    require(vision_states[0].visible_ally_count == 0);
    require(vision_states[0].visible_neutral_count == 0);
    require(vision_states[0].current_target_candidate == visible_player_net_id);
    require(vision_states[0].last_seen_target == visible_player_net_id);
    require(vision_states[0].last_known_target_position.x == 5.0f);
    require(vision_states[0].relation_to_current_target == KernelAgentRelation_Hostile);

    KernelVec3 behind_position{-5.0f, 0.0f, 0.0f};
    KernelQuat player_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel,
        visible_player_net_id,
        &behind_position,
        &player_rotation));
    Kernel_Update(kernel, 1.0f / 30.0f);
    require(Kernel_QueryVisionState(kernel, &vision_query, vision_states.data(), 1) == 1);
    require(vision_states[0].visible_hostile_count == 0);
    require(vision_states[0].current_target_candidate == 0);
    require(vision_states[0].last_seen_target == visible_player_net_id);
    require(vision_states[0].last_known_target_position.x == 5.0f);
    require(vision_states[0].time_since_last_seen_target > 0.0f);

    KernelVec3 outside_range_position{15.0f, 0.0f, 0.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel,
        visible_player_net_id,
        &outside_range_position,
        &player_rotation));
    Kernel_Update(kernel, 1.0f / 30.0f);
    require(Kernel_QueryVisionState(kernel, &vision_query, vision_states.data(), 1) == 1);
    require(vision_states[0].visible_hostile_count == 0);

    KernelVec3 neutral_position{3.0f, 0.0f, 0.0f};
    player_create_info.position = neutral_position;
    std::uint32_t visible_neutral_net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &player_create_info, &visible_neutral_net_id));
    KernelAgentVisionConfig neutral_vision_config{};
    neutral_vision_config.struct_size = sizeof(neutral_vision_config);
    neutral_vision_config.camp = KernelAgentCamp_Neutral;
    require(Kernel_ServerSetEntityVisionConfig(
        kernel,
        visible_neutral_net_id,
        &neutral_vision_config));
    Kernel_Update(kernel, 1.0f / 30.0f);
    require(Kernel_QueryVisionState(kernel, &vision_query, vision_states.data(), 1) == 1);
    require(vision_states[0].visible_hostile_count == 0);
    require(vision_states[0].visible_neutral_count == 1);
    require(vision_states[0].visible_neutrals[0] == visible_neutral_net_id);
    require(vision_states[0].current_target_candidate == 0);

    require(Kernel_ServerClearEntityVisionConfig(kernel, visible_player_net_id));
    require(Kernel_ServerDestroyEntity(
        kernel,
        visible_player_net_id,
        KernelDespawnReason_Destroyed));
    require(Kernel_ServerDestroyEntity(
        kernel,
        visible_neutral_net_id,
        KernelDespawnReason_Destroyed));
    std::array<KernelEntityLifecycleEvent, 4> drained_lifecycle_events{};
    Kernel_PollEntityLifecycleEvents(
        kernel,
        drained_lifecycle_events.data(),
        static_cast<std::uint32_t>(drained_lifecycle_events.size()));
    const std::uint32_t collider_count = Kernel_QueryColliderShapes(
        kernel,
        &collider_query,
        collider_shapes.data(),
        static_cast<std::uint32_t>(collider_shapes.size()));
    require(collider_count == 1);
    require(collider_shapes[0].entity_net_id == created_net_id);
    require(collider_shapes[0].entity_type == 1);
    require(collider_shapes[0].actor_type == KernelActorType_Agent);
    require(collider_shapes[0].collider_template_id == 10);
    require(collider_shapes[0].shape_type == KernelColliderShapeType_Aabb);
    require(collider_shapes[0].collider_id != 0);
    require(collider_shapes[0].owner_net_id == created_net_id);
    require(collider_shapes[0].world_center.y == 0.8f);
    require(collider_shapes[0].shape_params.x == 0.25f);
    require(collider_shapes[0].remaining_ticks == 0);
    require(Kernel_QueryColliderShapes(
               kernel,
               nullptr,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 1);
    KernelColliderShapeQuery query_all{};
    query_all.struct_size = sizeof(query_all);
    query_all.entity_net_id = 0;
    query_all.purpose_mask = 0;
    require(Kernel_QueryColliderShapes(
               kernel,
               &query_all,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 1);

    KernelColliderTemplateDefinition changed_collider_template{};
    changed_collider_template.struct_size = sizeof(changed_collider_template);
    changed_collider_template.template_id = 11;
    changed_collider_template.shape_type = KernelColliderShapeType_Sphere;
    changed_collider_template.shape_params = KernelVec4{3.0f, 0.0f, 0.0f, 0.0f};
    changed_collider_template.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    changed_collider_template.purpose_flags = KernelColliderPurpose_Damage;
    std::array<KernelColliderTemplateDefinition, 2> changed_collider_templates = {
        collider_template,
        changed_collider_template,
    };
    KernelGameplayCatalogDefinition changed_catalog{};
    changed_catalog.struct_size = sizeof(changed_catalog);
    changed_catalog.catalog_version = 4;
    changed_catalog.catalog_hash = 0x8877665544332211ull;
    changed_catalog.projectile_templates = projectile_templates.data();
    changed_catalog.projectile_template_count =
        static_cast<std::uint32_t>(projectile_templates.size());
    changed_catalog.collider_templates = changed_collider_templates.data();
    changed_catalog.collider_template_count =
        static_cast<std::uint32_t>(changed_collider_templates.size());
    changed_catalog.action_templates = action_templates.data();
    changed_catalog.action_template_count =
        static_cast<std::uint32_t>(action_templates.size());
    require(Kernel_LoadGameplayCatalog(kernel, &changed_catalog, nullptr));
    for (KernelColliderShapeView& shape : collider_shapes) {
        shape = KernelColliderShapeView{};
        shape.struct_size = sizeof(KernelColliderShapeView);
    }
    require(Kernel_QueryColliderShapes(
               kernel,
               &collider_query,
               collider_shapes.data(),
               static_cast<std::uint32_t>(collider_shapes.size())) == 1);
    require(collider_shapes[0].collider_template_id == 10);
    require(collider_shapes[0].shape_type == KernelColliderShapeType_Aabb);
    require(collider_shapes[0].shape_params.x == 0.25f);

    weapon_mechanics.weapon_id = 3;
    weapon_mechanics.fire_mode = KernelWeaponFireMode_Projectile;
    weapon_mechanics.magazine_size = 3;
    weapon_mechanics.reserve_magazines = 6;
    weapon_mechanics.damage = 5;
    weapon_mechanics.projectile_template_id = 3;
    weapon_mechanics.fire_action_template_id = 1005;
    weapon_mechanics.reload_action_template_id = 1004;
    require(Kernel_ServerValidateMechanicsConfig(&weapon_mechanics));
    require(!Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, nullptr));
    require(Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &weapon_mechanics));
    KernelWeaponMechanicsDefinition queried_weapon{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        3,
        &queried_weapon));
    require(queried_weapon.weapon_id == 3);
    require(queried_weapon.reserve_magazines == 6);
    require(queried_weapon.projectile_template_id == 3);
    require(queried_weapon.fire_action_template_id == 1005);
    KernelWeaponMechanicsDefinition missing_action_template = weapon_mechanics;
    missing_action_template.fire_action_template_id = 9999;
    require(Kernel_ServerValidateMechanicsConfig(&missing_action_template));
    require(!Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &missing_action_template));
    KernelGameplayCatalogDefinition dangling_action_catalog = changed_catalog;
    dangling_action_catalog.action_templates = nullptr;
    dangling_action_catalog.action_template_count = 0;
    require(!Kernel_LoadGameplayCatalog(
        kernel,
        &dangling_action_catalog,
        nullptr));
    KernelWeaponMechanicsDefinition missing_projectile_template = weapon_mechanics;
    missing_projectile_template.projectile_template_id = 0;
    require(!Kernel_ServerValidateMechanicsConfig(&missing_projectile_template));

    KernelWeaponMechanicsDefinition homing_weapon = weapon_mechanics;
    homing_weapon.weapon_id = 6;
    homing_weapon.projectile_template_id = 6;
    require(Kernel_ServerValidateMechanicsConfig(&homing_weapon));
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &homing_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        6,
        &queried_weapon));
    require(queried_weapon.projectile_template_id == 6);

    KernelWeaponMechanicsDefinition invalid_weapon = weapon_mechanics;
    invalid_weapon.struct_size = sizeof(invalid_weapon) - 1;
    require(!Kernel_ServerValidateMechanicsConfig(&invalid_weapon));
    require(!Kernel_ServerSetEntityWeaponMechanics(
        kernel,
        created_net_id,
        &invalid_weapon));

    KernelWeaponMechanicsDefinition area_weapon{};
    area_weapon.struct_size = sizeof(area_weapon);
    area_weapon.weapon_id = 4;
    area_weapon.fire_mode = KernelWeaponFireMode_Projectile;
    area_weapon.magazine_size = 2;
    area_weapon.damage = 7;
    area_weapon.projectile_template_id = 4;
    area_weapon.fire_action_template_id = 1005;
    area_weapon.reload_action_template_id = 1004;
    require(Kernel_ServerValidateMechanicsConfig(&area_weapon));
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &area_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        4,
        &queried_weapon));
    require(queried_weapon.fire_mode == KernelWeaponFireMode_Projectile);
    require(queried_weapon.projectile_template_id == 4);

    KernelWeaponMechanicsDefinition beam_weapon{};
    beam_weapon.struct_size = sizeof(beam_weapon);
    beam_weapon.weapon_id = 5;
    beam_weapon.fire_mode = KernelWeaponFireMode_Projectile;
    beam_weapon.magazine_size = 2;
    beam_weapon.damage = 30;
    beam_weapon.projectile_template_id = 5;
    beam_weapon.fire_action_template_id = 1005;
    beam_weapon.reload_action_template_id = 1004;
    require(Kernel_ServerValidateMechanicsConfig(&beam_weapon));
    require(Kernel_ServerSetEntityWeaponMechanics(kernel, created_net_id, &beam_weapon));
    queried_weapon = KernelWeaponMechanicsDefinition{};
    queried_weapon.struct_size = sizeof(queried_weapon);
    require(Kernel_ServerGetEntityWeaponMechanics(
        kernel,
        created_net_id,
        5,
        &queried_weapon));
    require(queried_weapon.fire_mode == KernelWeaponFireMode_Projectile);
    require(queried_weapon.projectile_template_id == 5);

    KernelVec3 enemy_position{5.0f, 0.0f, 0.0f};
    KernelQuat enemy_rotation{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerSetEntityTransform(
        kernel,
        created_net_id,
        &enemy_position,
        &enemy_rotation));
    KernelVec3 enemy_velocity{1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSetEntityVelocity(kernel, created_net_id, &enemy_velocity));
    require(Kernel_ServerSetEntityState(kernel, created_net_id, 7, 0x12345678u));
    server_entity_input.buttons = 0;
    server_entity_input.selected_weapon = 3;
    server_entity_input.aim_dir = KernelVec3{-1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    require(server_state.valid != 0u);
    require(server_state.net_id == created_net_id);
    require(server_state.entity_type == 1);
    require(server_state.actor_type == KernelActorType_Agent);
    require(server_state.animation_state == 7);
    require((server_state.visual_flags & KERNEL_VISUAL_FLAG_MOVING) != 0u);
    require(server_state.position.x == 5.0f);
    require(server_state.velocity.x == 1.0f);
    require(server_state.hp == 240);
    require(server_state.max_hp == 240);
    require(server_state.active_weapon_slot == 0);
    require(server_state.weapon_ids[0] == 3);
    require(server_state.ammo[0] == 3);
    require(server_state.reserve_magazines[0] == 6);
    require(server_state.is_reloading == 0u);
    require(server_state.reload_remaining_ticks == 0u);
    std::array<KernelServerEntityState, 4> queried_states{};
    for (KernelServerEntityState& queried_state : queried_states) {
        queried_state.struct_size = sizeof(KernelServerEntityState);
    }
    require(Kernel_ServerQueryEntities(
               kernel,
               1,
               queried_states.data(),
               static_cast<std::uint32_t>(queried_states.size())) == 1);
    require(queried_states[0].net_id == created_net_id);
    require(queried_states[0].actor_type == KernelActorType_Agent);
    require(queried_states[0].hp == 240);
    require(queried_states[0].max_hp == 240);
    require(queried_states[0].active_weapon_slot == 0);
    require(queried_states[0].weapon_ids[0] == 3);
    require(queried_states[0].ammo[0] == 3);
    require(queried_states[0].reserve_magazines[0] == 6);
    require(Kernel_ServerQueryEntities(
               kernel,
               0,
               queried_states.data(),
               static_cast<std::uint32_t>(queried_states.size())) == 1);

    server_entity_input.input_seq = 2;
    server_entity_input.action_intent = KernelActionIntent{
        1u, KernelActionBinding_PrimaryFire, 0u, 0u};
    server_entity_input.selected_weapon = 3;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    for (int tick = 0; tick < 4; ++tick) {
        Kernel_Update(kernel, 1.0f / 30.0f);
    }
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    require(server_state.active_weapon_slot == 0);
    require(server_state.ammo[0] == 2);
    require(server_state.reserve_magazines[0] == 6);

    server_entity_input.input_seq = 3;
    server_entity_input.action_intent = KernelActionIntent{
        2u, KernelActionBinding_Reload, 0u, 0u};
    server_entity_input.selected_weapon = 3;
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    require(server_state.is_reloading != 0u);
    require(server_state.reload_remaining_ticks > 0u);

    for (int tick = 0; tick < 30; ++tick) {
        Kernel_Update(kernel, 1.0f / 30.0f);
    }
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    require(server_state.is_reloading == 0u);
    require(server_state.reload_remaining_ticks == 0u);
    require(server_state.ammo[0] == 3);
    require(server_state.reserve_magazines[0] == 5);

    KernelPlayerInput input{};
    input.input_seq = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    Kernel_SubmitPlayerInput(kernel, 1, &input);
    Kernel_Update(kernel, 1.0f / 30.0f);
    server_state = KernelServerEntityState{};
    server_state.struct_size = sizeof(server_state);
    require(Kernel_ServerGetEntityState(kernel, created_net_id, &server_state));
    // Template-less actors use the explicit kNone movement policy and do not
    // integrate authored velocity automatically.
    require(server_state.position.x == 5.0f);
    require(server_state.hp == 240);
    require(server_state.max_hp == 240);

    std::array<RenderEntityState, 8> states{};
    require(Kernel_GetRenderStates(kernel, nullptr, states.size()) == 0);
    require(Kernel_GetRenderStates(kernel, states.data(), 0) == 0);
    require(Kernel_GetRenderStatesAtTime(kernel, 0, nullptr, states.size()) == 0);
    require(Kernel_GetRenderStatesAtTime(kernel, 0, states.data(), 0) == 0);
    const std::uint32_t render_count =
        Kernel_GetRenderStates(kernel, states.data(), states.size());
    require(render_count >= 1);
    KernelSkeletonRenderStateResult skeleton_result{};
    skeleton_result.struct_size = sizeof(skeleton_result);
    require(Kernel_GetSkeletonRenderStates(
               kernel, nullptr, 0u, nullptr, 0u, &skeleton_result) == 0u);
    require(skeleton_result.status == KERNEL_SKELETON_RENDER_STATUS_SUCCESS);
    require(skeleton_result.required_state_count == 0u);
    skeleton_result.struct_size = sizeof(skeleton_result);
    require(Kernel_GetSkeletonRenderStatesAtTime(
               kernel,
               33333u,
               nullptr,
               0u,
               nullptr,
               0u,
               &skeleton_result) == 0u);
    require(skeleton_result.flags == KERNEL_SKELETON_RENDER_RESULT_FLAG_AT_TIME);
    const RenderEntityState* rendered_actor = nullptr;
    for (std::uint32_t index = 0; index < render_count; ++index) {
        if (states[index].net_id == created_net_id) {
            rendered_actor = &states[index];
            break;
        }
    }
    require(rendered_actor != nullptr);
    require(rendered_actor->entity_id != 0);
    require(rendered_actor->owner_peer == 0);
    require(rendered_actor->position.x == 5.0f);
    require(rendered_actor->velocity.x == 1.0f);
    require(rendered_actor->hp == 240);
    require(rendered_actor->max_hp == 240);
    require(rendered_actor->animation_state == 7);
    require(
        (rendered_actor->visual_flags & KERNEL_VISUAL_FLAG_MOVING) != 0u);
    require(rendered_actor->spawn_tick == 0);
    require(rendered_actor->action_instance_id == 0);
    const std::uint32_t render_at_time_count =
        Kernel_GetRenderStatesAtTime(kernel, 33333, states.data(), states.size());
    require(render_at_time_count >= 1);
    rendered_actor = nullptr;
    for (std::uint32_t index = 0; index < render_at_time_count; ++index) {
        if (states[index].net_id == created_net_id) {
            rendered_actor = &states[index];
            break;
        }
    }
    require(rendered_actor != nullptr);
    require(rendered_actor->hp == 240);
    require(rendered_actor->max_hp == 240);

    server_entity_input.action_intent = KernelActionIntent{
        3u, KernelActionBinding_PrimaryFire, 0u, 0u};
    server_entity_input.selected_weapon = 4;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> area_events{};
    const std::uint32_t area_event_count =
        Kernel_PollEvents(kernel, area_events.data(), area_events.size());
    std::uint32_t area_net_id = 0;
    for (std::uint32_t index = 0; index < area_event_count; ++index) {
        if (area_events[index].type == KernelEventType_EntitySpawned &&
            area_events[index].code == 3u) {
            area_net_id = area_events[index].net_id;
        }
    }
    require(area_net_id != 0);

    server_entity_input.action_intent = KernelActionIntent{
        4u, KernelActionBinding_PrimaryFire, 0u, 0u};
    server_entity_input.selected_weapon = 5;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> beam_events{};
    const std::uint32_t beam_event_count =
        Kernel_PollEvents(kernel, beam_events.data(), beam_events.size());
    std::uint32_t beam_net_id = 0;
    for (std::uint32_t index = 0; index < beam_event_count; ++index) {
        if (beam_events[index].type == KernelEventType_EntitySpawned &&
            beam_events[index].code == 3u) {
            beam_net_id = beam_events[index].net_id;
        }
    }
    require(beam_net_id != 0);

    server_entity_input.action_intent = KernelActionIntent{
        5u, KernelActionBinding_PrimaryFire, 0u, 0u};
    server_entity_input.selected_weapon = 6;
    server_entity_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    require(Kernel_ServerSubmitEntityInput(
        kernel,
        created_net_id,
        &server_entity_input));
    Kernel_Update(kernel, 1.0f / 30.0f);
    std::array<KernelEvent, 16> homing_events{};
    const std::uint32_t homing_event_count =
        Kernel_PollEvents(kernel, homing_events.data(), homing_events.size());
    std::uint32_t homing_net_id = 0;
    for (std::uint32_t index = 0; index < homing_event_count; ++index) {
        if (homing_events[index].type == KernelEventType_EntitySpawned &&
            homing_events[index].code == 3u) {
            homing_net_id = homing_events[index].net_id;
        }
    }
    require(homing_net_id != 0);
    homing_state = KernelHomingState{};
    homing_state.struct_size = sizeof(homing_state);
    require(Kernel_ServerGetHomingState(kernel, homing_net_id, &homing_state));
    require(homing_state.valid != 0u);
    require(homing_state.shooter_net_id == created_net_id);
    require(homing_state.guidance_phase <= KernelMissileGuidancePhase_LostTarget);
    require(homing_state.lock_on_range == 25.0f);
    require(homing_state.max_speed == 40.0f);

    std::array<KernelEvent, 16> events{};
    require(Kernel_PollEvents(kernel, nullptr, events.size()) == 0);
    require(Kernel_PollEvents(kernel, events.data(), 0) == 0);
    Kernel_PollEvents(kernel, events.data(), events.size());
    require(Kernel_ServerDestroyEntity(
        kernel,
        created_net_id,
        KernelDespawnReason_Destroyed));
    std::array<KernelEntityLifecycleEvent, 4> lifecycle_events{};
    const std::uint32_t lifecycle_count =
        Kernel_PollEntityLifecycleEvents(
            kernel,
            lifecycle_events.data(),
            static_cast<std::uint32_t>(lifecycle_events.size()));
    require(lifecycle_count == 1);
    require(lifecycle_events[0].type == KernelEntityLifecycleEventType_Destroyed);
    require(lifecycle_events[0].net_id == created_net_id);
    require(lifecycle_events[0].reason == KernelDespawnReason_Destroyed);

    require(!Kernel_ServerClearEntityWeaponMechanics(kernel, created_net_id, 3));

    Kernel_Destroy(kernel);
    server_set_entity_health_updates_hp_only();
    return 0;
}
