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

        bool firePressed = Input.GetMouseButtonDown(0);
        var input = new KernelPlayerInput
        {
            input_seq = sequence,
            client_action_time_us = clientRenderTimeUs,
            move = new KernelVec2(Input.GetAxisRaw("Horizontal"), Input.GetAxisRaw("Vertical")),
            aim_dir = new KernelVec3(1.0f, 0.0f, 0.0f),
            buttons = Input.GetMouseButton(1) ? (uint)InputButton.Aim : 0U,
            action_intent = new KernelActionIntent
            {
                action_instance_id = firePressed ? sequence : 0U,
                binding_id = KernelActionBinding.PrimaryFire,
            },
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
