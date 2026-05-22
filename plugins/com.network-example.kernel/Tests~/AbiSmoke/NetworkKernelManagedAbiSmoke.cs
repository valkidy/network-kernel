using System;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Host;

public static class NetworkKernelManagedAbiSmoke
{
    public static int Main()
    {
        try
        {
            KernelAbi.ValidateNativeAbi();
            GameServerAbi.ValidateNativeAbi();
            KernelAbiInfo info = KernelAbi.GetInfo();
            GameServerAbiInfo gameServerInfo = GameServerAbi.GetInfo();
            if ((info.capability_flags & KernelConstants.CapabilityLocalPlayerInfo) == 0)
            {
                throw new InvalidOperationException("Kernel local-player info capability is missing.");
            }
            if ((gameServerInfo.capability_flags &
                 GameServerConstants.CapabilityEnemyManager) == 0)
            {
                throw new InvalidOperationException("Game server enemy-manager capability is missing.");
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
                    client_action_time_us = 33333,
                    client_action_id = 1001,
                    move = new KernelVec2(1.0f, 0.0f),
                    aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
                    buttons = (uint)InputButton.Fire,
                    selected_weapon = 2,
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

                var enemyPosition = new KernelVec3(5.0f, 0.0f, 0.0f);
                var enemyRotation = new KernelQuat(0.0f, 0.0f, 0.0f, 1.0f);
                if (!kernel.ServerSetEntityTransform(
                        enemyNetId,
                        enemyPosition,
                        enemyRotation))
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
                    enemyState.hp != 240 ||
                    enemyState.max_hp != 240 ||
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
                if (!HasRenderEntityHealth(states, stateCount, enemyNetId, 240, 240))
                {
                    throw new InvalidOperationException(
                        "Kernel_GetRenderStates missed enemy health.");
                }
                if (!HasRenderEntityHealth(
                        states,
                        stateCount,
                        localInfo.player_net_id,
                        100,
                        100))
                {
                    throw new InvalidOperationException(
                        "Kernel_GetRenderStates missed local player health.");
                }
                if (!HasProjectileRenderMetadata(states, stateCount, 1, input.client_action_id))
                {
                    throw new InvalidOperationException(
                        "Kernel_GetRenderStates missed projectile sync metadata.");
                }
                uint stateAtTimeCount = kernel.GetRenderStatesAtTime(33333, states);
                if (stateAtTimeCount == 0)
                {
                    throw new InvalidOperationException(
                        "Kernel_GetRenderStatesAtTime returned no states.");
                }

                var events = new KernelEvent[16];
                uint eventCount = kernel.PollEvents(events);
                if (eventCount == 0)
                {
                    throw new InvalidOperationException("Kernel_PollEvents returned no events.");
                }
                if (!HasValidEventTimeline(events, eventCount))
                {
                    throw new InvalidOperationException(
                        "Kernel_PollEvents returned invalid event presentation timestamps.");
                }
                if (!kernel.ServerDestroyEntity(enemyNetId, KernelDespawnReason.Destroyed))
                {
                    throw new InvalidOperationException("Kernel_ServerDestroyEntity failed.");
                }
            }

            using (var host = new NetworkHost())
            {
                if (!host.Start(7778))
                {
                    throw new InvalidOperationException("NetworkHost.Start failed.");
                }

                var hostEvents = new KernelEvent[16];
                host.Update(1.0f / 30.0f, hostEvents);
                if (!host.IsLocalClientReady ||
                    host.LocalPlayerNetId == 0 ||
                    host.EnemyCount != 1)
                {
                    throw new InvalidOperationException("NetworkHost smoke failed.");
                }

                var hostEnemies = new KernelServerEntityState[8];
                uint hostEnemyCount = host.Kernel.ServerQueryEntities(
                    KernelEntityType.Enemy,
                    hostEnemies);
                if (hostEnemyCount != 1)
                {
                    throw new InvalidOperationException("NetworkHost enemy query failed.");
                }

                uint hostCombatEventCount = host.Update(1.0f / 30.0f, hostEvents);
                if (!TryGetFirstEntityHp(
                        host.Kernel,
                        KernelEntityType.Player,
                        out ushort playerHp) ||
                    playerHp >= 100)
                {
                    throw new InvalidOperationException(
                        "NetworkHost enemy did not damage the local player.");
                }
                if (!HasEvent(hostEvents, hostCombatEventCount, KernelEventType.FireConfirmed) ||
                    !HasEvent(hostEvents, hostCombatEventCount, KernelEventType.DamageApplied))
                {
                    throw new InvalidOperationException(
                        "NetworkHost enemy combat events were not observed.");
                }
            }

            Console.WriteLine(
                "Network kernel managed ABI smoke passed: kernel_version={0} kernel_capabilities=0x{1:x16} game_server_version={2} game_server_capabilities=0x{3:x16}",
                info.abi_version,
                info.capability_flags,
                gameServerInfo.abi_version,
                gameServerInfo.capability_flags);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
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

    private static bool HasRenderEntityHealth(
        RenderEntityState[] states,
        uint count,
        uint netId,
        ushort hp,
        ushort maxHp)
    {
        int countInt = (int)count;
        for (int index = 0; index < countInt; ++index)
        {
            if (states[index].net_id == netId)
            {
                return states[index].hp == hp && states[index].max_hp == maxHp;
            }
        }

        return false;
    }

    private static bool TryGetFirstEntityHp(
        Kernel kernel,
        KernelEntityType entityType,
        out ushort hp)
    {
        var states = new KernelServerEntityState[8];
        uint count = kernel.ServerQueryEntities(entityType, states);
        int countInt = (int)count;
        for (int index = 0; index < countInt; ++index)
        {
            if (states[index].valid && states[index].entity_type == entityType)
            {
                hp = states[index].hp;
                return true;
            }
        }

        hp = 0;
        return false;
    }

    private static bool HasEvent(
        KernelEvent[] events,
        uint count,
        KernelEventType eventType)
    {
        int countInt = Math.Min(events.Length, (int)count);
        for (int index = 0; index < countInt; ++index)
        {
            if (events[index].type == eventType)
            {
                return true;
            }
        }

        return false;
    }

    private static bool HasProjectileRenderMetadata(
        RenderEntityState[] states,
        uint count,
        uint ownerPeer,
        uint clientActionId)
    {
        int countInt = (int)count;
        for (int index = 0; index < countInt; ++index)
        {
            RenderEntityState state = states[index];
            if (state.entity_type == KernelEntityType.Projectile &&
                state.owner_peer == ownerPeer &&
                state.client_action_id == clientActionId &&
                state.velocity.x > 0.0f)
            {
                return true;
            }
        }

        return false;
    }

    private static bool HasValidEventTimeline(KernelEvent[] events, uint count)
    {
        int countInt = (int)count;
        for (int index = 0; index < countInt; ++index)
        {
            KernelEvent kernelEvent = events[index];
            if (kernelEvent.presentation_time_us != 0 &&
                (kernelEvent.event_time_us == 0 ||
                 kernelEvent.event_time_us > kernelEvent.presentation_time_us))
            {
                return false;
            }
        }

        return true;
    }
}
