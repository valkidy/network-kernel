#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/public/kernel_api.h"

_Static_assert(
    KERNEL_ABI_VERSION == 87u,
    "server entity state reports the entity template it came from, which is "
    "the only way to ask what a prop is");
_Static_assert(
    offsetof(KernelWeaponMechanicsDefinition, melee_collider_template_id) >
        offsetof(KernelWeaponMechanicsDefinition, collision_mask),
    "the melee collider template is appended");
_Static_assert(
    offsetof(KernelAreaEffectMechanicsDefinition, motion_collision_mask) >
        offsetof(KernelAreaEffectMechanicsDefinition, collision_mask),
    "the area effect motion mask is appended");
_Static_assert(
    offsetof(KernelAreaEffectMechanicsDefinition, hit_instigator) >
        offsetof(KernelAreaEffectMechanicsDefinition, collision_mask),
    "the area effect instigator switch is appended");
_Static_assert(
    offsetof(KernelWeaponMechanicsDefinition, collision_mask) >
        offsetof(KernelWeaponMechanicsDefinition, reload_action_template_id),
    "the weapon collision mask is appended");
_Static_assert(
    KERNEL_HIT_ZONE_UNSCALED != 0u,
    "the neutral multiplier must not be zero, or unauthored volumes are immune");
_Static_assert(
    (KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_LIMB) == 0u,
    "limbs stay out of the damageable aggregate");
_Static_assert(
    (KERNEL_MOVEMENT_MASK_DEFAULT & KERNEL_MOVEMENT_LAYER_LIMB) == 0u,
    "limbs stay out of what zero selects");
_Static_assert(
    (KERNEL_MOVEMENT_MASK_SUPPORTED & KERNEL_MOVEMENT_LAYER_LIMB) != 0u,
    "limbs are authorable");
_Static_assert(
    offsetof(RenderEntityState, beam_end) >
        offsetof(RenderEntityState, carrier_entity_id),
    "beam endpoint is appended");
_Static_assert(
    offsetof(KernelStatusEffectDefinition, max_stacks) >
        offsetof(KernelStatusEffectDefinition, on_expire_trigger),
    "status stack authoring fields are appended");
_Static_assert(
    offsetof(KernelStatusEffectView, stack_count) >
        offsetof(KernelStatusEffectView, expire_tick),
    "status stack query fields are appended");
_Static_assert(
    offsetof(KernelRemoteActionPresentationEvent, stack_count) >
        offsetof(KernelRemoteActionPresentationEvent, duration_ticks),
    "status stack presentation fields are appended");
_Static_assert(
    offsetof(KernelSkeletonBindingDefinition, colliders) >
        offsetof(KernelSkeletonBindingDefinition, stance_crouch_meters),
    "limb colliders are appended to the skeleton binding ABI");
_Static_assert(
    offsetof(KernelMovementDefinition, movement_collision_mask) >
        offsetof(KernelMovementDefinition, max_yaw_degrees_per_second),
    "movement collision mask is appended to the movement ABI");
_Static_assert(
    offsetof(KernelSkeletonBindingDefinition, stance_crouch_meters) >
        offsetof(KernelSkeletonBindingDefinition, processing_order),
    "stance crouch is appended to the skeleton binding ABI");
_Static_assert(
    sizeof(KernelActionTriggerDefinition) == 692u,
    "KernelActionTriggerDefinition ABI size");
_Static_assert(
    offsetof(KernelActionDefinition, impulse_strength_vertical) >
        offsetof(KernelActionDefinition, impulse_lockout_ticks),
    "impulse strength split is appended to KernelActionDefinition");
_Static_assert(
    offsetof(KernelActionTriggerDefinition, impulse_strength_vertical) >
        offsetof(KernelActionTriggerDefinition, impulse_lockout_ticks),
    "impulse strength split is appended to KernelActionTriggerDefinition");
_Static_assert(
    offsetof(KernelActionDefinition, impulse_lockout_ticks) >
        offsetof(KernelActionDefinition, modifier_value),
    "impulse_lockout_ticks is appended to KernelActionDefinition");
_Static_assert(
    offsetof(KernelActionTriggerDefinition, impulse_lockout_ticks) >
        offsetof(KernelActionTriggerDefinition, modifier_value),
    "impulse_lockout_ticks is appended to KernelActionTriggerDefinition");
_Static_assert(
    offsetof(KernelActionDefinition, health_change_amount) >
        offsetof(KernelActionDefinition, spawn_item_quantity),
    "health change amount is appended to action ABI");
_Static_assert(
    offsetof(KernelActionDefinition, condition_type) >
        offsetof(KernelActionDefinition, health_change_amount),
    "action condition is appended to action ABI");
