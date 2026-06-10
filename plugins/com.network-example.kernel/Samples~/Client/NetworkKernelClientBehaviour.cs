using System;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Client;
using UnityEngine;

public sealed class NetworkKernelClientBehaviour : MonoBehaviour
{
    [SerializeField]
    private string address = "127.0.0.1:7777";

    [SerializeField]
    private TextAsset gameplayCatalogBundle;

    [SerializeField]
    private string gameplayCatalogEntryPath = "gameplay_catalog.yaml";

    private readonly RenderEntityState[] states = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private NetworkClient client;
    private uint sequence = 1;
    private uint logFrame;
    private ulong clientRenderTimeUs;

    private const byte RocketWeaponId = 3;

    private void Start()
    {
        LogVersionInfo();

        client = new NetworkClient();
        if (gameplayCatalogBundle != null &&
            !LoadGameplayCatalogBundle(client))
        {
            enabled = false;
            return;
        }

        if (!client.Start(address))
        {
            Debug.LogError($"Kernel_StartClient failed for {address}.");
            enabled = false;
            return;
        }

        Debug.Log($"Network kernel client connecting to {address}.");
    }

    private bool LoadGameplayCatalogBundle(NetworkClient networkClient)
    {
        if (networkClient.LoadGameplayCatalogFromMemory(
                gameplayCatalogBundle.bytes,
                gameplayCatalogEntryPath,
                out KernelGameplayCatalogLoadResult result))
        {
            Debug.Log(
                $"Loaded gameplay catalog bundle version={result.catalog_version} " +
                $"hash={result.catalog_hash:x16} projectile_templates={result.projectile_template_count} " +
                $"collider_templates={result.collider_template_count} " +
                $"collider_bindings={result.collider_binding_count}");
            return true;
        }

        Debug.LogError(
            $"Kernel_LoadGameplayCatalogFromMemory failed for '{gameplayCatalogEntryPath}': " +
            $"{result.error_message}");
        return false;
    }

    private static void LogVersionInfo()
    {
        try
        {
            KernelAbiInfo kernelInfo = KernelAbi.GetInfo();
            KernelBuildInfo buildInfo = KernelAbi.GetBuildInfo();
            GameServerAbiInfo gameServerInfo = GameServerAbi.GetInfo();
            Debug.Log(
                $"Network kernel package {NetworkKernelPackageInfo.Name}@{NetworkKernelPackageInfo.Version}: " +
                $"native_version={buildInfo.module_version} git_commit={buildInfo.git_commit} " +
                $"platform={buildInfo.build_platform} config={buildInfo.build_config} " +
                $"kernel_abi={kernelInfo.abi_version} game_server_abi={gameServerInfo.abi_version}");
        }
        catch (Exception exception)
        {
            Debug.LogWarning($"Network kernel version info unavailable: {exception.Message}");
        }
    }

    private void Update()
    {
        if (client == null)
        {
            return;
        }

        float deltaSeconds = Time.deltaTime;
        clientRenderTimeUs += SecondsToMicroseconds(deltaSeconds);

        uint eventCount = client.Update(deltaSeconds, events);
        LogKernelEvents(eventCount);

        if (client.IsReady && !client.IsDisconnected)
        {
            SubmitInput();
        }

        uint stateCount = client.GetRenderStatesAtTime(clientRenderTimeUs, states);
        RenderEntityState localState = default;
        bool foundLocalState = false;
        int stateCountInt = (int)stateCount;
        for (int index = 0; index < stateCountInt; ++index)
        {
            if (states[index].net_id == client.LocalPlayerNetId)
            {
                localState = states[index];
                foundLocalState = true;
                break;
            }
        }

        ++logFrame;
        if (foundLocalState && logFrame % 30U == 0U)
        {
            Debug.Log(
                $"client render states={stateCount} local_net_id={client.LocalPlayerNetId} " +
                $"local_x={localState.position.x:0.00}");
        }
    }

    private void LogKernelEvents(uint eventCount)
    {
        int eventCountInt = (int)eventCount;
        for (int index = 0; index < eventCountInt; ++index)
        {
            KernelEvent kernelEvent = events[index];
            switch (kernelEvent.type)
            {
                case KernelEventType.PlayerJoined:
                    if (client.IsReady)
                    {
                        Debug.Log(
                            $"client ready peer={client.LocalPeerId} local_net_id={client.LocalPlayerNetId}");
                    }
                    else
                    {
                        Debug.Log(
                            $"player joined net_id={kernelEvent.net_id} peer={kernelEvent.peer_id}");
                    }
                    break;
                case KernelEventType.PlayerLeft:
                    Debug.Log(
                        $"player left net_id={kernelEvent.net_id} peer={kernelEvent.peer_id}");
                    break;
                case KernelEventType.Disconnected:
                    Debug.Log($"client disconnected code={kernelEvent.code}");
                    break;
                case KernelEventType.Error:
                    Debug.LogWarning(
                        $"kernel error peer={kernelEvent.peer_id} code={kernelEvent.code}");
                    break;
            }
        }
    }

    private void SubmitInput()
    {
        uint buttons = Input.GetMouseButton(0) ? (uint)InputButton.Fire : 0U;
        var input = new PlayerInput
        {
            input_seq = sequence,
            client_action_time_us = clientRenderTimeUs,
            client_action_id = buttons == 0U ? 0U : sequence,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = buttons,
            selected_weapon = buttons == 0U ? (byte)0 : RocketWeaponId,
        };
        sequence++;

        client.TrySubmitInput(input);
    }

    private static ulong SecondsToMicroseconds(float seconds)
    {
        return seconds <= 0.0f ? 0UL : (ulong)(seconds * 1000000.0f);
    }

    private void OnDestroy()
    {
        client?.Dispose();
        client = null;
    }
}
