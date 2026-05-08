using System;
using NetworkExample.Kernel;

public static class NetworkKernelManagedAbiSmoke
{
    public static int Main()
    {
        try
        {
            KernelAbi.ValidateNativeAbi();
            KernelAbiInfo info = KernelAbi.GetInfo();
            if ((info.capability_flags & KernelConstants.CapabilityLocalPlayerInfo) == 0)
            {
                throw new InvalidOperationException("Kernel local-player info capability is missing.");
            }

            using (var kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer)))
            {
                if (!kernel.StartListenServer(7777))
                {
                    throw new InvalidOperationException("Kernel_StartListenServer failed.");
                }
                if (!kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo localInfo) ||
                    localInfo.player_net_id == 0 ||
                    kernel.LocalPlayerNetId != localInfo.player_net_id ||
                    !kernel.IsClientReady)
                {
                    throw new InvalidOperationException("Kernel_GetLocalPlayerInfo returned no local player.");
                }

                var input = new PlayerInput
                {
                    input_seq = 1,
                    client_tick = 1,
                    move = new KernelVec2(1.0f, 0.0f),
                    aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
                    buttons = (uint)InputButton.Fire,
                };

                kernel.SubmitInput(1, input);
                kernel.Update(1.0f / 30.0f);

                var states = new RenderEntityState[16];
                uint stateCount = kernel.GetRenderStates(states);
                if (stateCount == 0)
                {
                    throw new InvalidOperationException("Kernel_GetRenderStates returned no states.");
                }

                var events = new KernelEvent[16];
                uint eventCount = kernel.PollEvents(events);
                if (eventCount == 0)
                {
                    throw new InvalidOperationException("Kernel_PollEvents returned no events.");
                }
            }

            Console.WriteLine(
                "Network kernel managed ABI smoke passed: version={0} capabilities=0x{1:x16}",
                info.abi_version,
                info.capability_flags);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
