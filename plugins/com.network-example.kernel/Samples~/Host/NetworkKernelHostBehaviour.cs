using NetworkExample.Kernel;
using NetworkExample.Kernel.Host;
using UnityEngine;

public sealed class NetworkKernelHostBehaviour : MonoBehaviour
{
    [SerializeField]
    private ushort port = 7777;

    private readonly RenderEntityState[] states = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private NetworkHost host;
    private uint sequence = 1;
    private uint logFrame;
    private ulong clientRenderTimeUs;

    private const byte RocketWeaponId = 3;

    private void Start()
    {
        host = new NetworkHost();
        if (!host.Start(port))
        {
            Debug.LogError($"NetworkHost failed to listen on port {port}.");
            enabled = false;
            return;
        }

        Debug.Log($"Network host listening on 127.0.0.1:{port}.");
    }

    private void Update()
    {
        if (host == null)
        {
            return;
        }

        float deltaSeconds = Time.deltaTime;
        clientRenderTimeUs += SecondsToMicroseconds(deltaSeconds);

        uint eventCount = host.Update(deltaSeconds, events);
        LogKernelEvents(eventCount);

        if (host.IsLocalClientReady)
        {
            SubmitLocalInput();
        }

        uint stateCount = host.GetRenderStatesAtTime(clientRenderTimeUs, states);
        ++logFrame;
        if (logFrame % 30U == 0U)
        {
            Debug.Log(
                $"host states={stateCount} enemies={host.EnemyCount} " +
                $"local_net_id={host.LocalPlayerNetId}");
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
                    Debug.Log(
                        $"host player joined net_id={kernelEvent.net_id} peer={kernelEvent.peer_id}");
                    break;
                case KernelEventType.EntitySpawned:
                    Debug.Log(
                        $"host entity spawned net_id={kernelEvent.net_id} peer={kernelEvent.peer_id}");
                    break;
                case KernelEventType.EntityDestroyed:
                    Debug.Log($"host entity destroyed net_id={kernelEvent.net_id}");
                    break;
                case KernelEventType.FireConfirmed:
                    Debug.Log(
                        $"host fire confirmed net_id={kernelEvent.net_id} " +
                        $"peer={kernelEvent.peer_id} weapon={kernelEvent.code}");
                    break;
                case KernelEventType.HitConfirmed:
                    Debug.Log(
                        $"host hit confirmed target_net_id={kernelEvent.net_id} " +
                        $"peer={kernelEvent.peer_id} weapon={kernelEvent.code}");
                    break;
                case KernelEventType.DamageApplied:
                    Debug.Log(
                        $"host damage applied target_net_id={kernelEvent.net_id} " +
                        $"peer={kernelEvent.peer_id} damage={kernelEvent.code}");
                    break;
                case KernelEventType.Error:
                    Debug.LogWarning(
                        $"host kernel error peer={kernelEvent.peer_id} code={kernelEvent.code}");
                    break;
            }
        }
    }

    private void SubmitLocalInput()
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

        host.TrySubmitLocalInput(input);
    }

    private static ulong SecondsToMicroseconds(float seconds)
    {
        return seconds <= 0.0f ? 0UL : (ulong)(seconds * 1000000.0f);
    }

    private void OnDestroy()
    {
        host?.Dispose();
        host = null;
    }
}
