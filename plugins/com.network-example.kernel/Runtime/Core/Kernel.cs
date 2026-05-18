using System;

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

        public bool ServerGetEntityState(uint netId, out KernelServerEntityState state)
        {
            ThrowIfDisposed();
            state = new KernelServerEntityState
            {
                struct_size = KernelServerEntityState.StructSize,
            };
            return KernelNative.Kernel_ServerGetEntityState(handle, netId, ref state);
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
