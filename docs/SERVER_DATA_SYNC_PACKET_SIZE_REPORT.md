# Server Data Sync Packet Size Report

Date: 2026-07-13  
Kernel ABI: 38  
Packet/snapshot schema: 14  
Gameplay catalog version: 1

## Scope And Measurement Boundary

This report covers gameplay catalog synchronization, catalog metadata used
during connection setup, and runtime world snapshots. Input, ping/pong, and
non-data-driven combat event traffic are outside this report.

Sizes are encoded application-message bytes, including the kernel's 28-byte
packet header. They exclude GameNetworkingSockets framing, encryption, UDP/IP,
and link-layer overhead. Fixed packet sizes are asserted against encoded byte
vectors in `session_packets_test`; they are not C/C++ `sizeof` values.

## Gameplay Catalog Bundle

The data-driven action migration changes only the compressed pre-handshake
bundle. It does not change a runtime packet layout.

| Bundle | Compressed size | Change |
|---|---:|---:|
| Before independent action YAML | 8,274 B | baseline |
| Current bundle | 10,631 B | +2,357 B (+28.5%) |

The current bundle uses one 32 KiB chunk. The final and only chunk message is
10,703 B: 10,631 B of bundle data plus 72 B of application overhead.

## Catalog Sync Packets

| Packet | Direction | Encoded size | Trigger |
|---|---|---:|---|
| Manifest request | Client to server | 34 B | Catalog sync start |
| Manifest | Server to client | 268 B | Valid manifest request |
| Bundle request | Client to server | 60 B | Cache miss |
| Bundle chunk | Server to client | 72 B + data | Bundle download |
| Maximum bundle chunk | Server to client | 32,840 B | 32 KiB data chunk |
| Sync error | Server to client | 32 B | Invalid or unavailable catalog |
| Gameplay handshake | Client to server | 178 B | After catalog is current |
| Welcome | Server to client | 60 B | Accepted handshake |

Cache hit catalog check:

- client to server: 34 B
- server to client: 268 B
- combined: 302 B

Current cache miss catalog download:

- client to server: 94 B
- server to client: `268 + 10,631 + 72 = 10,971 B`
- combined: 11,065 B across three request/response stages and one data chunk

For bundle size `B`, the server-to-client download is:

```text
268 + B + 72 * ceil(B / 32768)
```

At the 1 MiB bundle limit, 32 chunks produce 1,051,148 B server-to-client;
including the two client requests produces 1,051,242 B combined application
traffic.

## Fragmentation And Packet Splitting

A maximum 32,840-byte chunk is larger than a normal network MTU. The current
transport sends it as a GameNetworkingSockets reliable message, whose message
layer segments and reassembles it below the kernel packet boundary. The
application already bounds the transfer to 32 KiB data chunks and validates
offset, total size, hash, and maximum chunk length.

No additional application-level split is required for the current transport
or 10,631-byte bundle. The report does not claim a UDP datagram size: transport
fragmentation, encryption, retransmission, and IP/link overhead are deliberately
outside the exact byte totals. A transport change with a lower reliable-message
limit must revisit `kGameplayCatalogBundleChunkBytes`.

## Runtime Snapshot Budget

| Snapshot item | Encoded application size |
|---|---:|
| Snapshot base | 60 B |
| Player actor, idle | 76 B |
| Player actor, active action | 96 B |
| Agent actor, idle | 68 B |
| Agent actor, active action | 88 B |
| Compact projectile | 34 B |
| Hybrid-correction projectile | 46 B |

Snapshots are selected against a 1,200 B per-client send budget. At the
default 15 snapshots per second, a continuously full budget is 18,000 B/s or
144 kbit/s per client before transport overhead. Actual traffic depends on
relevance, action activity, projectile sync mode, and round-robin selection.

Action templates are synchronized once through the catalog. Runtime action
state remains the existing 20-byte conditional actor block, so this change
adds no snapshot bytes and does not require runtime snapshot splitting.

## Measurement Commands

```text
bazel --output_base=/private/tmp/bazel-network-example-action-data build --config=macos -c opt //game_server/gameplay_catalog_bundle:bundle
stat -f '%z' bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
unzip -l bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
bazel --output_base=/private/tmp/bazel-network-example-action-data test --config=macos --copt=-Wunused-function -c opt //engine/src/tests/protocol_tests:session_packets_test
```
