using NetworkExample.Kernel;
using NetworkExample.Kernel.Client;
using UnityEngine;

public sealed class NetworkKernelClientBehaviour : MonoBehaviour
{
    [SerializeField]
    private string address = "127.0.0.1:7777";

    private readonly RenderEntityState[] states = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private NetworkClient client;
    private uint sequence = 1;
    private uint logFrame;

    private void Start()
    {
        client = new NetworkClient();
        if (!client.Start(address))
        {
            Debug.LogError($"Kernel_StartClient failed for {address}.");
            enabled = false;
            return;
        }

        Debug.Log($"Network kernel client connecting to {address}.");
    }

    private void Update()
    {
        if (client == null)
        {
            return;
        }

        uint eventCount = client.Update(Time.deltaTime, events);
        LogKernelEvents(eventCount);

        if (client.IsReady && !client.IsDisconnected)
        {
            SubmitInput();
        }

        uint stateCount = client.GetRenderStates(states);
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
            client_action_time_us = sequence * 33333UL,
            client_action_id = buttons == 0U ? 0U : sequence,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = buttons,
        };
        sequence++;

        client.TrySubmitInput(input);
    }

    private void OnDestroy()
    {
        client?.Dispose();
        client = null;
    }
}
