using NetworkExample.Kernel;
using UnityEngine;

public sealed class NetworkKernelSmokeBehaviour : MonoBehaviour
{
    private readonly RenderEntityState[] states = new RenderEntityState[64];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private Kernel kernel;
    private uint sequence = 1;

    private void Start()
    {
        kernel = new Kernel(KernelConfig.CreateDefault(KernelMode.ListenServer));
        if (!kernel.StartListenServer(7777))
        {
            Debug.LogError("Kernel_StartListenServer failed.");
            enabled = false;
        }
    }

    private void Update()
    {
        if (kernel == null)
        {
            return;
        }

        var input = new PlayerInput
        {
            input_seq = sequence,
            client_tick = sequence,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = Input.GetMouseButton(0) ? (uint)InputButton.Fire : 0U,
        };
        sequence++;

        kernel.SubmitInput(1, input);
        kernel.Update(1.0f / 30.0f);

        uint stateCount = kernel.GetRenderStates(states);
        uint eventCount = kernel.PollEvents(events);
        Debug.Log($"network kernel smoke states={stateCount} events={eventCount}");
    }

    private void OnDestroy()
    {
        kernel?.Dispose();
        kernel = null;
    }
}
