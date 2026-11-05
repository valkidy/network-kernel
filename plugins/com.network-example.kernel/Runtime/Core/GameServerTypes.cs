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
        // GameServer_GetTerrainInfo / CopyTerrainHeights / CopyTerrainPlacements /
        // ReleaseTerrain: a generated terrain level's map, for the client to draw.
        // Optional: the queries check it, ValidateNativeAbi does not require it.
        public const ulong CapabilityTerrainQuery = 0x0000000000000080UL;

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

    // A generated terrain level (catalog `terrain:`), as composed when the
    // catalog was loaded into a kernel: the same heights the kernel collides
    // with. Sample (i, j) is at world x = origin_x + i * cell_mm / 1000,
    // z = origin_z + j * cell_mm / 1000; rows run south to north (j = 0 first),
    // columns west to east, kernel world axes (no flip). A collision mesh built
    // from it should split each cell along the (i, j) -> (i + 1, j + 1)
    // diagonal, as the kernel's heightfield does. 48 bytes.
    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerTerrainInfo
    {
        public uint struct_size;
        public uint samples_x;
        public uint samples_z;
        public uint cell_mm;
        public float origin_x;
        public float origin_z;
        public int min_height_cm;
        public int max_height_cm;
        public ulong heights_hash;
        public int seed;
        public uint placement_count;

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerTerrainInfo>();
    }

    // One stamp the recipe placed: its template, its footprint in the world
    // (after turning), the height its border sits at, and how it was turned:
    // quarter turns as TerrainMapComposer.Compose counts them, mirror = the
    // stamp's own x flipped before the turn. 32 bytes.
    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerTerrainPlacement
    {
        public uint struct_size;
        public uint stamp_template_id;
        public float min_x;
        public float min_z;
        public float size_x;
        public float size_z;
        public float base_y;
        public byte turns;
        public byte mirror;
        public byte reserved0;
        public byte reserved1;

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerTerrainPlacement>();
    }
}
