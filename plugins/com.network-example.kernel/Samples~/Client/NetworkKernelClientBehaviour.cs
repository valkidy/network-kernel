using NetworkExample.Kernel;
using UnityEngine;

public sealed class NetworkKernelClientBehaviour : MonoBehaviour
{
    [SerializeField]
    private string address = "127.0.0.1:7777";

    private readonly RenderEntityState[] states = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private Kernel kernel;
    private uint sequence = 1;
    private bool readyForInput;
    private bool disconnected;
    private uint localPlayerNetId;
    private uint logFrame;

    private void Start()
    {
        kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.Client));
        if (!kernel.StartClient(address))
        {
            Debug.LogError($"Kernel_StartClient failed for {address}.");
            enabled = false;
            return;
        }

        Debug.Log($"Network kernel client connecting to {address}.");
    }

    private void Update()
    {
        if (kernel == null)
        {
            return;
        }

        kernel.Update(Time.deltaTime);
        PollKernelEvents();

        if (readyForInput && !disconnected)
        {
            SubmitInput();
        }

        uint stateCount = kernel.GetRenderStates(states);
        RenderEntityState localState = default;
        bool foundLocalState = false;
        int stateCountInt = (int)stateCount;
        for (int index = 0; index < stateCountInt; ++index)
        {
            if (states[index].net_id == localPlayerNetId)
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
                $"client render states={stateCount} local_net_id={localPlayerNetId} " +
                $"local_x={localState.position.x:0.00}");
        }
    }

    private void PollKernelEvents()
    {
        uint eventCount = kernel.PollEvents(events);
        int eventCountInt = (int)eventCount;
        for (int index = 0; index < eventCountInt; ++index)
        {
            KernelEvent kernelEvent = events[index];
            switch (kernelEvent.type)
            {
                case KernelEventType.PlayerJoined:
                    if (kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) &&
                        info.connected)
                    {
                        readyForInput = true;
                        disconnected = false;
                        localPlayerNetId = info.player_net_id;
                        Debug.Log(
                            $"client ready peer={info.peer_id} local_net_id={localPlayerNetId}");
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
                    readyForInput = false;
                    disconnected = true;
                    localPlayerNetId = 0;
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
        var input = new PlayerInput
        {
            input_seq = sequence,
            client_tick = sequence,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = Input.GetMouseButton(0) ? (uint)InputButton.Fire : 0U,
        };
        sequence++;

        kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info);
        kernel.SubmitInput(info.peer_id, input);
    }

    private void OnDestroy()
    {
        kernel?.Dispose();
        kernel = null;
    }
}
