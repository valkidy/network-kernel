using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelAbi
    {
        public static KernelAbiInfo GetInfo()
        {
            if (!KernelNative.Kernel_GetAbiInfo(
                    out KernelAbiInfo info,
                    (uint)Marshal.SizeOf<KernelAbiInfo>()))
            {
                throw new InvalidOperationException("Kernel_GetAbiInfo failed.");
            }

            return info;
        }

        public static KernelBuildInfo GetBuildInfo()
        {
            int managedSize = Marshal.SizeOf<KernelBuildInfo>();
            if (!KernelNative.Kernel_GetBuildInfo(
                    out KernelBuildInfo info,
                    (uint)managedSize))
            {
                throw new InvalidOperationException("Kernel_GetBuildInfo failed.");
            }
            RequireSize(nameof(KernelBuildInfo), info.struct_size, managedSize);

            return info;
        }

        public static void ValidateNativeAbi()
        {
            KernelAbiInfo info = GetInfo();
            if (info.abi_version != KernelConstants.AbiVersion)
            {
                throw new InvalidOperationException(
                    $"Unsupported kernel ABI version {info.abi_version}; expected {KernelConstants.AbiVersion}.");
            }
            RequireCapability(
                info,
                KernelConstants.CapabilityLocalPlayerInfo,
                "Kernel local-player info capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityCreate,
                "Kernel server entity create capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityDestroy,
                "Kernel server entity destroy capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityTransformWrite,
                "Kernel server entity transform-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityVelocityWrite,
                "Kernel server entity velocity-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityStateWrite,
                "Kernel server entity state-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityQuery,
                "Kernel server entity query capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityLagCompensatedProjectile,
                "Kernel lag-compensated projectile capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityEventPresentationTime,
                "Kernel event presentation-time capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityRenderStatesAtTime,
                "Kernel render-states-at-time capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerMechanicsConfig,
                "Kernel server mechanics config capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityWeaponMetadataQuery,
                "Kernel weapon metadata query capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityProjectileResponseMasks,
                "Kernel projectile response-mask capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityHomingProjectiles,
                "Kernel homing projectile capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityLANDiscovery,
                "Kernel LAN discovery capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityGameplayCatalog,
                "Kernel gameplay catalog capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityProjectileSpawnBatch,
                "Kernel projectile spawn batch capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityDebugRecords,
                "Kernel debug records capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityColliderShapeQuery,
                "Kernel collider shape query capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityBenchmarkStats,
                "Kernel benchmark stats capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityNetworkStats,
                "Kernel network stats capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityEntityLifecycleEvents,
                "Kernel entity lifecycle event capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityVisionStateQuery,
                "Kernel vision state query capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityGameplayCatalogSync,
                "Kernel gameplay catalog sync capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityControlPlaneRpc,
                "Kernel control-plane RPC capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityActionTimeline,
                "Kernel action timeline capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityLocalActionResults,
                "Kernel local action result capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityRemoteActionPresentation,
                "Kernel remote action presentation capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityActionIntents,
                "Kernel action intent capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityItemPropSystem,
                "Kernel item/prop system capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilitySkeletonRenderStates,
                "Kernel skeleton render-state capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilitySkeletonBindPose,
                "Kernel skeleton bind-pose capability is missing.");

            RequireSize(nameof(KernelAbiInfo), info.struct_size, Marshal.SizeOf<KernelAbiInfo>());
            RequireSize(nameof(KernelConfig), info.kernel_config_size, Marshal.SizeOf<KernelConfig>());
            RequireSize(
                nameof(KernelPlayerInput),
                info.player_input_size,
                Marshal.SizeOf<KernelPlayerInput>());
            RequireSize(
                nameof(RenderEntityState),
                info.render_entity_state_size,
                Marshal.SizeOf<RenderEntityState>());
            RequireSize(nameof(KernelEvent), info.kernel_event_size, Marshal.SizeOf<KernelEvent>());
            RequireSize(
                nameof(KernelLocalPlayerInfo),
                info.local_player_info_size,
                Marshal.SizeOf<KernelLocalPlayerInfo>());
            RequireSize(
                nameof(KernelServerEntityCreateInfo),
                info.server_entity_create_info_size,
                Marshal.SizeOf<KernelServerEntityCreateInfo>());
            RequireSize(
                nameof(KernelServerEntityState),
                info.server_entity_state_size,
                Marshal.SizeOf<KernelServerEntityState>());
            RequireSize(
                nameof(KernelWeaponMechanicsDefinition),
                info.weapon_mechanics_definition_size,
                Marshal.SizeOf<KernelWeaponMechanicsDefinition>());
            RequireSize(
                nameof(KernelProjectileMechanicsDefinition),
                info.projectile_mechanics_definition_size,
                Marshal.SizeOf<KernelProjectileMechanicsDefinition>());
            RequireSize(
                nameof(KernelAreaEffectMechanicsDefinition),
                info.area_effect_mechanics_definition_size,
                Marshal.SizeOf<KernelAreaEffectMechanicsDefinition>());
            RequireSize(
                nameof(KernelBeamMechanicsDefinition),
                info.beam_mechanics_definition_size,
                Marshal.SizeOf<KernelBeamMechanicsDefinition>());
            RequireSize(
                nameof(KernelCombatStateDefinition),
                info.combat_state_definition_size,
                Marshal.SizeOf<KernelCombatStateDefinition>());
            RequireSize(
                nameof(KernelHomingMechanicsDefinition),
                info.homing_mechanics_definition_size,
                Marshal.SizeOf<KernelHomingMechanicsDefinition>());
            RequireSize(
                nameof(KernelHomingState),
                info.homing_state_size,
                Marshal.SizeOf<KernelHomingState>());
            RequireSize(
                nameof(KernelLANDiscoveryServerConfig),
                info.lan_discovery_server_config_size,
                Marshal.SizeOf<KernelLANDiscoveryServerConfig>());
            RequireSize(
                nameof(KernelLANDiscoveryQueryConfig),
                info.lan_discovery_query_config_size,
                Marshal.SizeOf<KernelLANDiscoveryQueryConfig>());
            RequireSize(
                nameof(KernelLANDiscoveryResult),
                info.lan_discovery_result_size,
                Marshal.SizeOf<KernelLANDiscoveryResult>());
            RequireSize(
                nameof(KernelGameplayCatalogDefinition),
                info.gameplay_catalog_definition_size,
                Marshal.SizeOf<KernelGameplayCatalogDefinition>());
            RequireSize(
                nameof(KernelPropPopulationRuleDefinition),
                info.prop_population_rule_definition_size,
                Marshal.SizeOf<KernelPropPopulationRuleDefinition>());
            RequireSize(
                nameof(KernelGameplayCatalogLoadResult),
                info.gameplay_catalog_load_result_size,
                Marshal.SizeOf<KernelGameplayCatalogLoadResult>());
            RequireSize(
                nameof(KernelGameplayCatalogLoadOptions),
                info.gameplay_catalog_load_options_size,
                Marshal.SizeOf<KernelGameplayCatalogLoadOptions>());
            RequireSize(
                nameof(KernelActorTemplateDefinition),
                info.actor_template_definition_size,
                Marshal.SizeOf<KernelActorTemplateDefinition>());
            RequireSize(
                nameof(KernelProjectileTemplateDefinition),
                info.projectile_template_definition_size,
                Marshal.SizeOf<KernelProjectileTemplateDefinition>());
            RequireSize(
                nameof(KernelColliderTemplateDefinition),
                info.collider_template_definition_size,
                Marshal.SizeOf<KernelColliderTemplateDefinition>());
            RequireSize(
                nameof(KernelColliderBindingDefinition),
                info.collider_binding_definition_size,
                Marshal.SizeOf<KernelColliderBindingDefinition>());
            RequireSize(
                nameof(KernelBenchmarkStats),
                info.benchmark_stats_size,
                Marshal.SizeOf<KernelBenchmarkStats>());
            RequireSize(
                nameof(KernelNetworkStatsConfig),
                info.network_stats_config_size,
                Marshal.SizeOf<KernelNetworkStatsConfig>());
            RequireSize(
                nameof(KernelNetworkStats),
                info.network_stats_size,
                Marshal.SizeOf<KernelNetworkStats>());
            RequireSize(
                nameof(KernelDebugRecordFilter),
                info.debug_record_filter_size,
                Marshal.SizeOf<KernelDebugRecordFilter>());
            RequireSize(
                nameof(KernelDebugInfo),
                info.debug_info_size,
                Marshal.SizeOf<KernelDebugInfo>());
            RequireSize(
                nameof(KernelColliderShapeQuery),
                info.collider_shape_query_size,
                Marshal.SizeOf<KernelColliderShapeQuery>());
            RequireSize(
                nameof(KernelColliderShapeView),
                info.collider_shape_view_size,
                Marshal.SizeOf<KernelColliderShapeView>());
            RequireSize(
                nameof(KernelAgentVisionConfig),
                info.agent_vision_config_size,
                Marshal.SizeOf<KernelAgentVisionConfig>());
            RequireSize(
                nameof(KernelVisionStateQuery),
                info.vision_state_query_size,
                Marshal.SizeOf<KernelVisionStateQuery>());
            RequireSize(
                nameof(KernelVisionStateView),
                info.vision_state_view_size,
                Marshal.SizeOf<KernelVisionStateView>());
            RequireSize(
                nameof(KernelGameplayCatalogManifest),
                info.gameplay_catalog_manifest_size,
                Marshal.SizeOf<KernelGameplayCatalogManifest>());
            RequireSize(
                nameof(KernelGameplayCatalogSyncStatus),
                info.gameplay_catalog_sync_status_size,
                Marshal.SizeOf<KernelGameplayCatalogSyncStatus>());
            RequireSize(
                nameof(KernelEntityTemplateDefinition),
                info.entity_template_definition_size,
                Marshal.SizeOf<KernelEntityTemplateDefinition>());
            RequireSize(
                nameof(KernelEntityAiDefinition),
                info.entity_ai_definition_size,
                Marshal.SizeOf<KernelEntityAiDefinition>());
            RequireSize(
                nameof(KernelActionTemplateDefinition),
                info.action_template_definition_size,
                Marshal.SizeOf<KernelActionTemplateDefinition>());
            RequireSize(
                nameof(KernelActionRuntimeView),
                info.action_runtime_view_size,
                Marshal.SizeOf<KernelActionRuntimeView>());
            RequireSize(
                nameof(KernelLocalActionResult),
                info.local_action_result_size,
                Marshal.SizeOf<KernelLocalActionResult>());
            RequireSize(
                nameof(KernelRemoteActionPresentationEvent),
                info.remote_action_presentation_event_size,
                Marshal.SizeOf<KernelRemoteActionPresentationEvent>());
            RequireSize(
                nameof(KernelActionIntent),
                info.action_intent_size,
                Marshal.SizeOf<KernelActionIntent>());
            RequireSize(
                nameof(KernelActionInput),
                info.action_input_size,
                Marshal.SizeOf<KernelActionInput>());
            RequireSize(
                nameof(KernelItemTemplateDefinition),
                info.item_template_definition_size,
                Marshal.SizeOf<KernelItemTemplateDefinition>());
            RequireSize(
                nameof(KernelGameplayRequest),
                info.gameplay_request_size,
                Marshal.SizeOf<KernelGameplayRequest>());
            RequireSize(
                nameof(KernelGameplayRequestOutcome),
                info.gameplay_request_outcome_size,
                Marshal.SizeOf<KernelGameplayRequestOutcome>());
            RequireSize(
                nameof(KernelItemInstanceView),
                info.item_instance_view_size,
                Marshal.SizeOf<KernelItemInstanceView>());
            RequireSize(
                nameof(KernelInventoryContainerView),
                info.inventory_container_view_size,
                Marshal.SizeOf<KernelInventoryContainerView>());
            RequireSize(
                nameof(KernelInventoryDelta),
                info.inventory_delta_size,
                Marshal.SizeOf<KernelInventoryDelta>());
            RequireSize(
                nameof(KernelBoneLocalTransform),
                info.bone_local_transform_size,
                Marshal.SizeOf<KernelBoneLocalTransform>());
            RequireSize(
                nameof(KernelSkeletonRenderState),
                info.skeleton_render_state_size,
                Marshal.SizeOf<KernelSkeletonRenderState>());
            RequireSize(
                nameof(KernelSkeletonRenderStateResult),
                info.skeleton_render_state_result_size,
                Marshal.SizeOf<KernelSkeletonRenderStateResult>());
            RequireSize(
                nameof(KernelSkeletonAssetDefinition),
                info.skeleton_asset_definition_size,
                Marshal.SizeOf<KernelSkeletonAssetDefinition>());
            RequireSize(
                nameof(KernelSkeletonBindingDefinition),
                info.skeleton_binding_definition_size,
                Marshal.SizeOf<KernelSkeletonBindingDefinition>());
            RequireSize(
                nameof(KernelSkeletonLegDefinition),
                info.skeleton_leg_definition_size,
                Marshal.SizeOf<KernelSkeletonLegDefinition>());
        }

        private static void RequireCapability(
            KernelAbiInfo info,
            ulong capability,
            string message)
        {
            if ((info.capability_flags & capability) == 0)
            {
                throw new InvalidOperationException(message);
            }
        }

        private static void RequireSize(string typeName, uint nativeSize, int managedSize)
        {
            if (nativeSize != managedSize)
            {
                throw new InvalidOperationException(
                    $"{typeName} ABI size mismatch: native={nativeSize}, managed={managedSize}.");
            }
        }
    }
}
