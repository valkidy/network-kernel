using System;

namespace NetworkExample.Kernel.Client
{
    public sealed class NetworkClient : IDisposable
    {
        private readonly Kernel kernel;
        private bool disconnected;
        private uint localPeerId;
        private uint localPlayerNetId;

        public NetworkClient()
            : this(KernelConfig.CreateDefault(KernelMode.Client))
        {
        }

        public NetworkClient(KernelConfig config)
        {
            config.mode = KernelMode.Client;
            kernel = new Kernel(config);
        }

        public bool IsReady => localPeerId != 0 && localPlayerNetId != 0 && !disconnected;
        public bool IsDisconnected => disconnected;
        public uint LocalPeerId => localPeerId;
        public uint LocalPlayerNetId => localPlayerNetId;
        public Kernel Kernel => kernel;

        public bool Start(string address)
        {
            return kernel.StartClient(address);
        }

        public bool LoadGameplayCatalogFromMemory(
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult result)
        {
            return kernel.LoadGameplayCatalogFromMemory(bundleBytes, entryPath, out result);
        }

        public uint Update(float deltaSeconds, KernelEvent[] events)
        {
            kernel.Update(deltaSeconds);
            uint eventCount = kernel.PollEvents(events);
            ApplyEvents(events, eventCount);
            return eventCount;
        }

        public uint GetRenderStates(RenderEntityState[] states)
        {
            return kernel.GetRenderStates(states);
        }

        public uint GetRenderStatesAtTime(ulong clientRenderTimeUs, RenderEntityState[] states)
        {
            return kernel.GetRenderStatesAtTime(clientRenderTimeUs, states);
        }

        public bool TrySubmitInput(PlayerInput input)
        {
            if (!IsReady)
            {
                return false;
            }

            kernel.SubmitInput(localPeerId, input);
            return true;
        }

        public void Dispose()
        {
            kernel.Dispose();
        }

        private void ApplyEvents(KernelEvent[] events, uint eventCount)
        {
            if (events == null)
            {
                return;
            }

            int count = Math.Min(events.Length, (int)eventCount);
            for (int index = 0; index < count; ++index)
            {
                KernelEvent kernelEvent = events[index];
                if (kernelEvent.type == KernelEventType.PlayerJoined)
                {
                    RefreshLocalPlayerInfo();
                }
                else if (kernelEvent.type == KernelEventType.Disconnected)
                {
                    disconnected = true;
                    localPeerId = 0;
                    localPlayerNetId = 0;
                }
            }
        }

        private void RefreshLocalPlayerInfo()
        {
            if (!kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) ||
                !info.connected)
            {
                return;
            }

            disconnected = false;
            localPeerId = info.peer_id;
            localPlayerNetId = info.player_net_id;
        }
    }
}
