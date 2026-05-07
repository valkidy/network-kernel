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

        public uint GetRenderStates(RenderEntityState[] states)
        {
            ThrowIfDisposed();
            if (states == null || states.Length == 0)
            {
                return 0;
            }

            return KernelNative.Kernel_GetRenderStates(handle, states, (uint)states.Length);
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
