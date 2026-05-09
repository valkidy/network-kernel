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
                if (!kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo localInfo) ||
                    localInfo.player_net_id == 0 ||
                    !localInfo.connected)
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

                var createInfo = new KernelServerEntityCreateInfo
                {
                    entity_type = KernelEntityType.Enemy,
                    position = new KernelVec3(6.0f, 0.0f, 0.0f),
                    rotation = new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f),
                    animation_state = 4,
                    visual_flags = 8,
                };
                if (!kernel.ServerCreateEntity(createInfo, out uint enemyNetId) ||
                    enemyNetId == 0)
                {
                    throw new InvalidOperationException("Kernel_ServerCreateEntity failed.");
                }

                var queried = new KernelServerEntityState[16];
                uint queryCount = kernel.ServerQueryEntities(KernelEntityType.Enemy, queried);
                if (!HasServerEntity(queried, queryCount, enemyNetId))
                {
                    throw new InvalidOperationException("Kernel_ServerQueryEntities missed enemy.");
                }

                if (!kernel.ServerSetEntityTransform(
                        enemyNetId,
                        new KernelVec3(5.0f, 0.0f, 0.0f),
                        new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f)))
                {
                    throw new InvalidOperationException("Kernel_ServerSetEntityTransform failed.");
                }
                if (!kernel.ServerSetEntityVelocity(
                        enemyNetId,
                        new KernelVec3(1.0f, 0.0f, 0.0f)))
                {
                    throw new InvalidOperationException("Kernel_ServerSetEntityVelocity failed.");
                }
                if (!kernel.ServerSetEntityState(enemyNetId, 7, 0x12345678U))
                {
                    throw new InvalidOperationException("Kernel_ServerSetEntityState failed.");
                }
                if (!kernel.ServerGetEntityState(
                        enemyNetId,
                        out KernelServerEntityState enemyState) ||
                    !enemyState.valid ||
                    enemyState.entity_type != KernelEntityType.Enemy ||
                    enemyState.animation_state != 7 ||
                    (enemyState.visual_flags & 0x12345678U) != 0x12345678U)
                {
                    throw new InvalidOperationException("Kernel_ServerGetEntityState failed.");
                }

                kernel.Update(0.0f);

                var states = new RenderEntityState[16];
                uint stateCount = kernel.GetRenderStates(states);
                if (stateCount == 0)
                {
                    throw new InvalidOperationException("Kernel_GetRenderStates returned no states.");
                }
                if (!HasRenderEntity(states, stateCount, enemyNetId))
                {
                    throw new InvalidOperationException("Kernel_GetRenderStates missed enemy.");
                }

                var events = new KernelEvent[16];
                uint eventCount = kernel.PollEvents(events);
                if (eventCount == 0)
                {
                    throw new InvalidOperationException("Kernel_PollEvents returned no events.");
                }
                if (!kernel.ServerDestroyEntity(enemyNetId, KernelDespawnReason.Destroyed))
                {
                    throw new InvalidOperationException("Kernel_ServerDestroyEntity failed.");
                }
            }

            Debug.Log("Network kernel ABI smoke passed.");
        }

        private static bool HasServerEntity(
            KernelServerEntityState[] states,
            uint count,
            uint netId)
        {
            int countInt = (int)count;
            for (int index = 0; index < countInt; ++index)
            {
                if (states[index].valid && states[index].net_id == netId)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool HasRenderEntity(
            RenderEntityState[] states,
            uint count,
            uint netId)
        {
            int countInt = (int)count;
            for (int index = 0; index < countInt; ++index)
            {
                if (states[index].net_id == netId)
                {
                    return true;
                }
            }

            return false;
        }
    }
}