_Static_assert(
    offsetof(KernelActionDefinition, impulse_strength) >
        offsetof(KernelActionDefinition, condition_type),
    "impulse strength is appended to action ABI");
_Static_assert(
    offsetof(KernelActionDefinition, impulse_collision_mask) >
        offsetof(KernelActionDefinition, impulse_strength),
    "impulse collision mask is appended to action ABI");
_Static_assert(
    offsetof(KernelActionDefinition, impulse_direction) >
        offsetof(KernelActionDefinition, impulse_collision_mask),
    "impulse direction is appended to action ABI");
_Static_assert(
    offsetof(KernelEntityTemplateDefinition, collision_trigger_mask) >
        offsetof(KernelEntityTemplateDefinition, prop),
    "collision trigger mask follows prop definition");
_Static_assert(
    offsetof(KernelEntityTemplateDefinition, impulse_resistance) >
        offsetof(KernelEntityTemplateDefinition, skeleton),
    "impulse resistance is appended to entity template ABI");
_Static_assert(
    offsetof(KernelEvent, health_delta) >
        offsetof(KernelEvent, presentation_time_us),
    "health delta is appended to event ABI");
_Static_assert(sizeof(KernelActionIntent) == 8u, "KernelActionIntent ABI size");
_Static_assert(sizeof(KernelActionInput) == 8u, "KernelActionInput ABI size");
_Static_assert(
    offsetof(KernelWeaponMechanicsDefinition, reserve_magazines) >
        offsetof(KernelWeaponMechanicsDefinition, magazine_size),
    "weapon mechanics include authored reserve magazine count");
_Static_assert(
    offsetof(KernelWeaponMechanicsDefinition, projectile_template_id) >
        offsetof(KernelWeaponMechanicsDefinition, pellet_spread),
    "weapon mechanics reference projectile templates instead of duplicating mechanics");
_Static_assert(
    offsetof(KernelProjectileTemplateDefinition, mechanics) >
        offsetof(KernelProjectileTemplateDefinition, weapon_id),
    "projectile templates own projectile mechanics");
_Static_assert(
    offsetof(KernelProjectileMechanicsDefinition, projectile_type) >
        offsetof(KernelProjectileMechanicsDefinition, struct_size),
    "projectile mechanics use projectile_type naming");
_Static_assert(
    offsetof(KernelSkeletonLegDefinition, gait_group) >
        offsetof(KernelSkeletonLegDefinition, foot_bone_index),
    "skeleton leg gait group follows the IK bone chain");
_Static_assert(
    offsetof(KernelSkeletonBindingDefinition, step_threshold_meters) >
        offsetof(KernelSkeletonBindingDefinition, input_deadzone),
    "displacement gait fields follow locomotion input settings");

