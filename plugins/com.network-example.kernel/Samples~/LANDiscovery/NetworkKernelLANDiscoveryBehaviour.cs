using NetworkExample.Kernel;
using NetworkExample.Kernel.Client;
using UnityEngine;

public sealed class NetworkKernelLANDiscoveryBehaviour : MonoBehaviour
{
    [SerializeField]
    private ushort discoveryPort = KernelConstants.LANDiscoveryDefaultPort;

    [SerializeField]
    private uint timeoutMs = 500;

    private readonly LANDiscoveredServer[] servers = new LANDiscoveredServer[16];
    private LANDiscovery discovery;

    private void Awake()
    {
        discovery = new LANDiscovery();
    }

    public void RefreshServerList()
    {
        if (discovery == null)
        {
            return;
        }

        discovery.ClearResults();
        if (!discovery.RefreshServerList(discoveryPort, timeoutMs))
        {
            Debug.LogWarning("LAN discovery refresh did not start.");
            return;
        }

        Debug.Log($"LAN discovery broadcast sent on UDP port {discoveryPort}.");
    }

    private void Update()
    {
        if (discovery == null)
        {
            return;
        }

        uint count = discovery.GetDiscoveredServers(servers);
        if (count == 0)
        {
            return;
        }

        for (int index = 0; index < count && index < servers.Length; ++index)
        {
            LANDiscoveredServer server = servers[index];
            string endpoint = $"{server.Ip}:{server.Port}";
            Debug.Log(
                $"LAN server name={server.ServerName} endpoint={endpoint} " +
                $"protocol={server.ProtocolVersion} snapshot={server.SnapshotSchemaVersion} " +
                $"packet={server.PacketSchemaVersion} git={server.GitCommit} " +
                $"compatible={server.Compatible}. Connect with NetworkClient.Start(\"{endpoint}\").");
        }
        discovery.ClearResults();
    }

    private void OnDestroy()
    {
        discovery?.Dispose();
        discovery = null;
    }
}
