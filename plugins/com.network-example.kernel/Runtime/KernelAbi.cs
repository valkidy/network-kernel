using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    public static class KernelAbi
    {
        public static KernelAbiInfo GetInfo()
        {
            if (!KernelNative.Kernel_GetAbiInfo(
                    out KernelAbiInfo info,
                    (uint)Marshal.SizeOf<KernelAbiInfo>()))
            {
                throw new InvalidOperationException("Kernel_GetAbiInfo failed.");
            }

            return info;
        }

        public static void ValidateNativeAbi()
        {
            KernelAbiInfo info = GetInfo();
            if (info.abi_version != KernelConstants.AbiVersion)
            {
                throw new InvalidOperationException(
                    $"Unsupported kernel ABI version {info.abi_version}; expected {KernelConstants.AbiVersion}.");
            }

            RequireSize(nameof(KernelAbiInfo), info.struct_size, Marshal.SizeOf<KernelAbiInfo>());
            RequireSize(nameof(KernelConfig), info.kernel_config_size, Marshal.SizeOf<KernelConfig>());
            RequireSize(nameof(PlayerInput), info.player_input_size, Marshal.SizeOf<PlayerInput>());
            RequireSize(
                nameof(RenderEntityState),
                info.render_entity_state_size,
                Marshal.SizeOf<RenderEntityState>());
            RequireSize(nameof(KernelEvent), info.kernel_event_size, Marshal.SizeOf<KernelEvent>());
        }

        private static void RequireSize(string typeName, uint nativeSize, int managedSize)
        {
            if (nativeSize != managedSize)
            {
                throw new InvalidOperationException(
                    $"{typeName} ABI size mismatch: native={nativeSize}, managed={managedSize}.");
            }
        }
    }
}
