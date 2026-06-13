using System;

namespace NetworkExample.Kernel.Client
{
    // LAN discovery is a local-network helper only. The native client records
    // LANDiscoveredServer.Ip from the UDP response source address observed by
    // the querying client, not from a server-declared interface field. On a
    // normal single-interface Wi-Fi LAN this is expected to match the server's
    // `ipconfig getifaddr en0` value. On multi-interface, VPN, Ethernet plus
    // Wi-Fi, or routed networks, the OS may select another source interface.
    // Limited broadcast traffic can also be blocked by network or firewall
    // policy. This API does not provide internet reachability, NAT traversal,
    // relay, matchmaking, or MaxPlayers/CurrentPlayers; player limits are
    // pending until native support is added in a later hotfix.
    /// <summary>
    /// Managed wrapper for native Kernel_LANDiscovery_* local-network discovery.
    /// </summary>
    public sealed class LANDiscovery : IDisposable
    {
        private IntPtr handle;

        public LANDiscovery()
        {
            KernelAbi.ValidateNativeAbi();
            handle = KernelNative.Kernel_LANDiscovery_Create();
            if (handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("Kernel_LANDiscovery_Create failed.");
            }
        }

        public bool IsCreated => handle != IntPtr.Zero;

        public bool StartServer(
            ushort serverEndpointPort,
            string serverName = null,
            ushort discoveryPort = KernelConstants.LANDiscoveryDefaultPort)
        {
            ThrowIfDisposed();
            var config = new KernelLANDiscoveryServerConfig
            {
                struct_size = KernelLANDiscoveryServerConfig.StructSize,
                discovery_port = discoveryPort,
                server_endpoint_port = serverEndpointPort,
                server_name = ClampText(serverName),
            };
            return KernelNative.Kernel_LANDiscovery_StartServer(handle, ref config);
        }

        public void StopServer()
        {
            if (handle != IntPtr.Zero)
            {
                KernelNative.Kernel_LANDiscovery_StopServer(handle);
            }
        }

        public bool RefreshServerList(
            ushort discoveryPort = KernelConstants.LANDiscoveryDefaultPort,
            uint timeoutMs = 500)
        {
            ThrowIfDisposed();
            var config = new KernelLANDiscoveryQueryConfig
            {
                struct_size = KernelLANDiscoveryQueryConfig.StructSize,
                discovery_port = discoveryPort,
                timeout_ms = timeoutMs,
            };
            return KernelNative.Kernel_LANDiscovery_Query(handle, ref config);
        }

        public uint GetDiscoveredServers(LANDiscoveredServer[] servers)
        {
            ThrowIfDisposed();
            if (servers == null || servers.Length == 0)
            {
                return 0;
            }

            var nativeResults = new KernelLANDiscoveryResult[servers.Length];
            uint count = KernelNative.Kernel_LANDiscovery_PollResults(
                handle,
                nativeResults,
                (uint)nativeResults.Length);
            int countInt = Math.Min(servers.Length, (int)count);
            for (int index = 0; index < countInt; ++index)
            {
                servers[index] = LANDiscoveredServer.FromNative(nativeResults[index]);
            }
            return count;
        }

        public void ClearResults()
        {
            ThrowIfDisposed();
            KernelNative.Kernel_LANDiscovery_ClearResults(handle);
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        private static string ClampText(string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return string.Empty;
            }

            int maxLength = KernelConstants.LANDiscoveryTextSize - 1;
            return value.Length <= maxLength ? value : value.Substring(0, maxLength);
        }

        private void Dispose(bool disposing)
        {
            if (handle == IntPtr.Zero)
            {
                return;
            }

            KernelNative.Kernel_LANDiscovery_Destroy(handle);
            handle = IntPtr.Zero;
        }

        private void ThrowIfDisposed()
        {
            if (handle == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(LANDiscovery));
            }
        }

        ~LANDiscovery()
        {
            Dispose(false);
        }
    }

    /// <summary>
    /// Server advertisement observed from a LAN discovery response.
    /// </summary>
    public struct LANDiscoveredServer
    {
        public string ServerName;
        /// <summary>
        /// Client-observed UDP response source IP. On a normal single-interface
        /// LAN this is expected to match the server's `ipconfig getifaddr en0`
        /// value, but multi-interface, VPN, Ethernet plus Wi-Fi, routed, or
        /// firewalled networks may produce different or no results.
        /// </summary>
        public string Ip;
        public ushort Port;
        public string ModuleVersion;
        public uint ProtocolVersion;
        public uint SnapshotSchemaVersion;
        public uint PacketSchemaVersion;
        public string GitCommit;
        public bool Compatible;

        internal static LANDiscoveredServer FromNative(KernelLANDiscoveryResult result)
        {
            return new LANDiscoveredServer
            {
                ServerName = result.server_name ?? string.Empty,
                Ip = result.server_endpoint_ip ?? string.Empty,
                Port = result.server_endpoint_port,
                ModuleVersion = result.module_version ?? string.Empty,
                ProtocolVersion = result.protocol_version,
                SnapshotSchemaVersion = result.snapshot_schema_version,
                PacketSchemaVersion = result.packet_schema_version,
                GitCommit = result.git_commit ?? string.Empty,
                Compatible = result.compatible != 0,
            };
        }
    }
}
