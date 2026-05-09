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
        internal static extern void GameServer_DespawnAll(
            IntPtr gameServer,
            uint reason);
    }
}
