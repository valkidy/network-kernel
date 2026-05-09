using System;

namespace NetworkExample.Kernel.Host
{
    public sealed class GameServer : IDisposable
    {
        private IntPtr handle;

        public GameServer(Kernel kernel)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }

            GameServerAbi.ValidateNativeAbi();
            handle = GameServerNative.GameServer_Create(kernel.Handle);
            if (handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("GameServer_Create failed.");
            }
        }

        public bool IsCreated => handle != IntPtr.Zero;

        public uint EnemyCount
        {
            get
            {
                ThrowIfDisposed();
                return GameServerNative.GameServer_GetEnemyCount(handle);
            }
        }

        public void HandleEvent(KernelEvent kernelEvent)
        {
            ThrowIfDisposed();
            GameServerNative.GameServer_HandleEvent(handle, ref kernelEvent);
        }

        public void Tick(float deltaSeconds)
        {
            ThrowIfDisposed();
            GameServerNative.GameServer_Tick(handle, deltaSeconds);
        }

        public void DespawnAll(KernelDespawnReason reason)
        {
            ThrowIfDisposed();
            GameServerNative.GameServer_DespawnAll(handle, (uint)reason);
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        private void Dispose(bool disposing)
        {
            if (handle == IntPtr.Zero)
            {
                return;
            }

            GameServerNative.GameServer_Destroy(handle);
            handle = IntPtr.Zero;
        }

        private void ThrowIfDisposed()
        {
            if (handle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(GameServer));
            }
        }

        ~GameServer()
        {
            Dispose(false);
        }
    }
}
