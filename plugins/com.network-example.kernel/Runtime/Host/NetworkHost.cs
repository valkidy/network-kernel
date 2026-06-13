using System;

namespace NetworkExample.Kernel.Host
{
    public sealed class NetworkHost : IDisposable
    {
        private readonly HostLocalClientBridge localClient = new HostLocalClientBridge();
        private Kernel kernel;
        private GameServer gameServer;

        public bool IsRunning => kernel != null && gameServer != null;
        public bool IsLocalClientReady => localClient.IsReady;
        public uint LocalPeerId => localClient.LocalPeerId;
        public uint LocalPlayerNetId => localClient.LocalPlayerNetId;
        public uint EnemyCount => gameServer == null ? 0U : gameServer.EnemyCount;
        public Kernel Kernel => kernel;
        public GameServer GameServer => gameServer;
        public HostLocalClientBridge LocalClient => localClient;

        public bool Start(ushort port)
        {
            return Start(port, KernelConfig.CreateDefault(KernelMode.ListenServer));
        }

        public bool Start(ushort port, KernelConfig config)
        {
            if (IsRunning)
            {
                throw new InvalidOperationException("NetworkHost is already running.");
            }

            config.mode = KernelMode.ListenServer;
            Kernel newKernel = null;
            GameServer newGameServer = null;
            try
            {
                newKernel = new Kernel(config);
                if (!newKernel.StartListenServer(port))
                {
                    newKernel.Dispose();
                    return false;
                }

                newGameServer = new GameServer(newKernel);
                kernel = newKernel;
                gameServer = newGameServer;
                localClient.Reset();
                if (kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) &&
                    info.connected != 0)
                {
                    localClient.ApplyEvent(
                        kernel,
                        new KernelEvent
                        {
                            type = KernelEventType.PlayerJoined,
                            net_id = info.player_net_id,
                            peer_id = info.peer_id,
                        });
                }
                return true;
            }
            catch
            {
                newGameServer?.Dispose();
                newKernel?.Dispose();
                throw;
            }
        }

        public bool Start(
            ushort port,
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult loadResult)
        {
            return Start(
                port,
                KernelConfig.CreateDefault(KernelMode.ListenServer),
                bundleBytes,
                entryPath,
                out loadResult);
        }

        public bool Start(
            ushort port,
            KernelConfig config,
            byte[] bundleBytes,
            string entryPath,
            out KernelGameplayCatalogLoadResult loadResult)
        {
            if (IsRunning)
            {
                throw new InvalidOperationException("NetworkHost is already running.");
            }
            if (bundleBytes == null)
            {
                throw new ArgumentNullException(nameof(bundleBytes));
            }
            if (entryPath == null)
            {
                throw new ArgumentNullException(nameof(entryPath));
            }

            config.mode = KernelMode.ListenServer;
            Kernel newKernel = null;
            GameServer newGameServer = null;
            loadResult = default;
            try
            {
                newKernel = new Kernel(config);
                newGameServer = new GameServer(newKernel, bundleBytes, entryPath, out loadResult);
                if (!newKernel.StartListenServer(port))
                {
                    newGameServer.Dispose();
                    newKernel.Dispose();
                    return false;
                }

                kernel = newKernel;
                gameServer = newGameServer;
                localClient.Reset();
                if (kernel.TryGetLocalPlayerInfo(out KernelLocalPlayerInfo info) &&
                    info.connected != 0)
                {
                    localClient.ApplyEvent(
                        kernel,
                        new KernelEvent
                        {
                            type = KernelEventType.PlayerJoined,
                            net_id = info.player_net_id,
                            peer_id = info.peer_id,
                        });
                }
                return true;
            }
            catch
            {
                newGameServer?.Dispose();
                newKernel?.Dispose();
                throw;
            }
        }

        public uint Update(float deltaSeconds, KernelEvent[] events)
        {
            ThrowIfNotRunning();
            kernel.Update(deltaSeconds);
            uint eventCount = kernel.PollEvents(events);
            int count = events == null ? 0 : Math.Min(events.Length, (int)eventCount);
            for (int index = 0; index < count; ++index)
            {
                KernelEvent kernelEvent = events[index];
                localClient.ApplyEvent(kernel, kernelEvent);
                gameServer.HandleEvent(kernelEvent);
            }

            gameServer.Tick(deltaSeconds);
            return eventCount;
        }

        public uint GetRenderStates(RenderEntityState[] states)
        {
            ThrowIfNotRunning();
            return kernel.GetRenderStates(states);
        }

        public uint GetRenderStatesAtTime(ulong clientRenderTimeUs, RenderEntityState[] states)
        {
            ThrowIfNotRunning();
            return kernel.GetRenderStatesAtTime(clientRenderTimeUs, states);
        }

        public bool TrySubmitLocalInput(PlayerInput input)
        {
            ThrowIfNotRunning();
            return localClient.TrySubmitInput(kernel, input);
        }

        public void DespawnAll(KernelDespawnReason reason)
        {
            ThrowIfNotRunning();
            gameServer.DespawnAll(reason);
        }

        public void Dispose()
        {
            gameServer?.Dispose();
            gameServer = null;
            kernel?.Dispose();
            kernel = null;
            localClient.Reset();
        }

        private void ThrowIfNotRunning()
        {
            if (!IsRunning)
            {
                throw new InvalidOperationException("NetworkHost is not running.");
            }
        }
    }
}
