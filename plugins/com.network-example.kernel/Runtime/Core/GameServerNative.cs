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
    }
}
