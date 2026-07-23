# Server Data Sync Packet Size Report

- Date: 2026-07-23
- Git baseline: `0515cee`
- Kernel ABI: 45
- Protocol version: 1
- Packet schema: 17
- Snapshot schema: 15
- Gameplay catalog version: 2

## Scope And Measurement Boundary

This report covers gameplay catalog synchronization, catalog metadata used
during connection setup, and runtime world snapshots. Input size is included as
a reference. Ping/pong, reliable lifecycle events, local action results, remote
action presentation, transport framing, encryption, retransmission, UDP/IP, and
link-layer overhead are outside the totals.

All packet sizes are encoded application-message bytes, including the 28-byte
kernel packet header. Fixed sizes are asserted against encoded byte vectors in
the protocol tests; they are not C/C++ `sizeof` values.

## Current Gameplay Bundle

The Bazel-generated bundle contains gameplay YAML plus cooked Jolt and
Recast/Detour artifacts.

| Measurement | Current value |
|---|---:|
| Compressed `bundle.zip` | 98,452 B |
| Uncompressed archive payload | 155,900 B |
| ZIP entries, including directories | 48 |
| Jolt static collision artifact | 74,145 B |
| Recast/Detour navmesh artifact | 70,372 B |
| 32 KiB sync chunks | 4 |

The bundle size is below the 1 MiB protocol limit. Three chunks carry 32,768 B
each; the final chunk carries 148 B. Packet sizes are therefore three 32,840 B
messages and one 220 B message.

Bundle bytes change when YAML or cooked geometry changes. This report is a
point-in-time measurement and must be regenerated after either input changes.

## Catalog Sync Packets

| Packet | Direction | Encoded size | Trigger |
|---|---|---:|---|
| Manifest request | Client to server | 34 B | Catalog sync start |
| Manifest | Server to client | 268 B | Valid manifest request |
| Bundle request | Client to server | 60 B | Cache miss |
| Bundle chunk | Server to client | 72 B + data | Bundle download |
| Maximum bundle chunk | Server to client | 32,840 B | 32 KiB data chunk |
| Current final bundle chunk | Server to client | 220 B | 148 B remaining data |
| Sync error | Server to client | 32 B | Invalid or unavailable catalog |
| Gameplay handshake | Client to server | 178 B | After catalog is current |
| Welcome | Server to client | 60 B | Accepted handshake |

Cache-hit catalog check:

- client to server: 34 B
- server to client: 268 B
- combined: 302 B

Current cache-miss catalog download:

- client to server: `34 + 60 = 94 B`
- server to client: `268 + 98,452 + 4 * 72 = 99,008 B`
- combined: `99,102 B`

For a bundle of size `B`, the server-to-client download is:

```text
268 + B + 72 * ceil(B / 32768)
```

At the 1 MiB limit, 32 chunks produce 1,051,148 B server-to-client. Including
the manifest and bundle requests produces 1,051,242 B combined application
traffic.

## Fragmentation And Packet Splitting

A maximum 32,840-byte chunk is larger than a normal network MTU. The current
transport sends it as a reliable GameNetworkingSockets message, which segments
and reassembles it below the kernel packet boundary.

The application already:

- limits chunk data to 32 KiB;
- validates bundle hash, offset, total size, and chunk length;
- limits the complete compressed bundle to 1 MiB.

No additional application split is required for the current transport. A
transport with a lower reliable-message limit must revisit
`kGameplayCatalogBundleChunkBytes`.

## Runtime Snapshot Budget

Snapshot records are sectioned and conditional. Actor sizes must be calculated
from the fields present rather than from entity type alone.

| Snapshot item | Encoded application size |
|---|---:|
| Snapshot base, including four section headers | 60 B |
| Actor base plus rotation | 68 B |
| Actor owner peer block | +4 B |
| Actor health block | +4 B |
| Actor action timeline | +20 B |
| Actor movement state | +22 B |
| Example player, idle with owner/health/movement | 98 B |
| Example player, active action with owner/health/movement | 118 B |
| Example agent, idle with movement | 90 B |
| Example agent, active action with movement | 110 B |
| Compact projectile | 34 B |
| Hybrid-correction projectile | 46 B |
| Input packet | 85 B |

The per-client snapshot send budget is 1,200 B. At the default 15 snapshots per
second, a continuously full budget is 18,000 B/s or 144 kbit/s per client before
transport overhead. Actual traffic depends on relevance, conditional actor
blocks, projectile sync mode, and round-robin selection.

Action and movement templates are synchronized once through the catalog.
Runtime action and movement state appears only in the conditional actor blocks
shown above.

## Reproduction

```text
bazel build --config=macos -c opt //game_server/gameplay_catalog_bundle:bundle.zip
stat -f '%z' bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
unzip -l bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:session_packets_test
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:network_packets_test
```

Regenerate the bundle before recording its size; do not use a stale
`bazel-bin` artifact as a new baseline.
