using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class GameServerConstants
    {
        public const uint AbiVersion = 1;

        public const ulong CapabilityEnemyManager = 0x0000000000000001UL;
        public const ulong CapabilityEventHandling = 0x0000000000000002UL;
        public const ulong CapabilityDespawnAll = 0x0000000000000004UL;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerAbiInfo
    {
        public uint struct_size;
        public uint abi_version;
        public ulong capability_flags;
    }
}
