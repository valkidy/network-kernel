using NetworkExample.Kernel;
using UnityEngine;

public sealed class NetworkKernelSmokeBehaviour : MonoBehaviour
{
    private readonly RenderEntityState[] states = new RenderEntityState[64];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private Kernel kernel;
    private uint sequence = 1;
    private ulong clientRenderTimeUs;

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

        float deltaSeconds = Time.deltaTime;
        clientRenderTimeUs += SecondsToMicroseconds(deltaSeconds);

        uint buttons = Input.GetMouseButton(0) ? (uint)InputButton.Fire : 0U;
        var input = new PlayerInput
        {
            input_seq = sequence,
            client_action_time_us = clientRenderTimeUs,
            client_action_id = buttons == 0U ? 0U : sequence,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = buttons,
        };
        sequence++;

        kernel.SubmitInput(1, input);
        kernel.Update(deltaSeconds);

        uint stateCount = kernel.GetRenderStatesAtTime(clientRenderTimeUs, states);
        uint eventCount = kernel.PollEvents(events);
        Debug.Log($"network kernel smoke states={stateCount} events={eventCount}");
    }

    private static ulong SecondsToMicroseconds(float seconds)
    {
        return seconds <= 0.0f ? 0UL : (ulong)(seconds * 1000000.0f);
    }

    private void OnDestroy()
    {
        kernel?.Dispose();
        kernel = null;
    }
}
