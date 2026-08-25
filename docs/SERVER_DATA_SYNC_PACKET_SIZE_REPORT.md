# Server Data Sync Packet Size Report

- Date: 2026-08-25
- Git baseline: `01c3b68`
- Kernel ABI: 82
- Protocol version: 3
- Packet schema: 24
- Snapshot schema: 19
- Gameplay catalog version: 15

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
| Compressed `bundle.zip` | 141,630 B |
| Uncompressed archive payload | 234,969 B |
| ZIP entries, including directories | 122 |
| Jolt static collision artifacts | 74,366 B |
| Recast/Detour navmesh artifacts | 80,612 B |
| 32 KiB sync chunks | 5 |

The bundle size is below the 1 MiB protocol limit. Four chunks carry 32,768 B
each; the final chunk carries 10,558 B. Packet sizes are therefore four
32,840 B messages and one 10,630 B message.

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
| Current final bundle chunk | Server to client | 10,630 B | 10,558 B remaining data |
| Sync error | Server to client | 32 B | Invalid or unavailable catalog |
| Gameplay handshake | Client to server | 178 B | After catalog is current |
| Welcome | Server to client | 64 B | Accepted handshake |

Cache-hit catalog check:

- client to server: 34 B
- server to client: 268 B
- combined: 302 B

Current cache-miss catalog download:

- client to server: `34 + 60 = 94 B`
- server to client: `268 + 141,630 + 5 * 72 = 142,258 B`
- combined: `142,352 B`

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

Which conditional blocks appear is decided per session, not per entity:

- The **owner peer** and **health** blocks are written only for actors whose
  `actor_type` is player.
- The **movement state** block is written only for the receiving session's own
  player. `build_relevant_snapshot` overwrites
  `has_authoritative_movement_state` with `entity.net_id == session.player`, so
  no agent ever carries movement state on the wire, whatever the world holds.
- The **action timeline** block is written for any actor with a non-zero
  `action_template_id` or a non-`None` action phase.

| Snapshot item | Encoded application size |
|---|---:|
| Snapshot base, including four section headers | 60 B |
| Actor base | 52 B |
| Actor rotation block | +16 B |
| Actor owner peer block (player only) | +4 B |
| Actor health block (player only) | +4 B |
| Actor action timeline | +20 B |
| Actor movement state (own player only) | +22 B |
| Own player, idle | 98 B |
| Own player, active action | 118 B |
| **Agent, idle** | **68 B** |
| **Agent, active action** | **88 B** |
| Compact projectile | 34 B |
| Hybrid-correction projectile | 46 B |
| Input packet | 85 B |

The per-client snapshot send budget is 1,200 B. `publish_snapshot` passes
`kLargeSyncPacketWarningBytes`, which is a compile-time constant and is not
exposed through `KernelConfig`. At the default 15 snapshots per second, a
continuously full budget is 18,000 B/s or 144 kbit/s per client before
transport overhead.

Action and movement templates are synchronized once through the catalog.
Runtime action and movement state appears only in the conditional actor blocks
shown above.

## Measured Agent Refresh Rate

Derived budget arithmetic is not sufficient here, because the agent section is
round-robin and the cursor rule decides the refresh period. The numbers below
are measured by driving the real `build_relevant_snapshot` /
`build_snapshot_send_set` pair over a synthetic population and recording, per
agent, how many snapshots pass between appearances.

All agents are placed inside the 40 m relevance sphere, so every agent is
relevant and the measurement isolates send-set selection from range culling.

Idle agents, 1,200 B budget, 15 snapshots per second:

| Agents | Packed per snapshot | Snapshot bytes | Median gap | Max gap | Blackout |
|---:|---:|---:|---:|---:|---:|
| 16 | 15 | 1,178 B | 1 | 2 | 0.13 s |
| 64 | 15 | 1,178 B | 1 | 50 | 3.33 s |
| 128 | 15 | 1,178 B | 1 | 114 | 7.60 s |
| 256 | 15 | 1,178 B | 1 | 242 | 16.13 s |
| 500 | 15 | 1,178 B | 1 | 486 | 32.40 s |

With every agent mid-action (88 B each), 11 agents fit per snapshot and the
blackout at 500 agents is 32.67 s.

The gap distribution is bimodal and a mean describes neither half. An agent is
refreshed on 15 consecutive snapshots — one second of smooth motion — and then
goes dark for the rest of the cycle. The blackout is what a player sees, and it
is `(N - 14) / 15` seconds.

### Why the cycle is N and not N/15

`build_snapshot_send_set` advances the agent cursor by exactly one per
snapshot, regardless of how many agents it packed:

```cpp
const std::size_t start = *cursor % entities.size();
for (std::size_t offset = 0; offset < entities.size(); ++offset) {
    const std::size_t index = (start + offset) % entities.size();
    try_add_entity(*entities[index]);
}
*cursor = (start + 1) % entities.size();
```

The window therefore slides by one rather than stepping to the next disjoint
group, so the cycle length is the population size rather than
`population / packed`. A control run that advances the cursor by the number
actually packed — measured through the same code path, with the cursor
overwritten between calls and no engine change — gives:

| Agents | Blackout, cursor +1 (shipped) | Blackout, cursor +packed (control) |
|---:|---:|---:|
| 64 | 3.33 s | 0.33 s |
| 128 | 7.60 s | 0.60 s |
| 256 | 16.13 s | 1.20 s |
| 500 | 32.40 s | 2.27 s |

The control is a measurement, not a proposal: sliding windows and disjoint
partitions have different behaviour under churn, and nothing here evaluates
that.

### Population ceiling

Bandwidth is unchanged by population — the budget is saturated from about 15
relevant agents upward, and per-client snapshot traffic is a flat 17,670 B/s
(141 kbit/s). What population buys is staleness, not bytes.

The blackout is `(N - K + 1) / 15` seconds, where `K` is the number of agents
that fit in one snapshot: 15 idle, 11 mid-action. Inverting it, for a target
worst-case staleness `S` seconds the ceiling on simultaneously relevant agents
is `N <= 15 * S + K - 1`:

| Worst-case staleness | Idle agents (K=15) | Mid-action agents (K=11) |
|---:|---:|---:|
| 0.5 s | 21 | 17 |
| 1 s | 29 | 25 |
| 2 s | 44 | 40 |
| 5 s | 89 | 85 |

The two columns converge because `K` shifts the intercept, not the slope: past
a couple of seconds of tolerance the ceiling is set almost entirely by the
snapshot rate, and packing more agents per snapshot barely moves it.

## Reproduction

```text
bazel build --config=macos -c opt //game_server/gameplay_catalog_bundle:bundle.zip
stat -f '%z' bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
unzip -l bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:session_packets_test
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:network_packets_test
bazel run -c opt //engine/src/tests/kernel_tests:snapshot_bandwidth_benchmark
```

Regenerate the bundle before recording its size; do not use a stale
`bazel-bin` artifact as a new baseline.

Both protocol tests are red on this baseline for reasons unrelated to size.
`session_packets_test` aborts at its Welcome assertion, which still expects the
pre-`actor_blocking_mode` 60 B packet; every size assertion above it passes and
is the source for the catalog table. `network_packets_test` aborts on a
snapshot field that the codec no longer serialises; its size assertions run
before that abort and pass.
