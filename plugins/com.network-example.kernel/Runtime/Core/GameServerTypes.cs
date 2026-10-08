using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class GameServerConstants
    {
        public const uint AbiVersion = 5;

        public const ulong CapabilityEnemyManager = 0x0000000000000001UL;
        public const ulong CapabilityEventHandling = 0x0000000000000002UL;
        public const ulong CapabilityDespawnAll = 0x0000000000000004UL;
        public const ulong CapabilityWeaponTemplateDirectory = 0x0000000000000008UL;
        public const ulong CapabilityWeaponTemplateQuery = 0x0000000000000010UL;
        public const ulong CapabilityGameplayCatalogBundle = 0x0000000000000020UL;
        // The loadout game messages below (initial camp, ui_id 2).
        public const ulong CapabilityLoadoutMessages = 0x0000000000000040UL;

        // KernelGameMessage.message_type values game_server sends and reads.
        // Bodies are little-endian and packed; see game_server_types.h.
        public const uint MessageLoadoutOffers = 1;   // server -> client
        public const uint MessageLoadoutSelect = 2;   // client -> server
        public const uint MessageLoadoutResult = 3;   // server -> client

        // LOADOUT_RESULT.result.
        public const byte LoadoutResultApplied = 0;
        public const byte LoadoutResultMalformed = 1;
        public const byte LoadoutResultNotACamp = 2;
        public const byte LoadoutResultOutOfRange = 3;
        public const byte LoadoutResultTooManyPicks = 4;
        public const byte LoadoutResultBadOption = 5;
        public const byte LoadoutResultDead = 6;
        public const byte LoadoutResultApplyFailed = 7;
        public const byte LoadoutResultCategoryTaken = 8;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerAbiInfo
    {
        public uint struct_size;
        public uint abi_version;
        public uint weapon_template_info_size;
        public uint gameplay_catalog_load_result_size;
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
        public uint valid;

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerWeaponTemplateInfo>();
    }
}
