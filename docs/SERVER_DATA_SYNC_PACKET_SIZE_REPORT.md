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

Slots are weighted, so the refresh rate is not one number. An entity's share
scales with a weight taken from how far it is from the receiving player — four
inside 10 m, two inside 25 m, one beyond — doubled again while it is mid-action.
The gap is therefore reported per band; a single worst case across all agents
would describe the far band's share being spent on the near band as if it were
only a loss.

Idle agents, 1,200 B budget, 15 snapshots per second:

| Agents | Packed per snapshot | Snapshot bytes | Near | Mid | Far | Worst |
|---:|---:|---:|---:|---:|---:|---:|
| 16 | 16 | 674 B | 0.07 s | 0.07 s | 0.07 s | 0.07 s |
| 64 | 32 | 1,186 B | 0.07 s | 0.13 s | 0.27 s | 0.27 s |
| 128 | 32 | 1,186 B | 0.13 s | 0.27 s | 0.47 s | 0.47 s |
| 256 | 32 | 1,186 B | 0.40 s | 0.60 s | 0.93 s | 0.93 s |
| 500 | 32 | 1,186 B | 1.00 s | 1.00 s | 1.47 s | 1.47 s |

With every agent mid-action (52 B each), 19 fit per snapshot: at 128 agents the
bands read 0.27 / 0.53 / 0.80 s, and at 500 they read 1.00 / 1.73 / 2.93 s.

Against the unweighted rotation this replaced, 128 agents went from a flat
0.27 s to 0.13 s near and 0.47 s far. That is the whole trade: the band the
player is fighting in refreshes twice as fast, and the band at the edge of the
sphere pays 1.7x for it.

At 16 agents the budget is no longer saturated at all — the whole population
fits in one snapshot, and a light scene costs 674 B rather than the full
1,178 B it did when an agent was 68 B.

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

Under weighting an entity's refresh interval is `sum(w) / (K * w_i)` snapshots,
where `K` is the number that fit in one snapshot — 32 idle, 19 mid-action. The
spiral used for these measurements puts about 8% of the population inside 10 m
and 48% inside 25 m, so at 128 agents `sum(w)` is 196 and the predicted
intervals are 1.5 / 3.1 / 6.1 snapshots. The measured 0.13 / 0.27 / 0.47 s are
those three numbers divided by 15.

Above the weighting sits a floor: anything that has gone
`kMaxSnapshotsWithoutSend` snapshots — 15, so one second — without a turn is
promoted ahead of the weighting entirely. It is what stops a crowd of
high-weight neighbours from holding an entity off indefinitely, and it is why
the 500-agent rows flatten towards 1.00 s.

That floor can only be honoured while the budget can carry the whole relevant
population inside the window, which is `N <= 15 * K`: 480 agents idle, 285
mid-action. The 500-agent row is the first that exceeds it, and its far band
reads 1.47 s rather than the promised 1.00 s — the promotion puts an entity at
the head of the queue, and past that population there are not enough slots for
everything at the head of the queue to fit.

For a target worst-case staleness `S` in the *far* band, the ceiling is still
`N <= 15 * S * K` scaled by the far band's share of `sum(w)`; the near band
reaches any given target at roughly four times the population.

`K` is now a multiplier rather than an intercept, so bytes per agent moves the
ceiling proportionally: halving the agent record roughly doubles the population
that fits a given staleness target. That is the lever quantisation pulls.

## Four Players

Everything above is measured against one session. Player count pulls on two
separate things, and only one of them is expensive.

- **Per-session work** — building a relevant snapshot and a send set, and the
  egress that follows — is linear in player count and small. Four sessions over
  an 800-agent world cost 310 us a tick, half a percent of the budget. Each
  session also carries its own 1,200 B budget and its own rotation, so the
  staleness figures above hold per player rather than being divided among them.
- **World-population work** — AI, vision, physics — is quadratic in the number
  of agents and completely indifferent to how many players are watching.

So the question that decides the cost is not how many players there are. It is
whether four players means four times the agents.

