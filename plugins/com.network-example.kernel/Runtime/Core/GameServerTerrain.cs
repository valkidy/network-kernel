using System;

namespace NetworkExample.Kernel
{
    // Queries for a generated terrain level (catalog `terrain:`), keyed by the
    // kernel its catalog was loaded into: NetworkClient.Kernel after catalog
    // sync, or NetworkHost.Kernel on a Host. Loading another catalog into that
    // kernel replaces what these return. Call Release before disposing the
    // kernel to free the native copy.
    public static class GameServerTerrain
    {
        // Whether the native library has the terrain queries at all.
        public static bool IsSupported =>
            (GameServerAbi.GetInfo().capability_flags & GameServerConstants.CapabilityTerrainQuery) != 0UL;

        // False when the kernel's last catalog load had no terrain, or after Release.
        public static bool TryGetInfo(Kernel kernel, out GameServerTerrainInfo info)
        {
            IntPtr handle = RequireSupported(kernel);
            info = new GameServerTerrainInfo { struct_size = GameServerTerrainInfo.StructSize };
            if (GameServerNative.GameServer_GetTerrainInfo(handle, ref info))
            {
                return true;
            }
            info = default;
            return false;
        }

        // A rectangle of heights in cm, rows south first: heightsCm[j * width + i]
        // is sample (x0 + i, z0 + j). False if the kernel has no terrain, the
        // rectangle is not inside the map, or heightsCm is shorter than width * depth.
        public static bool CopyHeights(
            Kernel kernel,
            uint x0,
            uint z0,
            uint width,
            uint depth,
            short[] heightsCm)
        {
            IntPtr handle = RequireSupported(kernel);
            if (heightsCm == null)
            {
                throw new ArgumentNullException(nameof(heightsCm));
            }
            return GameServerNative.GameServer_CopyTerrainHeights(
                handle, x0, z0, width, depth, heightsCm, (uint)heightsCm.Length);
        }

        // The whole heightfield, samples_x * samples_z heights in cm, or null if
        // the kernel has no terrain. A 1440 m map is ~23 M samples (46 MB);
        // prefer CopyHeights by chunk there.
        public static short[] CopyAllHeights(Kernel kernel, out GameServerTerrainInfo info)
        {
            if (!TryGetInfo(kernel, out info))
            {
                return null;
            }
            var heights = new short[(long)info.samples_x * info.samples_z];
            return CopyHeights(kernel, 0U, 0U, info.samples_x, info.samples_z, heights)
                ? heights
                : null;
        }

        // Every stamp the recipe placed; empty if the kernel has no terrain.
        public static GameServerTerrainPlacement[] GetPlacements(Kernel kernel)
        {
            IntPtr handle = RequireSupported(kernel);
            uint count = GameServerNative.GameServer_CopyTerrainPlacements(handle, null, 0U);
            var placements = new GameServerTerrainPlacement[count];
            if (count == 0U)
            {
                return placements;
            }
            uint total = GameServerNative.GameServer_CopyTerrainPlacements(handle, placements, count);
            if (total != count)
            {
                throw new InvalidOperationException(
                    $"Terrain placement count changed while copying ({count} -> {total}).");
            }
            return placements;
        }

        // Frees the native copy of the terrain and of the static scene mesh.
        public static void Release(Kernel kernel)
        {
            IntPtr handle = RequireSupported(kernel);
            GameServerNative.GameServer_ReleaseTerrain(handle);
        }

        // Whether the native library has the static scene mesh queries.
        public static bool IsStaticSceneMeshSupported =>
            (GameServerAbi.GetInfo().capability_flags & GameServerConstants.CapabilityStaticSceneMeshQuery) != 0UL;

        // A mesh level's baked static scene: false when the kernel's last
        // catalog load had none (a terrain level, or no map), or after Release.
        public static bool TryGetStaticSceneMeshInfo(Kernel kernel, out GameServerStaticSceneMeshInfo info)
        {
            IntPtr handle = RequireStaticSceneMeshSupported(kernel);
            info = new GameServerStaticSceneMeshInfo { struct_size = GameServerStaticSceneMeshInfo.StructSize };
            if (GameServerNative.GameServer_GetStaticSceneMeshInfo(handle, ref info))
            {
                return true;
            }
            info = default;
            return false;
        }

        // The whole mesh: positions x, y, z per vertex and three indices per
        // triangle, or false when there is none.
        public static bool TryCopyStaticSceneMesh(
            Kernel kernel,
            out GameServerStaticSceneMeshInfo info,
            out float[] positions,
            out uint[] indices)
        {
            positions = null;
            indices = null;
            if (!TryGetStaticSceneMeshInfo(kernel, out info))
            {
                return false;
            }
            var copiedPositions = new float[(long)info.vertex_count * 3];
            var copiedIndices = new uint[(long)info.triangle_count * 3];
            if (!GameServerNative.GameServer_CopyStaticSceneMesh(
                    kernel.Handle,
                    copiedPositions,
                    (uint)copiedPositions.Length,
                    copiedIndices,
                    (uint)copiedIndices.Length))
            {
                return false;
            }
            positions = copiedPositions;
            indices = copiedIndices;
            return true;
        }

        private static IntPtr RequireStaticSceneMeshSupported(Kernel kernel)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }
            if (!IsStaticSceneMeshSupported)
            {
                throw new NotSupportedException(
                    "The native game server has no static scene mesh queries (GAME_SERVER_CAPABILITY_STATIC_SCENE_MESH_QUERY).");
            }
            return kernel.Handle;
        }

        private static IntPtr RequireSupported(Kernel kernel)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }
            if (!IsSupported)
            {
                throw new NotSupportedException(
                    "The native game server has no terrain queries (GAME_SERVER_CAPABILITY_TERRAIN_QUERY).");
            }
            return kernel.Handle;
        }
    }
}
