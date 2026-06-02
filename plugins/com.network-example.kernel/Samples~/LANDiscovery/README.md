# Network Kernel LAN Discovery Sample

Add `NetworkKernelLANDiscoveryBehaviour` to a GameObject, then bind a Unity UI
Button click to `RefreshServerList`.

The sample sends a LAN discovery broadcast and logs any discovered servers. Use
the logged endpoint with direct-IP connection code such as:

```csharp
client.Start($"{server.Ip}:{server.Port}");
```

`server.Ip` is the client-observed UDP response source address. On a normal
single-interface Wi-Fi LAN, this is expected to match the server Mac's
`ipconfig getifaddr en0` value. On multi-interface, VPN, Ethernet plus Wi-Fi,
or routed networks, the OS may choose a different source interface.

Discovery is LAN-only. It does not provide matchmaking, relay, NAT traversal,
internet reachability, automatic connection, or max-player reporting.