int main(void) {
    KernelAbiInfo abi_info;
    KernelBuildInfo build_info;
    KernelLANDiscoveryServerConfig lan_discovery_server_config;
    KernelLANDiscoveryQueryConfig lan_discovery_query_config;
    KernelLANDiscoveryResult lan_discovery_result;
    KernelConfig config;
    KernelNetworkStatsConfig network_stats_config;
    KernelLocalPlayerInfo local_player_info;
    KernelPlayerInput input;
    RenderEntityState state;
    KernelBoneLocalTransform bone_transform;
    KernelSkeletonRenderState skeleton_state;
    KernelSkeletonRenderStateResult skeleton_result;
    KernelSkeletonAssetDefinition skeleton_asset;
    KernelSkeletonBindingDefinition skeleton_binding;
    KernelSkeletonLegDefinition skeleton_leg;
    KernelEvent event;
    KernelServerEntityCreateInfo create_info;
    KernelServerEntityState server_state;
    KernelCombatStateDefinition combat_state;
    KernelWeaponMechanicsDefinition weapon_mechanics;
    KernelProjectileMechanicsDefinition projectile_mechanics;
    KernelHomingMechanicsDefinition homing_mechanics;
    KernelHomingState homing_state;
    KernelGameplayCatalogDefinition gameplay_catalog;
    KernelGameplayCatalogLoadResult gameplay_catalog_load_result;
    KernelActorTemplateDefinition actor_template;
    KernelEntityAiDefinition entity_ai;
    KernelEntityTemplateDefinition entity_template;
    KernelProjectileTemplateDefinition projectile_template;
    KernelColliderTemplateDefinition collider_template;
    KernelColliderBindingDefinition collider_binding;
    KernelBenchmarkStats benchmark_stats;
    KernelNetworkStats network_stats;
    KernelDebugRecordFilter debug_filter;
    KernelDebugInfo debug_info;
    KernelColliderShapeQuery collider_query;
    KernelColliderShapeView collider_shape;
    KernelVec4 shape_params;
    KernelAgentVisionConfig agent_vision_config;
    KernelVisionStateQuery vision_query;
    KernelVisionStateView vision_state;

    (void)config;
    (void)network_stats_config;
    (void)build_info;
    (void)lan_discovery_server_config;
    (void)lan_discovery_query_config;
    (void)lan_discovery_result;
    (void)local_player_info;
    (void)input;
    (void)state;
    (void)bone_transform;
    (void)skeleton_state;
    (void)skeleton_result;
    (void)skeleton_asset;
    (void)skeleton_binding;
    (void)skeleton_leg;
    (void)event;
    (void)create_info;
    (void)server_state;
    (void)combat_state;
    (void)weapon_mechanics;
    (void)projectile_mechanics;
    (void)homing_mechanics;
    (void)homing_state;
    (void)gameplay_catalog;
    (void)gameplay_catalog_load_result;
    (void)actor_template;
    (void)entity_ai;
    (void)entity_template;
    (void)projectile_template;
    (void)collider_template;
    (void)collider_binding;
    (void)benchmark_stats;
    (void)network_stats;
    (void)debug_filter;
    (void)debug_info;
    (void)collider_query;
    (void)collider_shape;
    (void)shape_params;
    (void)agent_vision_config;
    (void)vision_query;
    (void)vision_state;

    assert(KERNEL_ABI_VERSION == 84u);
    assert(KernelEventVec3Source_SubjectDirection == 3);
    assert(sizeof(KernelAreaEffectMechanicsDefinition) >
           offsetof(KernelAreaEffectMechanicsDefinition, hit_instigator));
    assert(KERNEL_HIT_ZONE_UNSCALED == 100u);
    assert(KERNEL_COLLISION_LAYER_LIMB == (1u << 3));
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_LIMB) == 0u);
    assert(KERNEL_MOVEMENT_LAYER_LIMB == (1u << 4));
    assert((KERNEL_MOVEMENT_MASK_DEFAULT & KERNEL_MOVEMENT_LAYER_LIMB) == 0u);
    assert(sizeof(KernelSkeletonColliderDefinition) > 0u);
    assert(KERNEL_MAX_SKELETON_COLLIDERS > 0u);
    assert(KernelColliderPurpose_Limb == (1u << 5));
    assert(sizeof(KernelStatusEffectView) == 32u);
    assert(KERNEL_CAPABILITY_SKELETON_BIND_POSE != 0u);
    assert(sizeof(KernelBoneLocalTransform) > 0u);
    assert(sizeof(KernelSkeletonRenderState) > 0u);
    assert(sizeof(KernelSkeletonRenderStateResult) > 0u);
    assert(sizeof(KernelSkeletonAssetDefinition) > 0u);
    assert(sizeof(KernelSkeletonBindingDefinition) > 0u);
    assert(sizeof(KernelSkeletonLegDefinition) > 0u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_FAILED == 0u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_STATUS_SUCCESS == 1u);
    assert(KERNEL_GAMEPLAY_CATALOG_LOAD_ERROR_UNKNOWN_FIELD == 4u);
    assert(KERNEL_MAX_WEAPON_SLOTS == 4u);
    assert(KERNEL_MAX_VISIBLE_HOSTILES == 16u);
    assert(KERNEL_MAX_VISIBLE_ALLIES == 16u);
    assert(KERNEL_LAN_DISCOVERY_DEFAULT_PORT == 47777u);
    assert(sizeof(KernelAbiInfo) > 0u);
    assert(sizeof(KernelBuildInfo) > 0u);
    assert(sizeof(KernelLANDiscoveryServerConfig) > 0u);
    assert(sizeof(KernelLANDiscoveryQueryConfig) > 0u);
    assert(sizeof(KernelLANDiscoveryResult) > 0u);
    assert(sizeof(KernelLocalPlayerInfo) > 0u);
    assert(sizeof(KernelConfig) > 0u);
    assert(sizeof(KernelPlayerInput) > 0u);
    assert(sizeof(RenderEntityState) > 0u);
    assert(sizeof(KernelEvent) > 0u);
    assert(sizeof(KernelServerEntityCreateInfo) > 0u);
    assert(sizeof(KernelServerEntityState) > 0u);
    assert(sizeof(KernelCombatStateDefinition) > 0u);
    assert(sizeof(KernelWeaponMechanicsDefinition) > 0u);
    assert(sizeof(KernelProjectileMechanicsDefinition) > 0u);
    assert(sizeof(KernelHomingMechanicsDefinition) > 0u);
    assert(sizeof(KernelHomingState) > 0u);
    assert(sizeof(KernelGameplayCatalogDefinition) > 0u);
    assert(sizeof(KernelGameplayCatalogLoadResult) > 0u);
    assert(sizeof(KernelActorTemplateDefinition) > 0u);
    assert(sizeof(KernelEntityAiDefinition) > 0u);
    assert(sizeof(KernelEntityTemplateDefinition) > 0u);
    assert(sizeof(KernelAreaEffectMechanicsDefinition) > 0u);
    assert(sizeof(KernelBeamMechanicsDefinition) > 0u);
    assert(sizeof(KernelProjectileTemplateDefinition) > 0u);
    assert(sizeof(KernelColliderTemplateDefinition) > 0u);
    assert(sizeof(KernelColliderBindingDefinition) > 0u);
    assert(sizeof(KernelBenchmarkStats) > 0u);
    assert(sizeof(KernelNetworkStats) > 0u);
    assert(sizeof(KernelDebugRecordFilter) > 0u);
    assert(sizeof(KernelDebugInfo) > 0u);
    assert(sizeof(KernelColliderShapeQuery) > 0u);
    assert(sizeof(KernelColliderShapeView) > 0u);
    assert(sizeof(KernelVec4) == 16u);
    assert(sizeof(KernelAgentVisionConfig) > 0u);
    assert(sizeof(KernelVisionStateQuery) > 0u);
    assert(sizeof(KernelVisionStateView) > 0u);
    assert(KERNEL_MAX_VISIBLE_NEUTRALS == 16u);
    assert(KernelActorType_Player == 1);
    assert(KernelActorType_Agent == 2);
    assert(KernelEntityType_Actor == 1);
    assert(KernelEntityType_Projectile == 3);
    assert(KernelEntityType_Director == 5);
    assert(KernelAiControllerType_Sentry == 1);
    assert(KernelAiControllerType_Director == 2);
    assert(KernelAiControllerType_Chaser == 3);
    assert((KERNEL_ENTITY_COMPONENT_SERVER_ONLY &
            KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME) == 0u);
    assert(KernelProjectileCollisionQueryMode_Auto == 0);
    assert(KernelProjectileCollisionQueryMode_Overlap == 1);
    assert(KernelProjectileCollisionQueryMode_Sweep == 2);
    assert(KernelProjectileCollisionQueryMode_Ray == 3);
    assert(offsetof(KernelProjectileMechanicsDefinition, collision_query_mode) >
           offsetof(KernelProjectileMechanicsDefinition,
                    expired_trigger));
    assert(offsetof(KernelProjectileMechanicsDefinition, lifetime_ticks) >
           offsetof(KernelProjectileMechanicsDefinition, speed));
    assert(offsetof(KernelHomingMechanicsDefinition, max_turn_degrees_per_tick) >
           offsetof(KernelHomingMechanicsDefinition, lock_cone_degrees));
    assert(offsetof(KernelBeamMechanicsDefinition, damage_per_tick) >
           offsetof(KernelBeamMechanicsDefinition, radius));
    assert(offsetof(KernelAgentVisionConfig, max_visible_neutrals) >
           offsetof(KernelAgentVisionConfig, max_visible_allies));
    assert(offsetof(KernelVisionStateView, visible_neutrals) >
           offsetof(KernelVisionStateView, visible_ally_count));
    assert(sizeof(&Kernel_ServerSetEntityHealth) > 0u);
    assert(KernelAgentCamp_PlayerSide == 1);
    assert(KernelAgentRelation_Hostile == 2);
    assert(KernelColliderShapeType_Cone == 4);
    assert(KernelColliderPurpose_Vision == (1u << 3));
    assert((KERNEL_CAPABILITY_CLIENT_MODE & KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_CAPABILITY_LAG_COMPENSATED_PROJECTILE &
            KERNEL_CAPABILITY_EVENT_PRESENTATION_TIME) == 0u);
    assert((KERNEL_CAPABILITY_RENDER_STATES_AT_TIME &
            KERNEL_CAPABILITY_RENDER_STATES) == 0u);
    assert((KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG &
            KERNEL_CAPABILITY_SERVER_ENTITY_CREATE) == 0u);
    assert((KERNEL_CAPABILITY_WEAPON_METADATA_QUERY &
            KERNEL_CAPABILITY_SERVER_MECHANICS_CONFIG) == 0u);
    assert((KERNEL_CAPABILITY_VISION_STATE_QUERY &
            KERNEL_CAPABILITY_COLLIDER_SHAPE_QUERY) == 0u);
    assert((KERNEL_CAPABILITY_PROJECTILE_RESPONSE_MASKS &
            KERNEL_CAPABILITY_HOMING_PROJECTILES) == 0u);
    assert((KERNEL_CAPABILITY_LAN_DISCOVERY &
            KERNEL_CAPABILITY_HOMING_PROJECTILES) == 0u);
    assert((KERNEL_COLLISION_LAYER_PLAYER_SIDE & KERNEL_COLLISION_LAYER_HOSTILE_SIDE) == 0u);
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_PLAYER_SIDE) != 0u);
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_HOSTILE_SIDE) != 0u);
    assert((KERNEL_COLLISION_MASK_DAMAGEABLE & KERNEL_COLLISION_LAYER_NEUTRAL) != 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_RELOADING) == 0u);
    assert((KERNEL_VISUAL_FLAG_MOVING & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    assert((InputButton_Dodge & InputButton_Parry) == 0u);
    assert(sizeof(abi_info.abi_version) == sizeof(uint32_t));
    assert(offsetof(KernelPlayerInput, client_action_time_us) > offsetof(KernelPlayerInput, input_seq));
    assert(offsetof(KernelPlayerInput, action_intent) > offsetof(KernelPlayerInput, selected_weapon));
    assert(offsetof(KernelPlayerInput, action_input) > offsetof(KernelPlayerInput, action_intent));
    assert(offsetof(RenderEntityState, entity_id) == 0u);
    assert(offsetof(RenderEntityState, hp) > offsetof(RenderEntityState, velocity));
    assert(offsetof(RenderEntityState, max_hp) > offsetof(RenderEntityState, hp));
    assert(offsetof(RenderEntityState, status) >
           offsetof(RenderEntityState, action_instance_id));
    assert(offsetof(RenderEntityState, template_id) >
           offsetof(RenderEntityState, status));
    assert(offsetof(RenderEntityState, collider_template_id) >
           offsetof(RenderEntityState, template_id));
    assert(sizeof(RenderEntityState) == 160u);
    assert(offsetof(KernelCombatStateDefinition, collider_template_id) >
           offsetof(KernelCombatStateDefinition, active_weapon_slot));
    assert(offsetof(KernelServerEntityCreateInfo, entity_template_id) >
           offsetof(KernelServerEntityCreateInfo, actor_template_id));
    assert(offsetof(KernelEntityTemplateDefinition, ai) >
           offsetof(KernelEntityTemplateDefinition, vision));
    assert(offsetof(KernelEntityTemplateDefinition, activated_trigger) >
           offsetof(KernelEntityTemplateDefinition, movement));
    assert(offsetof(KernelEntityTemplateDefinition, collision_trigger) >
           offsetof(KernelEntityTemplateDefinition, activated_trigger));
    assert(offsetof(KernelEntityTemplateDefinition, health_depleted_trigger) >
           offsetof(KernelEntityTemplateDefinition, collision_trigger));
    assert(offsetof(KernelEntityTemplateDefinition, destroy_entity_trigger) >
           offsetof(KernelEntityTemplateDefinition, health_depleted_trigger));
    assert(RenderEntityStatus_Active == 0u);
    assert((KERNEL_VISUAL_FLAG_HP_UNKNOWN & KERNEL_VISUAL_FLAG_DEAD) == 0u);
    assert(sizeof(KernelEntityLifecycleEvent) > 0u);
    assert(offsetof(KernelEvent, event_time_us) > offsetof(KernelEvent, code));
    assert(offsetof(KernelEvent, presentation_time_us) > offsetof(KernelEvent, event_time_us));
    assert(sizeof(state.entity_id) == sizeof(uint64_t));
    assert(sizeof(build_info.module_name) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.module_file_name) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.module_version) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.git_commit) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_timestamp) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_platform) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.build_config) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(build_info.compiler_info) == KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(lan_discovery_server_config.server_name) ==
           KERNEL_LAN_DISCOVERY_TEXT_SIZE);
    assert(sizeof(lan_discovery_result.server_name) == KERNEL_LAN_DISCOVERY_TEXT_SIZE);
    assert(sizeof(lan_discovery_result.server_endpoint_ip) ==
           KERNEL_LAN_DISCOVERY_TEXT_SIZE);
    assert(sizeof(lan_discovery_result.module_version) ==
           KERNEL_BUILD_INFO_TEXT_SIZE);
    assert(sizeof(lan_discovery_result.git_commit) == KERNEL_BUILD_INFO_TEXT_SIZE);

    return 0;
}