| | A. party, shared 200 | B. spread, 200 each | C1. capped, even | C2. capped, all on one |
|---|---:|---:|---:|---:|
| World agents | 200 | 800 | 200 | 200 |
| Relevant to the measured player | 200 | 196 | 49 | 196 |
| Packed per snapshot | 25 | 27 | 30 | 26 |
| AI | 68.4 us | 631.3 us | 62.8 us | 62.1 us |
| Kernel update | 1.87 ms | **37.7 ms** | 1.40 ms | 1.71 ms |
| Snapshot build, four sessions | 126.2 us | 290.4 us | 71.7 us | 71.9 us |
| **Share of the 33.3 ms tick** | 6.2% | **115.8%** | 4.6% | 5.5% |

Staleness is not a column here. Slots are weighted, so an entity's refresh
interval depends on which band it is in rather than on the population divided by
the packed count; **Measured Agent Refresh Rate** above reports it per band. The
packed count is also not a measure of throughput any more — the weighting pulls
mid-action agents forward, and those carry the 20 B action timeline, so the same
1,136 B of budget buys fewer but more relevant records.

All four hold the same thing constant where they can: roughly 200 agents in
front of the measured player. What differs is how many the world holds and where
the other three players are standing.

### Cap the world, not the view

B is not slow, it is over budget: 38 ms of work in a 33.3 ms tick, of which 36.9
is inside `Kernel_Update` alone. Four times the agents costs twenty times the
kernel.

C is the same situation under a global population cap — the world holds 200
however the players are arranged — and it removes the problem outright, at no
cost to the player who has the crowd:

- server cost `115.8% -> 5.5%`, a factor of twenty-one;
- the player in front of the crowd sees **no change at all**: 27 packed per
  snapshot against 26, from an identical relevant set.

The three players who get an empty stretch of map are paying for it, but nothing
is transferred to the first player. This is not a trade against frame time.

C1 and C2 differ only in how the capped population happens to be distributed,
which is a consequence of where players go rather than something the server
chooses. The measurement is here to answer whether that distribution matters to
performance, and it does not: 4.6% against 5.5%, with the kernel column moving
1.40 ms to 1.71 ms. Either distribution is comfortably inside budget, so the
allocation policy can be decided on gameplay grounds alone.

### Standing together costs a little more

A costs 6.2% against C2's 5.5% on the same 200-agent world, and the difference
is the teammates: four players inside one relevance sphere means each session's
packet carries three of them at 76 B alongside its own record.

Those records are scheduled rather than guaranteed. Only the receiving session's
own player is written unconditionally — local prediction is reconciled against
it, so a snapshot without it is one the client cannot correct itself with.
Everyone else's competes on weight like an agent does, with a player bonus that
keeps a teammate standing next to you ahead of the crowd around it and lets one
at the edge of the sphere fall back to roughly what a near agent gets.

Which means the party case does not get much cheaper, and should not: teammates
you are standing next to are worth the bytes. What the change buys is the other
arrangement — a teammate 35 m away no longer holds a guaranteed slot against the
agents you are actually fighting.

### Where the ceiling is

| World agents | Share of the tick |
|---:|---|
| 200 | 5–6% |
| 400 | ~17%, and the engine's own overload warning fires (`avg_cost_us` 8,077 against an 8,000 threshold) |
| 800 | 113%, and the simulation command queue starts rejecting AI commands (1,404 rejected at a 2,048 capacity) |

The quadratic makes 200 to 400 cost 3.4x, so there is not much room to creep
upwards. **200–250 is a sound cap and 400 is the hard ceiling** for this
architecture; the two failures past it are silent ones — a warning in a log, and
AI commands dropped without an error.

## Composition

Agents are not the only thing in the world. The target is roughly 200 actors,
200 projectiles in flight, and up to 256 deployable props — the cap the
catalog's `temporary_deployable` population rule already carries.

| World | Relevant agents | Relevant projectiles | Relevant props | Packed agents | Packed projectiles | Packed props | Kernel | Share of tick |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 196 agents + 4 players | 192 | 93 | 0 | 30 | 0 | 0 | 1.64 ms | 5.3% |
| + 200 projectiles | 192 | 228 | 0 | 17 | 9 | 0 | 2.38 ms | 7.7% |
| + 256 props | 192 | 138 | 256 | 17 | 9 | 0 | 3.31 ms | 10.7% |
| 196 agents + 256 props | 192 | 0 | 256 | 30 | 0 | 0 | 2.28 ms | 7.4% |
| 20 agents, same everything | 20 | 193 | 256 | 8 | 16 | 0 | 0.43 ms | 1.7% |

