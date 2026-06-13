namespace NetworkExample.Kernel.Host
{
    public sealed class HostLocalClientBridge
    {
        private uint localPeerId;
        private uint localPlayerNetId;
        private bool disconnected;

        public bool IsReady => localPeerId != 0 && localPlayerNetId != 0 && !disconnected;
        public bool IsDisconnected => disconnected;
        public uint LocalPeerId => localPeerId;
        public uint LocalPlayerNetId => localPlayerNetId;

        public void ApplyEvent(Kernel kernel, KernelEvent kernelEvent)
        {
            if (kernelEvent.type == KernelEventType.PlayerJoined ||
                kernelEvent.type == KernelEventType.Connected)
            {
                Refresh(kernel);
                return;
            }

            if (kernelEvent.type == KernelEventType.Disconnected)
            {
                disconnected = true;
                localPeerId = 0;
                localPlayerNetId = 0;
            }
        }

        public bool TrySubmitInput(Kernel kernel, PlayerInput input)
        {
            if (!IsReady)
            {
                Refresh(kernel);
            }
            if (!IsReady)
            {
                return false;
            }

            kernel.SubmitInput(localPeerId, input);
            return true;
        }

        public void Reset()
        {
            localPeerId = 0;
            localPlayerNetId = 0;
            disconnected = false;
        }

        private void Refresh(Kernel kernel)
        {
            if (kernel == null ||
                !kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) ||
                info.connected == 0)
            {
                return;
            }

            disconnected = false;
            localPeerId = info.peer_id;
            localPlayerNetId = info.player_net_id;
        }
    }
}
