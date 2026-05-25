using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    internal static class KernelNative
    {
        internal const string LibraryName = "network_kernel";

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetAbiInfo(
            out KernelAbiInfo outInfo,
            uint outInfoSize);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_GetLocalPlayerInfo(
            IntPtr kernel,
            out KernelLocalPlayerInfo outInfo);

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
        internal static extern uint Kernel_GetRenderStatesAtTime(
            IntPtr kernel,
            ulong clientRenderTimeUs,
            [Out] RenderEntityState[] outStates,
            uint maxStates);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_PollEvents(
            IntPtr kernel,
            [Out] KernelEvent[] outEvents,
            uint maxEvents);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerCreateEntity(
            IntPtr kernel,
            ref KernelServerEntityCreateInfo createInfo,
            out uint outNetId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerDestroyEntity(
            IntPtr kernel,
            uint netId,
            uint reason);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityTransform(
            IntPtr kernel,
            uint netId,
            ref KernelVec3 position,
            ref KernelQuat rotation);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityVelocity(
            IntPtr kernel,
            uint netId,
            ref KernelVec3 velocity);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityState(
            IntPtr kernel,
            uint netId,
            ushort animationState,
            uint visualFlags);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSubmitEntityInput(
            IntPtr kernel,
            uint netId,
            ref PlayerInput input);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetEntityState(
            IntPtr kernel,
            uint netId,
            ref KernelServerEntityState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityCombatState(
            IntPtr kernel,
            uint netId,
            ref KernelCombatStateDefinition combatState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerSetEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            ref KernelWeaponMechanicsDefinition weaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerClearEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            byte weaponId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetEntityWeaponMechanics(
            IntPtr kernel,
            uint netId,
            byte weaponId,
            ref KernelWeaponMechanicsDefinition outWeaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetAreaEffectState(
            IntPtr kernel,
            uint netId,
            ref KernelAreaEffectState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetBeamState(
            IntPtr kernel,
            uint netId,
            ref KernelBeamState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerGetHomingState(
            IntPtr kernel,
            uint netId,
            ref KernelHomingState outState);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Kernel_ServerValidateMechanicsConfig(
            ref KernelWeaponMechanicsDefinition weaponMechanics);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint Kernel_ServerQueryEntities(
            IntPtr kernel,
            ushort entityTypeFilter,
            [Out] KernelServerEntityState[] outStates,
            uint maxStates);
    }
}
