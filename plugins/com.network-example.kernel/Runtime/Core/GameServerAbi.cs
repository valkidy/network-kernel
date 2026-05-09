using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class GameServerAbi
    {
        public static GameServerAbiInfo GetInfo()
        {
            if (!GameServerNative.GameServer_GetAbiInfo(
                    out GameServerAbiInfo info,
                    (uint)Marshal.SizeOf<GameServerAbiInfo>()))
            {
                throw new InvalidOperationException("GameServer_GetAbiInfo failed.");
            }

            return info;
        }

        public static void ValidateNativeAbi()
        {
            GameServerAbiInfo info = GetInfo();
            if (info.abi_version != GameServerConstants.AbiVersion)
            {
                throw new InvalidOperationException(
                    $"Unsupported game server ABI version {info.abi_version}; expected {GameServerConstants.AbiVersion}.");
            }
            RequireCapability(
                info,
                GameServerConstants.CapabilityEnemyManager,
                "Game server enemy-manager capability is missing.");
            RequireCapability(
                info,
                GameServerConstants.CapabilityEventHandling,
                "Game server event-handling capability is missing.");
            RequireCapability(
                info,
                GameServerConstants.CapabilityDespawnAll,
                "Game server despawn-all capability is missing.");
            RequireSize(
                nameof(GameServerAbiInfo),
                info.struct_size,
                Marshal.SizeOf<GameServerAbiInfo>());
        }

        private static void RequireCapability(
            GameServerAbiInfo info,
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
