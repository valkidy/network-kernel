# Item / Prop Client–Server Sync Policy

## Compatibility baseline

This policy describes the implemented ABI 58 wire contract: network protocol
3, packet schema 21, snapshot schema 17, and gameplay catalog version 6. The
handshake rejects older peers; there is no compatibility translation.

The server is authoritative for Item/Container ID allocation, residency,
quantity, portable state, cooldown, Prop mode, transform, velocity, HP, graph
execution, and request outcomes. A client mirror only applies server packets.
It must never allocate an authoritative Item ID, consume or move an Item, or
run an authoritative Item graph.

## Authority and transport matrix

| Data | Recipient | Transport | Cadence / trigger | Relevance |
|---|---|---|---|---|
| Catalog schema, templates, capabilities, portable field order and policies | Connecting peer | Existing reliable session catalog bundle | Handshake/hash mismatch only | Session |
| Owned Inventory initial state | Container owner only | Reliable `InventorySnapshotPage(26)` | Once after welcomed/catalog-ready; again on resync | Owner, never observers |
| Owned Inventory mutations | Container owner only | Reliable `InventoryDeltaBatch(24)` | At most one batch per container per server tick; no packet while idle | Owner, never observers |
| Inventory resync request | Server | Reliable `InventorySnapshotRequest(25)` | Once when a client detects a gap, until a snapshot completes | Owning session validated server-side |
| Entity/Prop lifecycle and static metadata | Relevant clients | Reliable entity spawn/despawn | Enter/leave relevance; metadata once per relevance lifetime | Existing world relevance filter |
| Carry, Place, Throw and settle mode transitions | Relevant clients | Reliable `PropStateChangeBatch(27)` | Coalesced by Prop and flushed once per tick | Existing world relevance filter |
| Prop transform, velocity and optional HP | Relevant clients | Unreliable periodic snapshot | Existing snapshot cadence and byte budget | Existing world relevance filter |
| Semantic gameplay request | Server | Reliable gameplay request packet | User action, not `KernelPlayerInput` | Requesting session |
| Domain/graph outcome | Requester only | Reliable gameplay outcome packet | Once after the committed facts are published; duplicate returns the cached outcome | Requesting session |

On the reliable event channel, a tick publishes lifecycle spawn metadata first,
then Inventory facts, Prop state changes, and finally request outcomes. A
pending network gameplay outcome forces a publication tick so the outcome does
not overtake a newly created Prop.

## Inventory wire contract

An Inventory container is synchronized only when
`container.owner_entity_id == session.player`. The first version deliberately
does not synchronize chests, trades, observers, or delegated access.

`InventorySnapshotPage` carries container ID, owner entity, revision,
capacity, page index/count, and occupied entries. Empty containers still send
one empty page. Pages contain at most 128 entries and the encoder rejects a
payload above 16 KiB. The client assembles all pages for one revision and then
atomically replaces the mirror; partial pages are exposed as `Syncing`, not as
a partially usable inventory.

`InventoryDeltaBatch` carries container ID and first revision once. Record
revisions are implicit and contiguous. A server flush emits at most 64 records
for a container, retaining history for later batches and for every session's
independent cursor. Native `Kernel_PollInventoryDeltas` uses a separate queue,
so polling cannot consume network history.

Records are encoded as follows:

| Record | Fields |
|---|---|
| Add | slot, Item Instance/Template ID, quantity, next-use tick, portable values |
| Update | slot, Item Instance ID, change mask, only changed quantity/cooldown/portable values |
| Remove | slot, Item Instance ID |
| Move | previous slot, new slot, Item Instance ID |

Portable values follow catalog schema order. The wire sends one 32-bit word
per value (float bit pattern, uint32 value, or 0/1 bool) and does not repeat
field ID, type, projection, or default.

If `first_revision != local_revision + 1`, the client marks the container
`Desynced`, stops applying new deltas, increments the gap statistic, and sends
one snapshot request. After atomic snapshot replacement it ignores stale
revisions and resumes deltas in `Ready`. Native discovery uses
`Kernel_CopyOwnedInventoryContainers`; states are `NotAvailable`, `Syncing`,
`Ready`, and `Desynced`.

## Prop lifecycle and snapshot merge

Reliable entity spawn payload now contains:

- Net/entity/actor identity, owner, tick, actor template, position and rotation.
- Entity Template ID and Collider Template ID.
- Item Template ID, Item Instance ID, world mode and carrier entity ID.

