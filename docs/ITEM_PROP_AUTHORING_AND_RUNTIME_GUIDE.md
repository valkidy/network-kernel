# Item / Prop Authoring and Native Runtime Guide

This document describes the native Item / Prop contract introduced with Kernel
ABI 55, protocol 3, snapshot schema 17, packet schema 19, and gameplay catalog
version 5. Unity UI, input gesture presentation, and C# bindings are separate
integration work.

## Authoring

The root gameplay catalog accepts `item_template_dir`. Each YAML file in that
directory defines one Item Template with a stable numeric ID, `fungible` or
`stateful` mode, maximum stack size, capabilities, semantic input mappings,
optional Item-backed Prop entity template, throw/use policy, portable state,
and `on_item_used` Action Graph binding.

Inventory mappings accept `consume`, `place`, `throw`, or `none`. World
mappings accept `pickup`, `carry`, `activate`, or `none`. Every non-`none`
mapping requires its corresponding capability. World interaction mappings
also require an authored interaction range; LOS can be enabled with a blocking
mask. Carrying mappings are fixed by the runtime: Use places and Fire throws.

An Entity Template referenced by an Item Template must be a Prop and must not
also author a Prop interaction policy. Pure world Props author their policy in
the Entity Template instead. This keeps one effective policy for every object.

Portable state fields have a stable name-derived `field_id`, one of `uint32`,
`float`, or `bool`, a default value, and a projection. Version 1 supports
`none` and `health_current`; Pickup captures the world Health value before the
Prop is removed. Stateful Items always have quantity one. A stateful
consumable can name one `uint32` charge field and choose whether reaching zero
terminates the Item.

Identity-preserving Throw requires an Item-backed Prop Entity Template.
Consume-and-spawn Throw requires a non-empty `on_item_used` graph containing a
spawn action. A normal Consume may explicitly bind an empty graph; it commits
successfully with no graph side effect.

See the checked-in examples:

- `game_server/item_templates/activation_token.yaml`
- `game_server/item_templates/grenade_consumable.yaml`
- `game_server/entity_templates/interaction_terminal.yaml`

## Identity and residency

`KernelItemInstanceId` and `KernelInventoryContainerId` are 64-bit session IDs.
Zero is null. The authoritative server allocates them monotonically and never
reuses them. Consumed and fully merged Items remain queryable as terminal
tombstones so stale references and duplicate requests have stable results.

An Item has exactly one residency: Inventory, World, or Terminal. Inventory
Items do not create ECS entities. Every world Item is represented by one Prop
carrying `ItemTemplateRef`, `ItemInstanceRef`, and `PropWorldMode`; Carrying
also carries `CarriedBy`. World modes are Placed, Carrying, and InFlight.

Containers have fixed slot capacity. One instance or stack occupies one slot.
Compatible fungible stacks merge before a free slot is used. Portable state
and cooldown must match for stacks to be compatible. Split operations allocate
a fresh non-reused Item Instance ID.

## Semantic requests and outcomes

Submit gameplay through `Kernel_SubmitGameplayRequest` (client or server) or
the server-only `Kernel_ServerSubmitGameplayRequest`. Do not encode these
requests in per-tick `KernelPlayerInput`. The reliable request key is
`(requester_peer, request_id)`. The server validates the requester, current
actor ownership, selected Item residency, target, range, LOS, capability,
quantity/charge, cooldown, placement, and graph admission using current
authoritative state.

Poll `KernelGameplayRequestOutcome` with
`Kernel_PollGameplayRequestOutcomes`. Domain status and graph status are
separate:

- `NoAction` means the effective mapping was `none`.
- `Rejected` means no domain commit occurred.
- `Committed` means the authoritative Item/Prop state changed.
- `NotSubmitted` means no graph was required.
- `Succeeded` means the accepted batch executed.
- `FailedAfterCommit` means the domain commit remains final even though graph
  execution failed. The runtime does not refund or retry it.

A duplicate reliable request returns the original outcome and cannot consume
again. Action Graph batch dedupe additionally keys requester peer, request ID,
event type, and sequence.

Item graphs receive `event.item` as a typed Item Instance ID. Entity-only
`self` remains an Entity ID. The runtime never substitutes the actor or an
inventory Item into entity `self`.

## Transfer and collision behavior

Pickup performs capacity/merge admission, captures world projections, moves
the Item, and then removes the Prop. Carry preserves Item and Prop identity,
follows the authored carry offset, zeros velocity, and disables the Prop's own
colliders. Place re-enables collision and supports whole-stack transfer or a
fungible partial split. Placement validates finite coordinates, actor range,
and current collider overlap.

Identity-preserving Throw transfers or splits an Inventory Item, or reuses the
Carrying Prop. Direction is normalized after server validation and speed comes
from the Item Template. InFlight Props cannot be picked up, carried, or
activated. Their first valid collider contact zeros velocity and changes the
mode to Placed.

Requests are processed by the single-threaded authoritative queue. The first
request that commits a contested Item/Prop wins; later requests receive a
stable rejection and do not mutate the source.

## Native query and replication contract

Server setup and inspection APIs are:

- `Kernel_ServerCreateInventoryContainer`
- `Kernel_ServerCreateInventoryItem`
- `Kernel_ServerCreateWorldItem`
- `Kernel_GetItemInstance`
- `Kernel_GetInventoryContainer`
- `Kernel_CopyOwnedInventoryContainers`
- `Kernel_CopyInventorySlots`
- `Kernel_PollInventoryDeltas`

Inventory deltas are revisioned Add, Update, Remove, or Move records. Polling
is paged without dropping unread records. If a consumer observes a revision
gap, it should fetch `KernelInventoryContainerView` and a full slot snapshot.
Item views include portable state values and the next-use tick.

Reliable entity spawn carries Entity/Collider Template ID, Item Template ID,
Item Instance ID, world mode, and carrier entity ID. Periodic snapshots carry
only dynamic transform, velocity, and optional HP for Props; the client merges
the lifecycle metadata into `RenderEntityState`. Carry/Place/Throw/settle mode
changes use a reliable tick-batched packet. Gameplay requests and outcomes
have dedicated reliable protocol packets. Client prediction may animate intent but
must not allocate authoritative Item IDs, mutate quantities, or execute Item
graphs.

The control-plane JSON RPC surface provides `inventory.create_container`,
`inventory.create_item`, `item.create_world`, `gameplay.submit_request`,
`item.get`, `inventory.list_owned`, `inventory.get_snapshot`, and
`gameplay.get_request_outcome`. Mutations enter the simulation command queue;
list and snapshot responses are assembled by manual JSON handlers.

## Out of scope

The native implementation intentionally does not define Inventory UI layout,
save/load, cross-scene persistence, reconnect identity recovery, nested
containers, Unity C# bindings, or Tap/Hold presentation policy.
