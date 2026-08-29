# Server Data Sync Packet Size Report

- Date: 2026-08-25
- Git baseline: `01c3b68`
- Kernel ABI: 82
- Protocol version: 3
- Packet schema: 24
- Snapshot schema: 20
- Gameplay catalog version: 15

The catalog measurements are from that baseline. The snapshot budget table and
**Measured Agent Refresh Rate** below were re-measured after two changes: the
agent and projectile sections stopped rotating on a sliding index, and agents
moved onto a record of their own. The send budget and the snapshot rate are
unchanged.

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

Agents ride a record of their own rather than the shared actor one. An agent is
never the receiving session's own player, which removes most of what the actor
record spends its bytes on, and it stands on the ground, so its facing is a
quaternion with one live axis. The agent record carries its type and actor type
in the section header rather than per entity, a `u16` turn in place of the
quaternion and again in place of the aim vector, velocity as three `i16` at
1/256 m/s, and single-byte-or-`u16` state and flags. Position is still three
floats — see **Position Is Still Three Floats** below, which is a decision
rather than an omission.

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
| Snapshot base, including five section headers | 64 B |
| Actor base | 52 B |
| Actor rotation block | +16 B |
| Actor owner peer block (player only) | +4 B |
| Actor health block (player only) | +4 B |
| Actor action timeline | +20 B |
| Actor movement state (own player only) | +22 B |
| Agent base | 32 B |
| Agent action timeline | +20 B |
| Own player, idle | 98 B |
| Own player, active action | 118 B |
| **Agent, idle** | **32 B** |
| **Agent, active action** | **52 B** |
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

### Position Is Still Three Floats

Position is 12 of the agent record's 32 bytes and the only field in it that was
left at full width. That is deliberate, and this section exists so the next
person does not have to rediscover why.

Quantising a position needs a bounded range to quantise against, and the two
candidate ranges are not equivalent:

- **World-absolute.** The scene today is `plane_200x200`, so a `u16` per axis
  over ±100 m resolves 3 mm. It also writes the size of the world into the wire
  format: a larger map is the same 65,536 steps spread over more metres, so
  resolution falls as the map grows.
- **Relative to the receiving player.** The range is then the relevance radius —
  44 m today, the outer edge of the hysteresis band — so a `u16` per axis
  resolves 1.5 mm and stays there whatever size the map is.
  `build_snapshot_send_set` packs every player record before it rotates the
  agent section, so the origin is always present in the same packet. The cost is
  that an agent record can no longer be decoded on its own; it depends on
  another record in the same snapshot.

What it would buy, as arithmetic on the measured `K` below rather than as a
measurement of its own: the record goes 32 B to 26 B, `K` goes 32 to 39, and the
worst-case staleness moves like this.

| Agents | Now (32 B) | Quantised (26 B) |
|---:|---:|---:|
| 128 | 0.27 s | 0.27 s |
| 200 | 0.47 s | 0.40 s |
| 256 | 0.53 s | 0.47 s |
| 500 | 1.07 s | 0.87 s |

At 128 agents it changes nothing at all, and across the 100–200 range it is
worth one snapshot of a pass. The leverage in this record has largely been spent
already: the fields that were carrying dead weight — a quaternion with one live
axis, a unit vector in three floats, a duplicated entity type — are gone, and
what is left is mostly information.

**Revisit this when the map grows.** Note which way growth pushes: it makes the
world-absolute option *worse*, not the deferral wrong, so a larger map argues
for the player-relative encoding rather than for leaving position alone. The
other input to re-check at the same time is
`kDefaultEntityRelevanceExitDistanceMeters`, because that radius is what a
player-relative encoding would be sized against.

## Measured Agent Refresh Rate

Derived budget arithmetic is not sufficient here, because the agent section is
rotated by a selection rule rather than sent whole, and that rule decides the
refresh period. The numbers below
are measured by driving the real `build_relevant_snapshot` /
`build_snapshot_send_set` pair over a synthetic population and recording, per
agent, how many snapshots pass between appearances.

All agents are placed inside the 40 m relevance sphere, so every agent is
relevant and the measurement isolates send-set selection from range culling.

Idle agents, 1,200 B budget, 15 snapshots per second:

| Agents | Packed per snapshot | Snapshot bytes | Median gap | Max gap | Blackout |
|---:|---:|---:|---:|---:|---:|
| 16 | 16 | 674 B | 1 | 1 | 0.07 s |
| 64 | 32 | 1,186 B | 2 | 2 | 0.13 s |
| 128 | 32 | 1,186 B | 4 | 4 | 0.27 s |
| 256 | 32 | 1,186 B | 8 | 8 | 0.53 s |
| 500 | 32 | 1,186 B | 16 | 16 | 1.07 s |

With every agent mid-action (52 B each), 19 agents fit per snapshot and the
blackout at 500 agents is 1.80 s.

At 16 agents the budget is no longer saturated at all — the whole population
fits in one snapshot, and a light scene now costs 674 B rather than the full
1,178 B it did when an agent was 68 B.

Median and maximum are within one snapshot of each other at every population.
The section serves whoever has waited longest, so the population is partitioned
across consecutive snapshots and every agent's gap is the length of one pass.
There is no burst-then-blackout pattern to average over, and a client can treat
the interval between updates for a given agent as a constant.

### Why the cycle is the number of passes and not the population

`build_snapshot_send_set` stamps each net id with the send sequence it last
went out on, and orders the section by that stamp — never-sent first, net id
breaking ties. An entity therefore does not come up again until everything else
in its section has had a turn, and one pass costs `ceil(N / K)` snapshots.

An earlier rule kept a single index into the relevant list and advanced it by
one per snapshot regardless of how many entities it packed. The window slid
instead of stepping, so a section that packed 15 entities re-sent 14 of them on
the next snapshot and one pass took `N` snapshots rather than `N / K`. The
blackout was `(N - K + 1) / 15` seconds — 7.60 s at 128 agents, 16.13 s at 256
— and the gap distribution was bimodal: an agent was refreshed on 15 consecutive
snapshots and then went dark for the rest of the cycle.

Stamps rather than an index, because an index only survives as long as the list
it points into. Agents enter and leave the relevant set constantly, and a
departure shifts every entity behind it forward by one slot — handing them
somebody else's turn, and skipping whoever the index now points past for a
further full cycle. A stamp is keyed on net id and moves with its entity.

### Population ceiling

Bandwidth is unchanged by population — the budget is saturated from about 15
relevant agents upward, and per-client snapshot traffic is a flat 17,670 B/s
(141 kbit/s). What population buys is staleness, not bytes.

The blackout is `ceil(N / K) / 15` seconds, where `K` is the number of agents
that fit in one snapshot: 32 idle, 19 mid-action. Inverting it, for a target
worst-case staleness `S` seconds the ceiling on simultaneously relevant agents
is `N <= 15 * S * K`:

| Worst-case staleness | Idle agents (K=32) | Mid-action agents (K=19) |
|---:|---:|---:|
| 0.5 s | 240 | 142 |
| 1 s | 480 | 285 |
| 2 s | 960 | 570 |
| 5 s | 2400 | 1425 |

`K` is now a multiplier rather than an intercept, so bytes per agent moves the
ceiling proportionally: halving the agent record roughly doubles the population
that fits a given staleness target. That is the lever quantisation pulls.

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
