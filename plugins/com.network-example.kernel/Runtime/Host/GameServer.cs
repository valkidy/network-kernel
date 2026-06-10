using System;

namespace NetworkExample.Kernel.Host
{
    public sealed class GameServer : IDisposable
    {
        private IntPtr handle;

        public GameServer(Kernel kernel)
            : this(kernel, null)
        {
        }

        public GameServer(Kernel kernel, string templateDirectory)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }

            GameServerAbi.ValidateNativeAbi();
            handle = string.IsNullOrEmpty(templateDirectory)
                ? GameServerNative.GameServer_Create(kernel.Handle)
                : GameServerNative.GameServer_CreateWithWeaponTemplateDirectory(
                    kernel.Handle,
                    templateDirectory);
            if (handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("GameServer_Create failed.");
            }
        }

        public GameServer(
            Kernel kernel,
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult loadResult)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }
            if (bundleBytes == null)
            {
                throw new ArgumentNullException(nameof(bundleBytes));
            }
            if (entryPath == null)
            {
                throw new ArgumentNullException(nameof(entryPath));
            }

            GameServerAbi.ValidateNativeAbi();
            loadResult = new KernelGameplayCatalogLoadResult
            {
                struct_size = KernelGameplayCatalogLoadResult.StructSize,
            };
            handle = GameServerNative.GameServer_CreateWithGameplayCatalogFromMemory(
                kernel.Handle,
                bundleBytes,
                (uint)bundleBytes.Length,
                entryPath,
                ref loadResult);
            if (handle == IntPtr.Zero)
            {
                throw new InvalidOperationException(
                    $"GameServer_CreateWithGameplayCatalogFromMemory failed: {FormatLoadError(loadResult)}");
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

        public bool QueryWeaponTemplate(
            byte weaponId,
            out GameServerWeaponTemplateInfo templateInfo)
        {
            ThrowIfDisposed();
            templateInfo = new GameServerWeaponTemplateInfo
            {
                struct_size = GameServerWeaponTemplateInfo.StructSize,
            };
            return GameServerNative.GameServer_QueryWeaponTemplate(
                handle,
                weaponId,
                ref templateInfo);
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

        private static string FormatLoadError(KernelGameplayCatalogLoadResult loadResult)
        {
            return string.IsNullOrEmpty(loadResult.error_message)
                ? "no native error message"
                : loadResult.error_message;
        }

        ~GameServer()
        {
            Dispose(false);
        }
    }
}
