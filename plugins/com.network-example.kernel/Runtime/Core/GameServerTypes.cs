using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class GameServerConstants
    {
        public const uint AbiVersion = 2;

        public const ulong CapabilityEnemyManager = 0x0000000000000001UL;
        public const ulong CapabilityEventHandling = 0x0000000000000002UL;
        public const ulong CapabilityDespawnAll = 0x0000000000000004UL;
        public const ulong CapabilityWeaponTemplateDirectory = 0x0000000000000008UL;
        public const ulong CapabilityWeaponTemplateQuery = 0x0000000000000010UL;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerAbiInfo
    {
        public uint struct_size;
        public uint abi_version;
        public uint weapon_template_info_size;
        public ulong capability_flags;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct GameServerWeaponTemplateInfo
    {
        public uint struct_size;
        public byte weapon_id;
        public byte fire_mode;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string name;
        public KernelWeaponMechanicsDefinition mechanics;
        [MarshalAs(UnmanagedType.I1)]
        public bool valid;

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerWeaponTemplateInfo>();
    }
}
