using System;
using System.Runtime.InteropServices;
using System.Text;

namespace NetworkExample.Kernel
{
    public sealed class Kernel : IDisposable
    {
        private IntPtr handle;

        public Kernel(KernelConfig config)
        {
            KernelAbi.ValidateNativeAbi();
            handle = KernelNative.Kernel_Create(ref config);
            if (handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("Kernel_Create failed.");
            }
        }

        public bool IsCreated => handle != IntPtr.Zero;

        public uint LocalPlayerNetId
        {
            get
            {
                return TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info)
                    ? info.player_net_id
                    : 0U;
            }
        }

        public bool IsClientReady
        {
            get
            {
                return TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) &&
                       info.has_welcome != 0 &&
                       info.connected != 0;
            }
        }

        internal IntPtr Handle
        {
            get
            {
                ThrowIfDisposed();
                return handle;
            }
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        public bool StartClient(string address)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_StartClient(handle, address);
        }

        public bool SetPhysicsConfig(KernelPhysicsConfig config)
        {
            ThrowIfDisposed();
            config.struct_size = KernelPhysicsConfig.StructSize;
            return KernelNative.Kernel_SetPhysicsConfig(handle, ref config);
        }

        public bool SetSessionRules(KernelSessionRulesConfig config)
        {
            ThrowIfDisposed();
            config.struct_size = KernelSessionRulesConfig.StructSize;
            return KernelNative.Kernel_SetSessionRules(handle, ref config);
        }

        public bool SetStaticCollisionScene(
            byte[] artifactBytes,
            uint sceneId,
            uint colliderId,
            uint collisionLayer = KernelConstants.StaticCollisionLayerTerrain)
        {
            ThrowIfDisposed();
            if (artifactBytes == null)
            {
                throw new ArgumentNullException(nameof(artifactBytes));
            }
            if (artifactBytes.Length == 0)
            {
                throw new ArgumentException(
                    "Static collision artifact must not be empty.",
                    nameof(artifactBytes));
            }
            if ((uint)artifactBytes.Length > KernelConstants.StaticCollisionSceneMaxBytes)
            {
                throw new ArgumentException(
                    $"Static collision artifact exceeds {KernelConstants.StaticCollisionSceneMaxBytes} bytes.",
                    nameof(artifactBytes));
            }

            GCHandle artifactHandle = GCHandle.Alloc(artifactBytes, GCHandleType.Pinned);
            try
            {
                var config = new KernelStaticCollisionSceneConfig
                {
                    struct_size = KernelStaticCollisionSceneConfig.StructSize,
                    artifact_bytes = artifactHandle.AddrOfPinnedObject(),
                    artifact_size = (uint)artifactBytes.Length,
                    scene_id = sceneId,
                    collider_id = colliderId,
                    collision_layer = collisionLayer,
                };
                return KernelNative.Kernel_SetStaticCollisionScene(handle, ref config);
            }
            finally
            {
                artifactHandle.Free();
            }
        }

        public bool StartClientCatalogSync(
            string address,
            uint maxBundleSize,
            uint timeoutMs)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(address))
            {
                throw new ArgumentException("Client address must not be empty.", nameof(address));
            }

            var config = new KernelGameplayCatalogSyncClientConfig
            {
                struct_size = KernelGameplayCatalogSyncClientConfig.StructSize,
                max_bundle_size = maxBundleSize,
                timeout_ms = timeoutMs,
            };
            return KernelNative.Kernel_StartClientCatalogSync(handle, address, ref config);
        }

        public bool StartListenServer(ushort port)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_StartListenServer(handle, port);
        }

        public bool StartDedicatedServer(ushort port)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_StartDedicatedServer(handle, port);
        }

        public bool InvokeRpcCommand(string requestJson, out ulong requestId)
        {
            ThrowIfDisposed();
            if (requestJson == null)
            {
                throw new ArgumentNullException(nameof(requestJson));
            }

            byte[] requestBytes = Encoding.UTF8.GetBytes(requestJson);
            return KernelNative.Kernel_InvokeRpcCommand(
                handle,
                requestBytes,
                (uint)requestBytes.Length,
                out requestId);
        }

        public bool TryPollRpcResponse(ulong requestId, out string responseJson)
        {
            ThrowIfDisposed();
            responseJson = null;
            if (!KernelNative.Kernel_PollRpcResponse(
                    handle,
                    requestId,
                    IntPtr.Zero,
                    0,
                    out uint requiredSize) &&
                requiredSize == 0)
            {
                return false;
            }
            if (requiredSize > int.MaxValue)
            {
                throw new InvalidOperationException(
                    $"Kernel RPC response is too large: {requiredSize} bytes.");
            }
            if (requiredSize == 0)
            {
                return false;
            }

            IntPtr responsePtr = Marshal.AllocHGlobal((int)requiredSize);
            try
            {
                if (!KernelNative.Kernel_PollRpcResponse(
                        handle,
                        requestId,
                        responsePtr,
                        requiredSize,
                        out uint responseSize))
                {
                    return false;
                }

                byte[] responseBytes = new byte[responseSize];
                Marshal.Copy(responsePtr, responseBytes, 0, (int)responseSize);
                responseJson = Encoding.UTF8.GetString(responseBytes);
                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(responsePtr);
            }
        }

        public bool SetGameplayCatalogSyncBundle(
            byte[] bundleBytes,
            string entryPath,
            string contentNamespace,
            out KernelGameplayCatalogManifest manifest)
        {
            ThrowIfDisposed();
            if (bundleBytes == null)
            {
                throw new ArgumentNullException(nameof(bundleBytes));
            }
            if (bundleBytes.Length == 0)
            {
                throw new ArgumentException("Bundle bytes must not be empty.", nameof(bundleBytes));
            }
            if (string.IsNullOrEmpty(entryPath))
            {
                throw new ArgumentException("Entry path must not be empty.", nameof(entryPath));
            }

            GCHandle bundleHandle = default;
            IntPtr entryPathPointer = IntPtr.Zero;
            IntPtr contentNamespacePointer = IntPtr.Zero;
            manifest = KernelGameplayCatalogManifest.Create();
            try
            {
                bundleHandle = GCHandle.Alloc(bundleBytes, GCHandleType.Pinned);
                entryPathPointer = Marshal.StringToHGlobalAnsi(entryPath);
                if (!string.IsNullOrEmpty(contentNamespace))
                {
                    contentNamespacePointer = Marshal.StringToHGlobalAnsi(contentNamespace);
                }

                var config = new KernelGameplayCatalogSyncServerConfig
                {
                    struct_size = KernelGameplayCatalogSyncServerConfig.StructSize,
                    bundle_bytes = bundleHandle.AddrOfPinnedObject(),
                    bundle_size = (uint)bundleBytes.Length,
                    entry_path = entryPathPointer,
                    content_namespace = contentNamespacePointer,
                };
                return KernelNative.Kernel_SetGameplayCatalogSyncBundle(
                    handle,
                    ref config,
                    ref manifest);
            }
            finally
            {
                FreeIfAllocated(bundleHandle);
                if (entryPathPointer != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(entryPathPointer);
                }
                if (contentNamespacePointer != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(contentNamespacePointer);
                }
            }
        }

        public bool TryGetGameplayCatalogSyncStatus(
            out KernelGameplayCatalogSyncStatus status)
        {
            ThrowIfDisposed();
            status = KernelGameplayCatalogSyncStatus.Create();
            return KernelNative.Kernel_GetGameplayCatalogSyncStatus(handle, ref status);
        }

        public bool RequestGameplayCatalogBundle()
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_RequestGameplayCatalogBundle(handle);
        }

        public bool CopyGameplayCatalogBundle(byte[] output, out uint copiedSize)
        {
            ThrowIfDisposed();
            copiedSize = 0;
            if (output == null || output.Length == 0)
            {
                return false;
            }

            return KernelNative.Kernel_CopyGameplayCatalogBundle(
                handle,
                output,
                (uint)output.Length,
                out copiedSize);
        }

        public bool ContinueClientHandshake()
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ContinueClientHandshake(handle);
        }

        public void Update(float deltaSeconds)
        {
            ThrowIfDisposed();
            KernelNative.Kernel_Update(handle, deltaSeconds);
        }

        public void SubmitInput(uint localPlayerId, KernelPlayerInput input)
        {
            ThrowIfDisposed();
            KernelNative.Kernel_SubmitPlayerInput(handle, localPlayerId, ref input);
        }

        public bool LoadGameplayCatalog(KernelGameplayCatalog catalog)
        {
            ThrowIfDisposed();
            return LoadGameplayCatalog(handle, catalog);
        }

        public bool LoadGameplayCatalogFromMemory(
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult result)
        {
            ThrowIfDisposed();
            if (bundleBytes == null)
            {
                throw new ArgumentNullException(nameof(bundleBytes));
            }
            if (entryPath == null)
            {
                throw new ArgumentNullException(nameof(entryPath));
            }

            result = new KernelGameplayCatalogLoadResult
            {
                struct_size = KernelGameplayCatalogLoadResult.StructSize,
            };
            return KernelNative.Kernel_LoadGameplayCatalogFromMemory(
                handle,
                bundleBytes,
                (uint)bundleBytes.Length,
                entryPath,
                ref result);
        }

        public bool TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_GetLocalPlayerInfo(handle, out info);
        }

        public uint GetRenderStates(RenderEntityState[] states)
        {
            ThrowIfDisposed();
            if (states == null || states.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_GetRenderStates(handle, states, (uint)states.Length);
        }

        public uint GetRenderStatesAtTime(ulong clientRenderTimeUs, RenderEntityState[] states)
        {
            ThrowIfDisposed();
            if (states == null || states.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_GetRenderStatesAtTime(
                handle,
                clientRenderTimeUs,
                states,
                (uint)states.Length);
        }

        public uint PollEvents(KernelEvent[] events)
        {
            ThrowIfDisposed();
            if (events == null || events.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_PollEvents(handle, events, (uint)events.Length);
        }

        public uint PollEntityLifecycleEvents(KernelEntityLifecycleEvent[] events)
        {
            ThrowIfDisposed();
            if (events == null || events.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_PollEntityLifecycleEvents(
                handle,
                events,
                (uint)events.Length);
        }

        public uint PollLocalActionResults(KernelLocalActionResult[] results)
        {
            ThrowIfDisposed();
            if (results == null || results.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_PollLocalActionResults(
                handle,
                results,
                (uint)results.Length);
        }

        public uint PollRemoteActionPresentationEvents(
            KernelRemoteActionPresentationEvent[] events)
        {
            ThrowIfDisposed();
            if (events == null || events.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_PollRemoteActionPresentationEvents(
                handle,
                events,
                (uint)events.Length);
        }

        public bool TryGetBenchmarkStats(out KernelBenchmarkStats stats)
        {
            ThrowIfDisposed();
            stats = new KernelBenchmarkStats
            {
                struct_size = KernelBenchmarkStats.StructSize,
            };
            return KernelNative.Kernel_GetBenchmarkStats(handle, ref stats);
        }

        public bool TryGetNetworkStats(out KernelNetworkStats stats)
        {
            ThrowIfDisposed();
            stats = new KernelNetworkStats
            {
                struct_size = KernelNetworkStats.StructSize,
            };
            return KernelNative.Kernel_GetNetworkStats(handle, ref stats);
        }

        public uint PollDebugRecords(
            KernelDebugRecordFilter? filter,
            KernelDebugInfo[] records)
        {
            ThrowIfDisposed();
            if (records == null || records.Length == 0)
            {
                return 0;
            }

            IntPtr filterPtr = IntPtr.Zero;
            try
            {
                if (filter.HasValue)
                {
                    KernelDebugRecordFilter nativeFilter = filter.Value;
                    if (nativeFilter.struct_size == 0)
                    {
                        nativeFilter.struct_size = KernelDebugRecordFilter.StructSize;
                    }
                    filterPtr = Marshal.AllocHGlobal(Marshal.SizeOf<KernelDebugRecordFilter>());
                    Marshal.StructureToPtr(nativeFilter, filterPtr, false);
                }

                return KernelNative.Kernel_PollDebugRecords(
                    handle,
                    filterPtr,
                    records,
                    (uint)records.Length);
            }
            finally
            {
                if (filterPtr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(filterPtr);
                }
            }
        }

        public uint QueryColliderShapes(
            KernelColliderShapeQuery? query,
            KernelColliderShapeView[] shapes)
        {
            ThrowIfDisposed();
            if (shapes == null || shapes.Length == 0)
            {
                return 0;
            }

            IntPtr queryPtr = IntPtr.Zero;
            try
            {
                if (query.HasValue)
                {
                    KernelColliderShapeQuery nativeQuery = query.Value;
                    if (nativeQuery.struct_size == 0)
                    {
                        nativeQuery.struct_size = KernelColliderShapeQuery.StructSize;
                    }
                    queryPtr = Marshal.AllocHGlobal(Marshal.SizeOf<KernelColliderShapeQuery>());
                    Marshal.StructureToPtr(nativeQuery, queryPtr, false);
                }

                return KernelNative.Kernel_QueryColliderShapes(
                    handle,
                    queryPtr,
                    shapes,
                    (uint)shapes.Length);
            }
            finally
            {
                if (queryPtr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(queryPtr);
                }
            }
        }

        public uint QueryVisionState(
            KernelVisionStateQuery? query,
            KernelVisionStateView[] states)
        {
            ThrowIfDisposed();
            if (states == null || states.Length == 0)
            {
                return 0;
            }

            IntPtr queryPtr = IntPtr.Zero;
            try
            {
                if (query.HasValue)
                {
                    KernelVisionStateQuery nativeQuery = query.Value;
                    if (nativeQuery.struct_size == 0)
                    {
                        nativeQuery.struct_size = KernelVisionStateQuery.StructSize;
                    }
                    queryPtr = Marshal.AllocHGlobal(Marshal.SizeOf<KernelVisionStateQuery>());
                    Marshal.StructureToPtr(nativeQuery, queryPtr, false);
                }

                return KernelNative.Kernel_QueryVisionState(
                    handle,
                    queryPtr,
                    states,
                    (uint)states.Length);
            }
            finally
            {
                if (queryPtr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(queryPtr);
                }
            }
        }

        public uint GetProjectileTemplates(KernelProjectileTemplateDefinition[] templates)
        {
            ThrowIfDisposed();
            if (templates == null || templates.Length == 0)
            {
                return KernelNative.Kernel_GetProjectileTemplates(handle, null, 0);
            }

            return KernelNative.Kernel_GetProjectileTemplates(
                handle,
                templates,
                (uint)templates.Length);
        }

        public bool TryGetActionTemplate(
            uint actionTemplateId,
            out KernelActionTemplateDefinition definition)
        {
            ThrowIfDisposed();
            definition = new KernelActionTemplateDefinition
            {
                struct_size = KernelActionTemplateDefinition.StructSize,
            };
            return KernelNative.Kernel_GetActionTemplate(
                handle,
                actionTemplateId,
                ref definition);
        }

        public uint GetActorTemplates(KernelActorTemplateDefinition[] templates)
        {
            ThrowIfDisposed();
            if (templates == null || templates.Length == 0)
            {
                return KernelNative.Kernel_GetActorTemplates(handle, null, 0);
            }

            return KernelNative.Kernel_GetActorTemplates(
                handle,
                templates,
                (uint)templates.Length);
        }

        public uint GetColliderTemplates(KernelColliderTemplateDefinition[] templates)
        {
            ThrowIfDisposed();
            if (templates == null || templates.Length == 0)
            {
                return KernelNative.Kernel_GetColliderTemplates(handle, null, 0);
            }

            return KernelNative.Kernel_GetColliderTemplates(
                handle,
                templates,
                (uint)templates.Length);
        }

        public uint GetColliderBindings(KernelColliderBindingDefinition[] bindings)
        {
            ThrowIfDisposed();
            if (bindings == null || bindings.Length == 0)
            {
                return KernelNative.Kernel_GetColliderBindings(handle, null, 0);
            }

            return KernelNative.Kernel_GetColliderBindings(
                handle,
                bindings,
                (uint)bindings.Length);
        }

        public bool ServerCreateEntity(
            KernelServerEntityCreateInfo createInfo,
            out uint netId)
        {
            ThrowIfDisposed();
            createInfo.struct_size = KernelServerEntityCreateInfo.StructSize;
            return KernelNative.Kernel_ServerCreateEntity(handle, ref createInfo, out netId);
        }

        public bool ServerCreateInventoryContainer(
            uint ownerEntityId,
            uint slotCapacity,
            out ulong containerId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerCreateInventoryContainer(
                handle,
                ownerEntityId,
                slotCapacity,
                out containerId);
        }

        public bool ServerCreateInventoryItem(
            uint itemTemplateId,
            uint quantity,
            ulong containerId,
            out ulong itemInstanceId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerCreateInventoryItem(
                handle,
                itemTemplateId,
                quantity,
                containerId,
                out itemInstanceId);
        }

        public bool ServerCreateWorldItem(
            uint itemTemplateId,
            uint quantity,
            KernelVec3 position,
            out ulong itemInstanceId,
            out uint propEntityId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerCreateWorldItem(
                handle,
                itemTemplateId,
                quantity,
                ref position,
                out itemInstanceId,
                out propEntityId);
        }

        public bool ServerSubmitGameplayRequest(KernelGameplayRequest request)
        {
            ThrowIfDisposed();
            request.struct_size = KernelGameplayRequest.StructSize;
            return KernelNative.Kernel_ServerSubmitGameplayRequest(handle, ref request);
        }

        public bool SubmitGameplayRequest(KernelGameplayRequest request)
        {
            ThrowIfDisposed();
            request.struct_size = KernelGameplayRequest.StructSize;
            return KernelNative.Kernel_SubmitGameplayRequest(handle, ref request);
        }

        public bool TryGetItemInstance(
            ulong itemInstanceId,
            out KernelItemInstanceView view)
        {
            ThrowIfDisposed();
            view = new KernelItemInstanceView
            {
                struct_size = KernelItemInstanceView.StructSize,
                portable_state_fields = new KernelPortableStateFieldDefinition[
                    KernelConstants.MaxPortableStateFields],
            };
            return KernelNative.Kernel_GetItemInstance(handle, itemInstanceId, ref view);
        }

        public bool TryGetInventoryContainer(
            ulong containerId,
            out KernelInventoryContainerView view)
        {
            ThrowIfDisposed();
            view = new KernelInventoryContainerView
            {
                struct_size = KernelInventoryContainerView.StructSize,
            };
            return KernelNative.Kernel_GetInventoryContainer(handle, containerId, ref view);
        }

        public uint CopyOwnedInventoryContainers(
            uint ownerEntityId,
            KernelInventoryContainerView[] containers)
        {
            ThrowIfDisposed();
            if (containers == null || containers.Length == 0)
            {
                return 0;
            }
            return KernelNative.Kernel_CopyOwnedInventoryContainers(
                handle,
                ownerEntityId,
                containers,
                (uint)containers.Length);
        }

        public uint CopyInventorySlots(ulong containerId, KernelItemInstanceView[] items)
        {
            ThrowIfDisposed();
            if (items == null || items.Length == 0)
            {
                return 0;
            }
            PrepareItemInstanceViews(items);
            return KernelNative.Kernel_CopyInventorySlots(
                handle,
                containerId,
                items,
                (uint)items.Length);
        }

        public uint PollGameplayRequestOutcomes(KernelGameplayRequestOutcome[] outcomes)
        {
            ThrowIfDisposed();
            if (outcomes == null || outcomes.Length == 0)
            {
                return 0;
            }
            return KernelNative.Kernel_PollGameplayRequestOutcomes(
                handle,
                outcomes,
                (uint)outcomes.Length);
        }

        public uint PollInventoryDeltas(ulong containerId, KernelInventoryDelta[] deltas)
        {
            ThrowIfDisposed();
            if (deltas == null || deltas.Length == 0)
            {
                return 0;
            }
            PrepareInventoryDeltas(deltas);
            return KernelNative.Kernel_PollInventoryDeltas(
                handle,
                containerId,
                deltas,
                (uint)deltas.Length);
        }

        public bool ServerDestroyEntity(uint netId, KernelDespawnReason reason)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerDestroyEntity(handle, netId, (uint)reason);
        }

        public bool ServerSetEntityTransform(
            uint netId,
            KernelVec3 position,
            KernelQuat rotation)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSetEntityTransform(
                handle,
                netId,
                ref position,
                ref rotation);
        }

        public bool ServerSetEntityVelocity(uint netId, KernelVec3 velocity)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSetEntityVelocity(handle, netId, ref velocity);
        }

        public bool ServerSetEntityState(
            uint netId,
            ushort animationState,
            uint visualFlags)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSetEntityState(
                handle,
                netId,
                animationState,
                visualFlags);
        }

        public bool ServerSetEntityHealth(uint netId, ushort hp)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSetEntityHealth(handle, netId, hp);
        }

        public bool ServerSubmitEntityInput(uint netId, KernelPlayerInput input)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSubmitEntityInput(handle, netId, ref input);
        }

        public bool ServerGetEntityState(uint netId, out KernelServerEntityState state)
        {
            ThrowIfDisposed();
            state = new KernelServerEntityState
            {
                struct_size = KernelServerEntityState.StructSize,
            };
            return KernelNative.Kernel_ServerGetEntityState(handle, netId, ref state);
        }

        public bool ServerSetEntityCombatState(
            uint netId,
            KernelCombatStateDefinition combatState)
        {
            ThrowIfDisposed();
            PrepareCombatState(ref combatState);
            return KernelNative.Kernel_ServerSetEntityCombatState(
                handle,
                netId,
                ref combatState);
        }

        public bool ServerSetEntityActorTemplate(uint netId, uint actorTemplateId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerSetEntityActorTemplate(
                handle,
                netId,
                actorTemplateId);
        }

        public bool ServerSetEntityVisionConfig(
            uint netId,
            KernelAgentVisionConfig visionConfig)
        {
            ThrowIfDisposed();
            if (visionConfig.struct_size == 0)
            {
                visionConfig.struct_size = KernelAgentVisionConfig.StructSize;
            }
            return KernelNative.Kernel_ServerSetEntityVisionConfig(
                handle,
                netId,
                ref visionConfig);
        }

        public bool ServerClearEntityVisionConfig(uint netId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerClearEntityVisionConfig(handle, netId);
        }

        public bool ServerSetEntityWeaponMechanics(
            uint netId,
            KernelWeaponMechanicsDefinition weaponMechanics)
        {
            ThrowIfDisposed();
            PrepareWeaponMechanics(ref weaponMechanics);
            return KernelNative.Kernel_ServerSetEntityWeaponMechanics(
                handle,
                netId,
                ref weaponMechanics);
        }

        public bool ServerClearEntityWeaponMechanics(uint netId, byte weaponId)
        {
            ThrowIfDisposed();
            return KernelNative.Kernel_ServerClearEntityWeaponMechanics(
                handle,
                netId,
                weaponId);
        }

        public bool ServerGetEntityWeaponMechanics(
            uint netId,
            byte weaponId,
            out KernelWeaponMechanicsDefinition weaponMechanics)
        {
            ThrowIfDisposed();
            weaponMechanics = new KernelWeaponMechanicsDefinition
            {
                struct_size = KernelWeaponMechanicsDefinition.StructSize,
            };
            return KernelNative.Kernel_ServerGetEntityWeaponMechanics(
                handle,
                netId,
                weaponId,
                ref weaponMechanics);
        }

        public bool ServerGetHomingState(uint netId, out KernelHomingState state)
        {
            ThrowIfDisposed();
            state = new KernelHomingState
            {
                struct_size = KernelHomingState.StructSize,
            };
            return KernelNative.Kernel_ServerGetHomingState(handle, netId, ref state);
        }

        public static bool ServerValidateMechanicsConfig(
            KernelWeaponMechanicsDefinition weaponMechanics)
        {
            PrepareWeaponMechanics(ref weaponMechanics);
            return KernelNative.Kernel_ServerValidateMechanicsConfig(ref weaponMechanics);
        }

        public uint ServerQueryEntities(
            KernelEntityType entityTypeFilter,
            KernelServerEntityState[] states)
        {
            ThrowIfDisposed();
            if (states == null || states.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_ServerQueryEntities(
                handle,
                (ushort)entityTypeFilter,
                states,
                (uint)states.Length);
        }

        private static void PrepareCombatState(ref KernelCombatStateDefinition combatState)
        {
            combatState.struct_size = KernelCombatStateDefinition.StructSize;
            if (combatState.weapon_ids == null ||
                combatState.weapon_ids.Length != KernelConstants.MaxWeaponSlots)
            {
                combatState.weapon_ids = new uint[KernelConstants.MaxWeaponSlots];
            }
            if (combatState.ammo == null ||
                combatState.ammo.Length != KernelConstants.MaxWeaponSlots)
            {
                combatState.ammo = new ushort[KernelConstants.MaxWeaponSlots];
            }
            if (combatState.reserve_magazines == null ||
                combatState.reserve_magazines.Length != KernelConstants.MaxWeaponSlots)
            {
                combatState.reserve_magazines = new ushort[KernelConstants.MaxWeaponSlots];
            }
        }

        private static void PrepareWeaponMechanics(
            ref KernelWeaponMechanicsDefinition weaponMechanics)
        {
            weaponMechanics.struct_size = KernelWeaponMechanicsDefinition.StructSize;
        }

        private static void PrepareProjectileMechanics(
            ref KernelProjectileMechanicsDefinition projectileMechanics)
        {
            if (projectileMechanics.struct_size == 0)
            {
                projectileMechanics.struct_size = KernelProjectileMechanicsDefinition.StructSize;
            }
            if (projectileMechanics.motion_model == (byte)KernelProjectileMotionModel.Homing &&
                projectileMechanics.homing.struct_size == 0)
            {
                projectileMechanics.homing.struct_size = KernelHomingMechanicsDefinition.StructSize;
            }
            if (projectileMechanics.projectile_type == (byte)KernelProjectileType.AreaEffect &&
                projectileMechanics.area_effect.struct_size == 0)
            {
                projectileMechanics.area_effect.struct_size =
                    KernelAreaEffectMechanicsDefinition.StructSize;
            }
            if (projectileMechanics.projectile_type == (byte)KernelProjectileType.Beam &&
                projectileMechanics.beam.struct_size == 0)
            {
                projectileMechanics.beam.struct_size = KernelBeamMechanicsDefinition.StructSize;
            }
            PrepareActionTrigger(ref projectileMechanics.projectile_impact_trigger);
            PrepareActionTrigger(ref projectileMechanics.expired_trigger);
        }

        private static void PrepareActionTrigger(ref KernelActionTriggerDefinition trigger)
        {
            if (trigger.struct_size == 0)
            {
                trigger.struct_size = KernelActionTriggerDefinition.StructSize;
            }
            if (trigger.actions == null ||
                trigger.actions.Length != KernelConstants.MaxActionGraphActions)
            {
                trigger.actions = new KernelActionDefinition[
                    KernelConstants.MaxActionGraphActions];
            }
        }

        private static void PrepareItemInstanceViews(KernelItemInstanceView[] views)
        {
            for (int index = 0; index < views.Length; ++index)
            {
                views[index].struct_size = KernelItemInstanceView.StructSize;
                if (views[index].portable_state_fields == null ||
                    views[index].portable_state_fields.Length !=
                        KernelConstants.MaxPortableStateFields)
                {
                    views[index].portable_state_fields =
                        new KernelPortableStateFieldDefinition[
                            KernelConstants.MaxPortableStateFields];
                }
            }
        }

        private static void PrepareInventoryDeltas(KernelInventoryDelta[] deltas)
        {
            for (int index = 0; index < deltas.Length; ++index)
            {
                deltas[index].struct_size = KernelInventoryDelta.StructSize;
                deltas[index].item.struct_size = KernelItemInstanceView.StructSize;
                if (deltas[index].item.portable_state_fields == null ||
                    deltas[index].item.portable_state_fields.Length !=
                        KernelConstants.MaxPortableStateFields)
                {
                    deltas[index].item.portable_state_fields =
                        new KernelPortableStateFieldDefinition[
                            KernelConstants.MaxPortableStateFields];
                }
            }
        }

        private static bool LoadGameplayCatalog(IntPtr kernel, KernelGameplayCatalog catalog)
        {
            KernelActorTemplateDefinition[] actorTemplates =
                catalog.ActorTemplates ?? new KernelActorTemplateDefinition[0];
            KernelProjectileTemplateDefinition[] projectileTemplates =
                catalog.ProjectileTemplates ?? new KernelProjectileTemplateDefinition[0];
            KernelColliderTemplateDefinition[] colliderTemplates =
                catalog.ColliderTemplates ?? new KernelColliderTemplateDefinition[0];
            KernelColliderBindingDefinition[] colliderBindings =
                catalog.ColliderBindings ?? new KernelColliderBindingDefinition[0];
            KernelEntityTemplateDefinition[] entityTemplates =
                catalog.EntityTemplates ?? new KernelEntityTemplateDefinition[0];
            KernelActionTemplateDefinition[] actionTemplates =
                catalog.ActionTemplates ?? new KernelActionTemplateDefinition[0];
            KernelItemTemplateDefinition[] itemTemplates =
                catalog.ItemTemplates ?? new KernelItemTemplateDefinition[0];

            PrepareActorTemplates(actorTemplates);
            PrepareProjectileTemplates(projectileTemplates);
            PrepareColliderTemplates(colliderTemplates);
            PrepareColliderBindings(colliderBindings);
            PrepareEntityTemplates(entityTemplates);
            PrepareActionTemplates(actionTemplates);
            PrepareItemTemplates(itemTemplates);

            GCHandle actorTemplatesHandle = PinArray(actorTemplates, out IntPtr actorTemplatesPtr);
            IntPtr projectileTemplatesPtr = MarshalArray(projectileTemplates);
            GCHandle colliderTemplatesHandle = PinArray(colliderTemplates, out IntPtr colliderTemplatesPtr);
            GCHandle colliderBindingsHandle = PinArray(colliderBindings, out IntPtr colliderBindingsPtr);
            IntPtr entityTemplatesPtr = MarshalArray(entityTemplates);
            GCHandle actionTemplatesHandle = PinArray(actionTemplates, out IntPtr actionTemplatesPtr);
            IntPtr itemTemplatesPtr = MarshalArray(itemTemplates);
            try
            {
                var nativeCatalog = new KernelGameplayCatalogDefinition
                {
                    struct_size = KernelGameplayCatalogDefinition.StructSize,
                    catalog_version = catalog.CatalogVersion,
                    catalog_hash = catalog.CatalogHash,
                    actor_templates = actorTemplatesPtr,
                    actor_template_count = (uint)actorTemplates.Length,
                    projectile_templates = projectileTemplatesPtr,
                    projectile_template_count = (uint)projectileTemplates.Length,
                    collider_templates = colliderTemplatesPtr,
                    collider_template_count = (uint)colliderTemplates.Length,
                    collider_bindings = colliderBindingsPtr,
                    collider_binding_count = (uint)colliderBindings.Length,
                    entity_templates = entityTemplatesPtr,
                    entity_template_count = (uint)entityTemplates.Length,
                    action_templates = actionTemplatesPtr,
                    action_template_count = (uint)actionTemplates.Length,
                    item_templates = itemTemplatesPtr,
                    item_template_count = (uint)itemTemplates.Length,
                };
                return KernelNative.Kernel_LoadGameplayCatalog(
                    kernel,
                    ref nativeCatalog,
                    IntPtr.Zero);
            }
            finally
            {
                FreeIfAllocated(actorTemplatesHandle);
                FreeMarshaledArray<KernelProjectileTemplateDefinition>(
                    projectileTemplatesPtr,
                    projectileTemplates.Length);
                FreeIfAllocated(colliderTemplatesHandle);
                FreeIfAllocated(colliderBindingsHandle);
                FreeMarshaledArray<KernelEntityTemplateDefinition>(
                    entityTemplatesPtr,
                    entityTemplates.Length);
                FreeIfAllocated(actionTemplatesHandle);
                FreeMarshaledArray<KernelItemTemplateDefinition>(
                    itemTemplatesPtr,
                    itemTemplates.Length);
            }
        }

        private static void PrepareActorTemplates(
            KernelActorTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelActorTemplateDefinition.StructSize;
                }
                if (templates[index].vision.struct_size == 0)
                {
                    templates[index].vision.struct_size = KernelAgentVisionConfig.StructSize;
                }
            }
        }

        private static void PrepareProjectileTemplates(
            KernelProjectileTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelProjectileTemplateDefinition.StructSize;
                }
                PrepareProjectileMechanics(ref templates[index].mechanics);
            }
        }

        private static void PrepareEntityTemplates(KernelEntityTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelEntityTemplateDefinition.StructSize;
                }
                PrepareCombatState(ref templates[index].combat);
                if (templates[index].vision.struct_size == 0)
                {
                    templates[index].vision.struct_size = KernelAgentVisionConfig.StructSize;
                }
                if (templates[index].ai.struct_size == 0)
                {
                    templates[index].ai.struct_size = KernelEntityAiDefinition.StructSize;
                }
                if (templates[index].movement.struct_size == 0)
                {
                    templates[index].movement.struct_size = KernelMovementDefinition.StructSize;
                }
                PrepareActionTrigger(ref templates[index].activated_trigger);
                PrepareActionTrigger(ref templates[index].collision_trigger);
                PrepareActionTrigger(ref templates[index].health_depleted_trigger);
                PrepareActionTrigger(ref templates[index].destroy_entity_trigger);
                if (templates[index].prop.struct_size == 0)
                {
                    templates[index].prop.struct_size = KernelPropDefinition.StructSize;
                }
                if (templates[index].prop.interaction.struct_size == 0)
                {
                    templates[index].prop.interaction.struct_size =
                        KernelPropInteractionDefinition.StructSize;
                }
            }
        }

        private static void PrepareItemTemplates(KernelItemTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelItemTemplateDefinition.StructSize;
                }
                if (templates[index].throw_policy.struct_size == 0)
                {
                    templates[index].throw_policy.struct_size =
                        KernelItemThrowDefinition.StructSize;
                }
                if (templates[index].use_policy.struct_size == 0)
                {
                    templates[index].use_policy.struct_size =
                        KernelItemUseDefinition.StructSize;
                }
                if (templates[index].portable_state_fields == null ||
                    templates[index].portable_state_fields.Length !=
                        KernelConstants.MaxPortableStateFields)
                {
                    templates[index].portable_state_fields =
                        new KernelPortableStateFieldDefinition[
                            KernelConstants.MaxPortableStateFields];
                }
                PrepareActionTrigger(ref templates[index].item_used_trigger);
            }
        }

        private static void PrepareColliderTemplates(
            KernelColliderTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelColliderTemplateDefinition.StructSize;
                }
            }
        }

        private static void PrepareActionTemplates(
            KernelActionTemplateDefinition[] templates)
        {
            for (int index = 0; index < templates.Length; ++index)
            {
                if (templates[index].struct_size == 0)
                {
                    templates[index].struct_size = KernelActionTemplateDefinition.StructSize;
                }
            }
        }

        private static void PrepareColliderBindings(
            KernelColliderBindingDefinition[] bindings)
        {
            for (int index = 0; index < bindings.Length; ++index)
            {
                if (bindings[index].struct_size == 0)
                {
                    bindings[index].struct_size = KernelColliderBindingDefinition.StructSize;
                }
            }
        }

        private static GCHandle PinArray<T>(T[] array, out IntPtr pointer)
            where T : struct
        {
            if (array.Length == 0)
            {
                pointer = IntPtr.Zero;
                return default(GCHandle);
            }

            GCHandle handle = GCHandle.Alloc(array, GCHandleType.Pinned);
            pointer = handle.AddrOfPinnedObject();
            return handle;
        }

        private static IntPtr MarshalArray<T>(T[] array)
            where T : struct
        {
            if (array.Length == 0)
            {
                return IntPtr.Zero;
            }

            int elementSize = Marshal.SizeOf<T>();
            IntPtr pointer = Marshal.AllocHGlobal(elementSize * array.Length);
            for (int index = 0; index < array.Length; ++index)
            {
                Marshal.StructureToPtr(
                    array[index],
                    IntPtr.Add(pointer, index * elementSize),
                    false);
            }
            return pointer;
        }

        private static void FreeMarshaledArray<T>(IntPtr pointer, int length)
            where T : struct
        {
            if (pointer == IntPtr.Zero)
            {
                return;
            }

            int elementSize = Marshal.SizeOf<T>();
            for (int index = 0; index < length; ++index)
            {
                Marshal.DestroyStructure<T>(IntPtr.Add(pointer, index * elementSize));
            }
            Marshal.FreeHGlobal(pointer);
        }

        private static void FreeIfAllocated(GCHandle handle)
        {
            if (handle.IsAllocated)
            {
                handle.Free();
            }
        }

        private void Dispose(bool disposing)
        {
            if (handle == IntPtr.Zero)
            {
                return;
            }

            KernelNative.Kernel_Destroy(handle);
            handle = IntPtr.Zero;
        }

        private void ThrowIfDisposed()
        {
            if (handle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(Kernel));
            }
        }

        ~Kernel()
        {
            Dispose(false);
        }
    }
}