Periodic generic snapshots no longer repeat the 17 bytes of Item Template ID,
Item Instance ID, world mode, and carrier. They contain dynamic identity/type,
owner, position, velocity, animation/visual flags, state flags, and HP/max HP
only when HP is known. The client lifecycle mirror merges static metadata into
`RenderEntityState`; pure World Props therefore also materialize the authored
collider metadata. Leaving relevance despawns the mirror entry, and re-entering
relevance sends a fresh reliable spawn containing the latest metadata.

`PropStateChangeBatch` coalesces repeated changes to one record per Prop per
tick. A full transition record includes mode/carrier, authoritative transform,
and velocity. It is sent only to sessions for which the Prop is currently
relevant. Placed, Carrying, and InFlight transforms continue to use the normal
snapshot stream; this version does not quantize transforms or derive Carrying
transforms on the client.

## Gameplay, transaction and Action Graph policy

Gameplay requests are deduplicated by `(requester_peer, request_id)`. Each
request carries an explicit `KernelDomainAction`; the server validates its
current residency context, ownership, target, range, LOS, capability,
placement, quantity/charge, cooldown, and graph admission. `NoAction` remains
reserved for compatibility but is not emitted.

Pickup, Carry, Place and identity-preserving Throw use a scope-transfer
transaction. The transaction claims the source Item/Prop, performs destination
and materialization preflight, and keeps generated lifecycle/Prop replication
facts hidden until commit. A failed request restores the source and discards
those facts. Queue ordering decides races: the first successful commit changes
authoritative residency, so later requests see the committed state and return
a stable rejection.

Consume and consume-and-spawn Throw perform graph preflight before resource
commit. After commit, an immutable `kItemUsed` event drives one accepted graph
batch. A graph failure is `FailedAfterCommit`: no refund and no automatic
retry. A duplicate returns the original outcome without consuming again.
`SpawnEntity` with `item_template` and `quantity` validates that the Item and
Entity Templates match, allocates a fresh Item Instance ID, and produces a
`Placed` Item-backed Prop. Omitting `item_template` creates a world-only entity.

## Encoded packet sizes

All sizes below include the 28-byte packet header and are asserted by
`network_packets_test` or `network_stats_test` using the schema-19 encoder.

| Packet / record example | Encoded bytes |
|---|---:|
| Reliable entity spawn with Item/Prop metadata | 101 |
| Generic snapshot record, HP unknown | 44 record bytes |
| Generic snapshot record, HP known | 48 record bytes |
| Empty Inventory snapshot page | 58 |
| One-entry Inventory snapshot, no portable values | 81 |
| One-entry Inventory snapshot, two portable values | 89 |
| One Add delta, no portable values | 74 |
| Add(two portable values) + Remove in one batch | 97 |
| Snapshot resync request | 44 |
| One full Prop mode/transform/velocity transition | 84 |
| One mode/carrier-only Prop transition | 44 |
| Gameplay request / outcome | 88 / 60 |

For comparison, removing static Item metadata saves 17 bytes from every
periodic generic Prop snapshot record. It costs 25 additional bytes on the
reliable spawn, so the change breaks even after two dynamic snapshots and then
continues saving bandwidth for the rest of that relevance lifetime.

## Verified traffic behavior and statistics

The owner-isolation network test creates two welcomed sessions and one owned
container. Only the owner receives the initial Inventory packet; the non-owner
receives zero Inventory packets. The measured initial one-item snapshot is 81
bytes. A second flush with no mutations emits no packet and leaves Inventory
bytes unchanged. A later Add produces one 74-byte delta batch only for the
owner.

`KernelNetworkStats` exposes `inventory_delta_bytes_sent`,
`inventory_snapshot_bytes_sent`, `prop_state_bytes_sent`,
`inventory_resync_request_count`, and `inventory_revision_gap_count`, in
addition to existing channel/snapshot counters. These counters measure encoded
application packet bytes before lower-level transport framing.

Verified targeted suites include protocol packet round trips and exact sizes,
ItemStore revision history/mirror gap handling, semantic Item gameplay and
post-commit graph failure, Item-backed graph spawn, owner-only idle-zero
Inventory traffic, client-mode metadata merge, RPC command-queue/query
contract, catalog version/hash validation, and kernel ABI tests.

## Explicitly unsupported client authority and scope

Clients may predict presentation only. They may not create Item/Container IDs,
apply Inventory deltas not received from the server, mutate portable state or
cooldown, change authoritative Prop mode/transform, execute Item graphs, or
assume success before a reliable outcome.

This version does not include Unity C#/UI, save/load, reconnect identity
recovery, nested containers, observer grants, chest/trade replication,
transform quantization, or client-derived Carrying transforms.
