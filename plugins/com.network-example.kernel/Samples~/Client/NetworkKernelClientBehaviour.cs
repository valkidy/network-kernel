using System;
using System.IO;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Client;
using UnityEngine;

public sealed class NetworkKernelClientBehaviour : MonoBehaviour
{
    private enum CatalogStartupMode
    {
        DedicatedServerSync,
        LocalBundleOverride,
    }

    [SerializeField]
    private string address = "127.0.0.1:7777";

    [SerializeField]
    private CatalogStartupMode catalogStartupMode =
        CatalogStartupMode.DedicatedServerSync;

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
    private NetworkClientConnectionState lastConnectionState;

    private const byte RocketWeaponId = 3;

    private void Start()
    {
        LogVersionInfo();

        client = new NetworkClient();
        bool started;
        if (catalogStartupMode == CatalogStartupMode.LocalBundleOverride)
        {
            if (gameplayCatalogBundle == null)
            {
                Debug.LogError(
                    "Local gameplay catalog override mode requires a bundle TextAsset.");
                enabled = false;
                return;
            }
            if (!LoadGameplayCatalogBundle(client))
            {
                enabled = false;
                return;
            }
            started = client.Start(address);
            Debug.LogWarning(
                "Network client using local gameplay catalog override; " +
                "server catalog sync is skipped.");
        }
        else
        {
            started = client.Start(
                address,
                new GameplayCatalogSyncOptions
                {
                    CacheDirectory = Path.Combine(
                        Application.persistentDataPath,
                        "NetworkExample",
                        "GameplayCatalogCache"),
                });
        }

        if (!started)
        {
            Debug.LogError($"Network kernel client startup failed for {address}.");
            enabled = false;
            return;
        }

        lastConnectionState = client.ConnectionState;
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
            $"{result.diagnostic}");
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
        LogConnectionState();

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

    private void LogConnectionState()
    {
        if (client.ConnectionState == lastConnectionState)
        {
            return;
        }

        lastConnectionState = client.ConnectionState;
        GameplayCatalogSyncResult syncResult = client.CatalogSyncResult;
        if (lastConnectionState == NetworkClientConnectionState.Ready)
        {
            Debug.Log(
                $"Gameplay catalog sync ready cache_hit={syncResult.CacheHit} " +
                $"memory_only={syncResult.MemoryOnly} warning={syncResult.CacheWarning}");
        }
        else if (lastConnectionState == NetworkClientConnectionState.Failed)
        {
            Debug.LogError(
                $"Gameplay catalog sync failed error={syncResult.Error}: " +
                $"{syncResult.ErrorMessage}");
        }
        else
        {
            Debug.Log($"Network client state={lastConnectionState}.");
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
        bool firePressed = Input.GetMouseButtonDown(0);
        var input = new KernelPlayerInput
        {
            input_seq = sequence,
            client_action_time_us = clientRenderTimeUs,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = Input.GetMouseButton(1) ? (uint)InputButton.Aim : 0U,
            selected_weapon = RocketWeaponId,
            action_intent = new KernelActionIntent
            {
                action_instance_id = firePressed ? sequence : 0U,
                binding_id = KernelActionBinding.PrimaryFire,
            },
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
