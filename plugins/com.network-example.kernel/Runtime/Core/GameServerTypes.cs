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
        // GameServer_GetStaticSceneMeshInfo / CopyStaticSceneMesh: a mesh
        // level's baked static collision scene as triangles, to draw. Optional,
        // like the terrain queries.
        public const ulong CapabilityStaticSceneMeshQuery = 0x0000000000000100UL;
        // The scene table: the SCENE_* and OPEN_UI messages below and
        // GameServerSceneTable. Optional, like the terrain queries.
        public const ulong CapabilitySceneSlots = 0x0000000000000200UL;

        // KernelGameMessage.message_type values game_server sends and reads.
        // Bodies are little-endian and packed; see game_server_types.h.
        public const uint MessageLoadoutOffers = 1;   // server -> client
        public const uint MessageLoadoutSelect = 2;   // client -> server
        public const uint MessageLoadoutResult = 3;   // server -> client
        public const uint MessageSceneState = 4;      // server -> client
        public const uint MessageSceneReady = 5;      // client -> server
        public const uint MessageSceneSelect = 6;     // client -> server
        public const uint MessageSceneAbort = 7;      // client -> server
        public const uint MessageSceneRecipeChunk = 8;    // server -> client
        public const uint MessageSceneRecipeRequest = 9;  // client -> server
        public const uint MessageSceneLoadFailed = 10;    // client -> server
        public const uint MessageOpenUi = 11;         // server -> client

        // The interface a prop's open_ui names when it means the level list.
        public const uint UiLevelSelect = 4;

        // OPEN_UI.result.
        public const byte OpenUiOk = 0;
        public const byte OpenUiNotOwner = 1;
        public const byte OpenUiBusy = 2;

        // Where a listed slot's map comes from.
        public const byte SceneSourceBundle = 0;
        public const byte SceneSourceRecipe = 1;

        // ClientPumpSceneRecipe and ClientPollSceneSlotLoad.
        public const uint SceneLoadPending = 0;
        public const uint SceneLoadReady = 1;
        public const uint SceneLoadFailed = 2;

        public const byte SceneFlowCamp = 0;
        public const byte SceneFlowLoading = 1;
        public const byte SceneFlowGateOpen = 2;
        public const byte SceneFlowReturnOpen = 3;

        public const byte SceneOutcomeNone = 0;
        public const byte SceneOutcomeCompleted = 1;
        public const byte SceneOutcomeFailed = 2;
        public const byte SceneOutcomeLoadFailed = 3;  // the level never loaded
        public const byte SceneOutcomeAborted = 4;     // owner ended it, nobody came, or everyone left

        // Scene load flags. A pure client sets RegisterCollision; a listen host
        // does not, because its server side already added the slot.
        public const uint SceneLoadRegisterCollision = 0x1;
        public const uint SceneLoadFromRecipe = 0x2;  // use the recipe PumpSceneRecipe fetched

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

    // One level a catalog offers. The text fields are UTF-8; read them through
    // Name, DisplayName and Description. 364 bytes.
    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerSceneTemplateInfo
    {
        public uint struct_size;
        public uint template_id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 33)]
        public byte[] name;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public byte[] display_name;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public byte[] description;

        public string Name => Text(name);
        public string DisplayName => Text(display_name);
        public string Description => Text(description);

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerSceneTemplateInfo>();

        private static string Text(byte[] bytes)
        {
            if (bytes == null)
            {
                return string.Empty;
            }
            int length = System.Array.IndexOf(bytes, (byte)0);
            return System.Text.Encoding.UTF8.GetString(bytes, 0, length < 0 ? bytes.Length : length);
        }
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

    // A mesh level's static collision scene (catalog static_collision_scene
    // entry_path, a baked .joltmesh) as the kernel collides with it: welded
    // vertices in world space (kernel axes = Unity axes, no flip) and three
    // indices per triangle in Unity's front-face order. Bounds are of the
    // vertices. 36 bytes.
    [StructLayout(LayoutKind.Sequential)]
    public struct GameServerStaticSceneMeshInfo
    {
        public uint struct_size;
        public uint vertex_count;
        public uint triangle_count;
        public float min_x;
        public float min_y;
        public float min_z;
        public float max_x;
        public float max_y;
        public float max_z;

        public static uint StructSize => (uint)Marshal.SizeOf<GameServerStaticSceneMeshInfo>();
    }
}
