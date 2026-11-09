using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    internal static class GameServerNative
    {
        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetAbiInfo(
            out GameServerAbiInfo outInfo,
            uint outInfoSize);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr GameServer_Create(IntPtr kernel);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr GameServer_CreateWithWeaponTemplateDirectory(
            IntPtr kernel,
            [MarshalAs(UnmanagedType.LPStr)] string templateDirectory);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr GameServer_CreateWithGameplayCatalogFromMemory(
            IntPtr kernel,
            [In] byte[] bundleBytes,
            uint bundleSize,
            [MarshalAs(UnmanagedType.LPStr)] string entryPath,
            ref KernelGameplayCatalogLoadResult outResult);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void GameServer_Destroy(IntPtr gameServer);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void GameServer_HandleEvent(
            IntPtr gameServer,
            ref KernelEvent kernelEvent);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void GameServer_Tick(IntPtr gameServer, float deltaSeconds);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_GetEnemyCount(IntPtr gameServer);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_QueryWeaponTemplate(
            IntPtr gameServer,
            byte weaponId,
            ref GameServerWeaponTemplateInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void GameServer_DespawnAll(
            IntPtr gameServer,
            uint reason);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetTerrainInfo(
            IntPtr kernel,
            ref GameServerTerrainInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_CopyTerrainHeights(
            IntPtr kernel,
            uint x0,
            uint z0,
            uint width,
            uint depth,
            [Out] short[] outCm,
            uint outCount);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_CopyTerrainPlacements(
            IntPtr kernel,
            [Out] GameServerTerrainPlacement[] outPlacements,
            uint capacity);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void GameServer_ReleaseTerrain(IntPtr kernel);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetStaticSceneMeshInfo(
            IntPtr kernel,
            ref GameServerStaticSceneMeshInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_CopyStaticSceneMesh(
            IntPtr kernel,
            [Out] float[] outPositions,
            uint positionCapacity,
            [Out] uint[] outIndices,
            uint indexCapacity);

        // The scene table, GameServerConstants.CapabilitySceneSlots.
        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_GetSceneTemplateCount(IntPtr kernel);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetSceneTemplate(
            IntPtr kernel,
            uint index,
            ref GameServerSceneTemplateInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_ClientLoadSceneSlot(
            IntPtr kernel,
            [In] byte[] bundleBytes,
            uint bundleSize,
            [MarshalAs(UnmanagedType.LPStr)] string entryPath,
            [MarshalAs(UnmanagedType.LPStr)] string sceneTemplate,
            uint slot,
            uint flags);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_ClientUnloadSceneSlot(
            IntPtr kernel,
            uint slot,
            uint flags);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_ClientAcceptSceneMessage(
            IntPtr kernel,
            [In] ref KernelGameMessage message);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_ClientPumpSceneRecipe(
            IntPtr kernel,
            uint slot,
            uint generation,
            ulong recipeHash,
            float deltaSeconds);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_ClientBeginLoadSceneSlot(
            IntPtr kernel,
            [In] byte[] bundleBytes,
            uint bundleSize,
            [MarshalAs(UnmanagedType.LPStr)] string entryPath,
            [MarshalAs(UnmanagedType.LPStr)] string sceneTemplate,
            uint slot,
            uint generation,
            uint flags);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_ClientPollSceneSlotLoad(
            IntPtr kernel,
            uint slot,
            uint generation);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetSceneTerrainInfo(
            IntPtr kernel,
            uint slot,
            ref GameServerTerrainInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_CopySceneTerrainHeights(
            IntPtr kernel,
            uint slot,
            uint x0,
            uint z0,
            uint width,
            uint depth,
            [Out] short[] outCm,
            uint outCount);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint GameServer_CopySceneTerrainPlacements(
            IntPtr kernel,
            uint slot,
            [Out] GameServerTerrainPlacement[] outPlacements,
            uint capacity);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_GetSceneStaticMeshInfo(
            IntPtr kernel,
            uint slot,
            ref GameServerStaticSceneMeshInfo outInfo);

        [DllImport(KernelNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool GameServer_CopySceneStaticMesh(
            IntPtr kernel,
            uint slot,
            [Out] float[] outPositions,
            uint positionCapacity,
            [Out] uint[] outIndices,
            uint indexCapacity);
    }
}
