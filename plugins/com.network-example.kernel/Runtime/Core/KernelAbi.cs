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
            RequireCapability(
                info,
                KernelConstants.CapabilityLocalPlayerInfo,
                "Kernel local-player info capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityCreate,
                "Kernel server entity create capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityDestroy,
                "Kernel server entity destroy capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityTransformWrite,
                "Kernel server entity transform-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityVelocityWrite,
                "Kernel server entity velocity-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityStateWrite,
                "Kernel server entity state-write capability is missing.");
            RequireCapability(
                info,
                KernelConstants.CapabilityServerEntityQuery,
                "Kernel server entity query capability is missing.");

            RequireSize(nameof(KernelAbiInfo), info.struct_size, Marshal.SizeOf<KernelAbiInfo>());
            RequireSize(nameof(KernelConfig), info.kernel_config_size, Marshal.SizeOf<KernelConfig>());
            RequireSize(nameof(PlayerInput), info.player_input_size, Marshal.SizeOf<PlayerInput>());
            RequireSize(
                nameof(RenderEntityState),
                info.render_entity_state_size,
                Marshal.SizeOf<RenderEntityState>());
            RequireSize(nameof(KernelEvent), info.kernel_event_size, Marshal.SizeOf<KernelEvent>());
            RequireSize(
                nameof(KernelLocalPlayerInfo),
                info.local_player_info_size,
                Marshal.SizeOf<KernelLocalPlayerInfo>());
            RequireSize(
                nameof(KernelServerEntityCreateInfo),
                info.server_entity_create_info_size,
                Marshal.SizeOf<KernelServerEntityCreateInfo>());
            RequireSize(
                nameof(KernelServerEntityState),
                info.server_entity_state_size,
                Marshal.SizeOf<KernelServerEntityState>());
        }

        private static void RequireCapability(
            KernelAbiInfo info,
            ulong capability,
            string message)
        {
            if ((info.capability_flags & capability) == 0)
            {
                throw new InvalidOperationException(message);
            }
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
