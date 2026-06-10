using System;
using System.Runtime.InteropServices;

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
                       info.has_welcome &&
                       info.connected;
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

        public void Update(float deltaSeconds)
        {
            ThrowIfDisposed();
            KernelNative.Kernel_Update(handle, deltaSeconds);
        }

        public void SubmitInput(uint localPlayerId, PlayerInput input)
        {
            ThrowIfDisposed();
            KernelNative.Kernel_SubmitInput(handle, localPlayerId, ref input);
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

        public bool ServerCreateEntity(
            KernelServerEntityCreateInfo createInfo,
            out uint netId)
        {
            ThrowIfDisposed();
            createInfo.struct_size = KernelServerEntityCreateInfo.StructSize;
            return KernelNative.Kernel_ServerCreateEntity(handle, ref createInfo, out netId);
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

        public bool ServerSubmitEntityInput(uint netId, PlayerInput input)
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

        public bool ServerGetAreaEffectState(uint netId, out KernelAreaEffectState state)
        {
            ThrowIfDisposed();
            state = new KernelAreaEffectState
            {
                struct_size = KernelAreaEffectState.StructSize,
            };
            return KernelNative.Kernel_ServerGetAreaEffectState(handle, netId, ref state);
        }

        public bool ServerGetBeamState(uint netId, out KernelBeamState state)
        {
            ThrowIfDisposed();
            state = new KernelBeamState
            {
                struct_size = KernelBeamState.StructSize,
            };
            return KernelNative.Kernel_ServerGetBeamState(handle, netId, ref state);
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
            if (combatState.ammo == null ||
                combatState.ammo.Length != KernelConstants.MaxWeapons)
            {
                combatState.ammo = new ushort[KernelConstants.MaxWeapons];
            }
            if (combatState.reserve_ammo == null ||
                combatState.reserve_ammo.Length != KernelConstants.MaxWeapons)
            {
                combatState.reserve_ammo = new ushort[KernelConstants.MaxWeapons];
            }
        }

        private static void PrepareWeaponMechanics(
            ref KernelWeaponMechanicsDefinition weaponMechanics)
        {
            weaponMechanics.struct_size = KernelWeaponMechanicsDefinition.StructSize;
        }

        private static bool LoadGameplayCatalog(IntPtr kernel, KernelGameplayCatalog catalog)
        {
            KernelProjectileTemplateDefinition[] projectileTemplates =
                catalog.ProjectileTemplates ?? new KernelProjectileTemplateDefinition[0];
            KernelColliderTemplateDefinition[] colliderTemplates =
                catalog.ColliderTemplates ?? new KernelColliderTemplateDefinition[0];
            KernelColliderBindingDefinition[] colliderBindings =
                catalog.ColliderBindings ?? new KernelColliderBindingDefinition[0];

            PrepareProjectileTemplates(projectileTemplates);
            PrepareColliderTemplates(colliderTemplates);
            PrepareColliderBindings(colliderBindings);

            GCHandle projectileTemplatesHandle = PinArray(projectileTemplates, out IntPtr projectileTemplatesPtr);
            GCHandle colliderTemplatesHandle = PinArray(colliderTemplates, out IntPtr colliderTemplatesPtr);
            GCHandle colliderBindingsHandle = PinArray(colliderBindings, out IntPtr colliderBindingsPtr);
            try
            {
                var nativeCatalog = new KernelGameplayCatalogDefinition
                {
                    struct_size = KernelGameplayCatalogDefinition.StructSize,
                    catalog_version = catalog.CatalogVersion,
                    catalog_hash = catalog.CatalogHash,
                    projectile_templates = projectileTemplatesPtr,
                    projectile_template_count = (uint)projectileTemplates.Length,
                    collider_templates = colliderTemplatesPtr,
                    collider_template_count = (uint)colliderTemplates.Length,
                    collider_bindings = colliderBindingsPtr,
                    collider_binding_count = (uint)colliderBindings.Length,
                };
                return KernelNative.Kernel_LoadGameplayCatalog(kernel, ref nativeCatalog);
            }
            finally
            {
                FreeIfAllocated(projectileTemplatesHandle);
                FreeIfAllocated(colliderTemplatesHandle);
                FreeIfAllocated(colliderBindingsHandle);
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
