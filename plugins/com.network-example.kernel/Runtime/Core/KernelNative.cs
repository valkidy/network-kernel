using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    internal static class KernelNative
    {
        internal const string LibraryName = "network_kernel";

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetAbiInfo(
            out KernelAbiInfo outInfo,
            uint outInfoSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetBuildInfo(
            out KernelBuildInfo outInfo,
            uint outInfoSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetLocalPlayerInfo(
            IntPtr kernel,
            out KernelLocalPlayerInfo outInfo);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Kernel_LANDiscovery_Create();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_LANDiscovery_Destroy(IntPtr discovery);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_LANDiscovery_StartServer(
            IntPtr discovery,
            ref KernelLANDiscoveryServerConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_LANDiscovery_StopServer(IntPtr discovery);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_LANDiscovery_Query(
            IntPtr discovery,
            ref KernelLANDiscoveryQueryConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_LANDiscovery_PollResults(
            IntPtr discovery,
            [Out] KernelLANDiscoveryResult[] outResults,
            uint maxResults);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_LANDiscovery_ClearResults(IntPtr discovery);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Kernel_Create(ref KernelConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_Destroy(IntPtr kernel);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_SetPhysicsConfig(
            IntPtr kernel,
            ref KernelPhysicsConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_SetSessionRules(
            IntPtr kernel,
            ref KernelSessionRulesConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_SetStaticCollisionScene(
            IntPtr kernel,
            ref KernelStaticCollisionSceneConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_InvokeRpcCommand(
            IntPtr kernel,
            [In] byte[] requestJson,
            uint requestJsonSize,
            out ulong outRequestId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_PollRpcResponse(
            IntPtr kernel,
            ulong requestId,
            IntPtr outResponseJson,
            uint responseJsonCapacity,
            out uint outResponseJsonSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartClient(
            IntPtr kernel,
            [MarshalAs(UnmanagedType.LPStr)] string address);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartClientCatalogSync(
            IntPtr kernel,
            [MarshalAs(UnmanagedType.LPStr)] string address,
            ref KernelGameplayCatalogSyncClientConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartListenServer(IntPtr kernel, ushort port);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartDedicatedServer(IntPtr kernel, ushort port);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_SetGameplayCatalogSyncBundle(
            IntPtr kernel,
            ref KernelGameplayCatalogSyncServerConfig config,
            ref KernelGameplayCatalogManifest outManifest);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetGameplayCatalogSyncStatus(
            IntPtr kernel,
            ref KernelGameplayCatalogSyncStatus outStatus);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_RequestGameplayCatalogBundle(IntPtr kernel);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_CopyGameplayCatalogBundle(
            IntPtr kernel,
            [Out] byte[] outBundle,
            uint outCapacity,
            out uint outBundleSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ContinueClientHandshake(IntPtr kernel);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_Update(IntPtr kernel, float deltaSeconds);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_SubmitPlayerInput(
            IntPtr kernel,
            uint localPlayerId,
            ref KernelPlayerInput input);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_LoadGameplayCatalog(
            IntPtr kernel,
            ref KernelGameplayCatalogDefinition catalog,
            IntPtr options);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_LoadGameplayCatalogFromMemory(
            IntPtr kernel,
            [In] byte[] bundleBytes,
            uint bundleSize,
            [MarshalAs(UnmanagedType.LPStr)] string entryPath,
            ref KernelGameplayCatalogLoadResult outResult);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetRenderStates(
            IntPtr kernel,
            [Out] RenderEntityState[] outStates,
            uint maxStates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetRenderStatesAtTime(
            IntPtr kernel,
            ulong clientRenderTimeUs,
            [Out] RenderEntityState[] outStates,
            uint maxStates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetSkeletonRenderStates(
            IntPtr kernel,
            [Out] KernelSkeletonRenderState[] outStates,
            uint maxStates,
            [Out] KernelBoneLocalTransform[] outBoneTransforms,
            uint maxBoneTransforms,
            ref KernelSkeletonRenderStateResult outResult);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetSkeletonRenderStatesAtTime(
            IntPtr kernel,
            ulong clientRenderTimeUs,
            [Out] KernelSkeletonRenderState[] outStates,
            uint maxStates,
            [Out] KernelBoneLocalTransform[] outBoneTransforms,
            uint maxBoneTransforms,
            ref KernelSkeletonRenderStateResult outResult);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetSkeletonBindPose(
            IntPtr kernel,
            uint skeletonAssetId,
            ulong skeletonContentHash,
            [Out] KernelBoneLocalTransform[] outBoneTransforms,
            uint maxBoneTransforms);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollEvents(
            IntPtr kernel,
            [Out] KernelEvent[] outEvents,
            uint maxEvents);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollEntityLifecycleEvents(
            IntPtr kernel,
            [Out] KernelEntityLifecycleEvent[] outEvents,
            uint maxEvents);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollLocalActionResults(
            IntPtr kernel,
            [Out] KernelLocalActionResult[] outResults,
            uint maxResults);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollRemoteActionPresentationEvents(
            IntPtr kernel,
            [Out] KernelRemoteActionPresentationEvent[] outEvents,
            uint maxEvents);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_QueryStatusEffects(
            IntPtr kernel,
            uint entityNetId,
            [Out] KernelStatusEffectView[] outEffects,
            uint maxEffects);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetBenchmarkStats(
            IntPtr kernel,
            ref KernelBenchmarkStats outStats);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetNetworkStats(
            IntPtr kernel,
            ref KernelNetworkStats outStats);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollDebugRecords(
            IntPtr kernel,
            IntPtr filter,
            [Out] KernelDebugInfo[] outRecords,
            uint maxRecords);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_QueryColliderShapes(
            IntPtr kernel,
            IntPtr query,
            [Out] KernelColliderShapeView[] outShapes,
            uint maxShapes);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_QueryVisionState(
            IntPtr kernel,
            IntPtr query,
            [Out] KernelVisionStateView[] outStates,
            uint maxStates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetProjectileTemplates(
            IntPtr kernel,
            [Out] KernelProjectileTemplateDefinition[] outTemplates,
            uint maxTemplates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetActionTemplate(
            IntPtr kernel,
            uint actionTemplateId,
            ref KernelActionTemplateDefinition outDefinition);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetActorTemplates(
            IntPtr kernel,
            [Out] KernelActorTemplateDefinition[] outTemplates,
            uint maxTemplates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetColliderTemplates(
            IntPtr kernel,
            [Out] KernelColliderTemplateDefinition[] outTemplates,
            uint maxTemplates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetColliderBindings(
            IntPtr kernel,
            [Out] KernelColliderBindingDefinition[] outBindings,
            uint maxBindings);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerCreateEntity(
            IntPtr kernel,
            ref KernelServerEntityCreateInfo createInfo,
            out uint outNetId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerCreateInventoryContainer(
            IntPtr kernel,
            uint ownerEntityId,
            uint slotCapacity,
            out ulong outContainerId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerCreateInventoryItem(
            IntPtr kernel,
            uint itemTemplateId,
            uint quantity,
            ulong containerId,
            out ulong outItemInstanceId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerCreateWorldItem(
            IntPtr kernel,
            uint itemTemplateId,
            uint quantity,
            ref KernelVec3 position,
            out ulong outItemInstanceId,
            out uint outPropEntityId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSubmitGameplayRequest(
            IntPtr kernel,
            ref KernelGameplayRequest request);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_SubmitGameplayRequest(
            IntPtr kernel,
            ref KernelGameplayRequest request);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetItemInstance(
            IntPtr kernel,
            ulong itemInstanceId,
            ref KernelItemInstanceView outView);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetInventoryContainer(
            IntPtr kernel,
            ulong containerId,
            ref KernelInventoryContainerView outView);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_CopyOwnedInventoryContainers(
            IntPtr kernel,
            uint ownerEntityId,
            [Out] KernelInventoryContainerView[] outContainers,
            uint maxContainers);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_CopyInventorySlots(
            IntPtr kernel,
            ulong containerId,
            [Out] KernelItemInstanceView[] outItems,
            uint maxItems);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollGameplayRequestOutcomes(
            IntPtr kernel,
            [Out] KernelGameplayRequestOutcome[] outOutcomes,
            uint maxOutcomes);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollInventoryDeltas(
            IntPtr kernel,
            ulong containerId,
            [Out] KernelInventoryDelta[] outDeltas,
            uint maxDeltas);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerDestroyEntity(
            IntPtr kernel,
            uint netId,
            uint reason);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityTransform(
            IntPtr kernel,
            uint netId,
            ref KernelVec3 position,
            ref KernelQuat rotation);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityVelocity(
            IntPtr kernel,
            uint netId,
            ref KernelVec3 velocity);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityState(
            IntPtr kernel,
            uint netId,
            ushort animationState,
            uint visualFlags);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityHealth(
            IntPtr kernel,
            uint netId,
            ushort hp);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSubmitEntityInput(
            IntPtr kernel,
            uint netId,
            ref KernelPlayerInput input);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetEntityState(
            IntPtr kernel,
            uint netId,
            ref KernelServerEntityState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityCombatState(
            IntPtr kernel,
            uint netId,
            ref KernelCombatStateDefinition combatState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityActorTemplate(
            IntPtr kernel,
            uint netId,
            uint actorTemplateId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityVisionConfig(
            IntPtr kernel,
            uint netId,
            ref KernelAgentVisionConfig visionConfig);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerClearEntityVisionConfig(
            IntPtr kernel,
            uint netId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            ref KernelWeaponMechanicsDefinition weaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerClearEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            byte weaponId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            byte weaponId,
            ref KernelWeaponMechanicsDefinition outWeaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetHomingState(
            IntPtr kernel,
            uint netId,
            ref KernelHomingState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerValidateMechanicsConfig(
            ref KernelWeaponMechanicsDefinition weaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_ServerQueryEntities(
            IntPtr kernel,
            ushort entityTypeFilter,
            [Out] KernelServerEntityState[] outStates,
            uint maxStates);
    }
}
