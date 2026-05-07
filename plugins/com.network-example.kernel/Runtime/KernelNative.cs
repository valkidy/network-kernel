using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    internal static class KernelNative
    {
        private const string LibraryName = "network_kernel";

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetAbiInfo(
            out KernelAbiInfo outInfo,
            uint outInfoSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Kernel_Create(ref KernelConfig config);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_Destroy(IntPtr kernel);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartClient(
            IntPtr kernel,
            [MarshalAs(UnmanagedType.LPStr)] string address);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartListenServer(IntPtr kernel, ushort port);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_StartDedicatedServer(IntPtr kernel, ushort port);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_Update(IntPtr kernel, float deltaSeconds);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Kernel_SubmitInput(
            IntPtr kernel,
            uint localPlayerId,
            ref PlayerInput input);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_GetRenderStates(
            IntPtr kernel,
            [Out] RenderEntityState[] outStates,
            uint maxStates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollEvents(
            IntPtr kernel,
            [Out] KernelEvent[] outEvents,
            uint maxEvents);
    }
}
