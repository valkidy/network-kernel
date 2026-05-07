using System;
using UnityEditor;
using UnityEngine;

namespace NetworkExample.Kernel.Editor
{
    public static class NetworkKernelAbiSmokeRunner
    {
        [MenuItem("Network Example/Run Kernel ABI Smoke")]
        public static void Run()
        {
            KernelAbi.ValidateNativeAbi();

            using (var kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer)))
            {
                if (!kernel.StartListenServer(7777))
                {
                    throw new InvalidOperationException("Kernel_StartListenServer failed.");
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

            Debug.Log("Network kernel ABI smoke passed.");
        }
    }
}