Packed counts are averages over the sample window. A single snapshot is one draw
from a rotating queue, and reading the last tick's counts as a rate is how an
earlier version of this table came out showing projectiles taking ninety per
cent of the budget.

**CPU is not the constraint.** The full composition is 594 entities at 10.7% of
the tick, and the marginal costs are small: about 0.74 ms for the projectiles and
0.65 ms for the props.

### Sections used to starve each other

The send set was built section by section in a fixed order — actors, then
projectiles, then everything else written straight out with no rotation at all.
The first section to fill the budget took all of it, so with a crowd of relevant
agents **no projectile reached the client at all**: 0 packed against 93 relevant
in the first row above, and 8 packed once the agent count dropped to 20. That was
measured, not deduced.

The queue is now one pass over everything the session can see, ordered by the
same `wait * weight` rule. Selection order does not have to match the wire:
`encode_snapshot_packet` regroups by section at encode time, so the merge left
the packet format untouched — no schema bump, no client change. The tail also
gains a rotation it never had; props and world items used to be written in world
iteration order with no memory of who had been dropped.

### Dormant props cost nothing on the wire

`packed props` is zero in every row, and that is by design rather than
starvation. A placed prop that is not moving is rejected by
`prepare_send_entity`; it is bootstrapped once through a
`PropStateChangeBatchPacket` when it enters relevance and then costs no snapshot
budget at all. **256 props are free on the wire.** Their cost is CPU and one
reliable packet each. A prop that starts moving rejoins the queue like anything
else.

### An actor's turn against a projectile's

Left weight-neutral, the queue splits by population: 192 relevant agents against
228 relevant projectiles gave 13 agent slots to 12 projectile ones, taking agents
from 30 a snapshot to 13.

That is the wrong split, for a reason that is about mechanism rather than taste.
An actor's snapshot record is the only thing telling the client where it is. A
projectile's is a *correction* on top of a reliable spawn packet that already
carried its position and velocity, which the client dead-reckons between
corrections — so what a projectile needs is a prompt first delivery, and
`wait * weight` only buys a high sustained rate. `kSnapshotPriorityActorWeight`
is the multiplier that says so; at 8 the split measures 17 agents to 9
projectiles.

Agents still lose about 43% of their rate when 228 projectiles are in flight, and
that is real: the budget is fixed, and anything projectiles get is taken from
somewhere. The far band goes from 0.47 s to roughly 0.75 s under that load.

The projectile population here is synthetic — 200 hand-spawned alongside whatever
the agents fire, with no lifetimes — so the split is indicative rather than
final. `kSnapshotPriorityActorWeight` is one constant with a measured effect;
re-run the composition table after changing it.

## Reproduction

```text
bazel build --config=macos -c opt //game_server/gameplay_catalog_bundle:bundle.zip
stat -f '%z' bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
unzip -l bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:session_packets_test
bazel test --config=macos -c opt //engine/src/tests/protocol_tests:network_packets_test
bazel run -c opt //engine/src/tests/kernel_tests:snapshot_bandwidth_benchmark
bazel run --config=macos -c opt //game_server:agent_cpu_bench
```

`agent_cpu_bench` is the source for the **Four Players** section and for the CPU
figures quoted in it. It drives real `chaser_grunt` agents through the shipping
`AgentRuntimeManager` path against the real catalog, and reports wall clock on
the host it runs on rather than asserting a bound.

Regenerate the bundle before recording its size; do not use a stale
`bazel-bin` artifact as a new baseline.

Both protocol tests are red on this baseline for reasons unrelated to size.
`session_packets_test` aborts at its Welcome assertion, which still expects the
pre-`actor_blocking_mode` 60 B packet; every size assertion above it passes and
is the source for the catalog table. `network_packets_test` aborts on a
snapshot field that the codec no longer serialises; its size assertions run
before that abort and pass.
