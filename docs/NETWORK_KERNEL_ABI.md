# Network Kernel ABI

M6.1 through M6.3 define the native plugin boundary for the network kernel.
M6.4 adds a thin Unity C# package over the same C ABI.

## Public ABI

The public ABI is the `extern "C"` `Kernel_*` surface declared in
`engine/src/kernel/public/kernel_api.h`. `KernelHandle` is opaque; callers must
create it with `Kernel_Create` and release it with `Kernel_Destroy`.

`Kernel_GetAbiInfo` returns the ABI version, public struct sizes, and capability
flags. Consumers should call it before creating a kernel and reject an ABI
version they do not support. The current native ABI version is
`KERNEL_ABI_VERSION == 86u`. (This line had read 76 for some time; treat
`kernel_types.h` as the authority and this document as a description.)

## Ownership

Callers own every input and output buffer passed to the ABI. The kernel copies
input data during the call and copies output data into caller-owned buffers. It
does not retain caller buffer pointers after an ABI call returns.

All handles are single-thread owned unless a later ABI revision explicitly
changes that rule. No STL, EnTT, glm, FlatBuffers, or C++ ownership types cross
the public boundary. C++ exceptions are contained inside the ABI implementation;
failures return `NULL`, `false`, or `0`.

## Compatibility Rules

Additive changes must prefer new `Kernel_*` functions or new capability flags.
Breaking changes to public struct layout, enum semantics, buffer ownership, or
function signatures require a `KERNEL_ABI_VERSION` bump.

ABI 86 is a removal rather than an addition, which the rule above does not
otherwise cover. Directors moved out of the kernel entirely, so the game-rule
definition, node, edge and spawn-group-effect structs went, along with both
game-rule enums, the three `KERNEL_MAX_GAME_RULE_*` limits, the four catalog
arrays and their counts, `KernelDirectorKind`,
`KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME`, `KernelAiControllerType_Director`,
and the eight director fields on `KernelEntityAiDefinition`. A director entity
template is now rejected at catalog load rather than validated. Nothing outside
`engine` and `game_server` consumed any of it -- no managed binding referenced a
game-rule struct or the director kind -- so this needed no coordination.
`docs/AI_PATROL_SYSTEM.md` records why they moved.

Snapshot schema 19 shrinks the beam snapshot record from 34 bytes to 6:
`net_id` plus the beam's reach as centimetres in a `uint16`. Position,
rotation, velocity, state and flags all come off the wire. A beam's origin and
aim were never its own -- they are its shooter's, and every snapshot already
carries the shooter's position and `aim_direction` in the actor section -- so
the client rebuilds both from the shooter named by `owner_net_id` in the
projectile spawn batch, and the reach is the one thing only the server can
know, because the server is what decides where the beam stops. State and flags
were structurally zero: beam templates author `speed: 0` and carry neither
`ReplicationState` nor `HomingState`.

`KERNEL_ABI_VERSION`, packet schema 24, and protocol 3 are all unchanged, and
`RenderEntityState::beam_end` still means exactly what it did -- it is now
derived during render-state building instead of decoded. Managed mirrors need
no change. Older clients are rejected at the handshake with
`kDisconnectReasonSnapshotSchemaMismatch` rather than silently misreading the
section.

ABI version 76 adds `KERNEL_MOVEMENT_LAYER_LIMB` (`0x10`) to the movement
collision-mask vocabulary, so a template can ask to be blocked by a legged rig's
per-bone colliders. No struct layout changes; the bit is authored inside the
existing `KernelMovementDefinition::movement_collision_mask`.

The set of authorable bits moved from `KERNEL_MOVEMENT_MASK_DEFAULT` to the new
`KERNEL_MOVEMENT_MASK_SUPPORTED`. The two are now different things on purpose:
`DEFAULT` is what a mask of zero selects and is **unchanged**, so an actor that
authors no mask keeps exactly the blockers it had, while `SUPPORTED` is what
validation accepts. Limbs stay out of `DEFAULT` because a legged actor
contributes a dozen boxes where every other actor contributes one capsule.
Managed mirrors that reproduce the movement-layer constants must add the new bit
and the new mask; the existing constants are unchanged.

ABI version 74 appends `collider_count` and `colliders` to
`KernelSkeletonBindingDefinition`, describing colliders carried by a skeleton's
bones rather than by the entity root. The definitions carry no dimensions: a
collider is sized from its bone's rest scale in the skeleton itself, so the size
has one source and cannot drift from the rig. `KernelColliderPurpose_Limb`
marks a bone-carried collider. Both packet and snapshot schemas are unchanged --
the data reaches clients in the gameplay catalog, not on the wire.

ABI version 73 extends status-effect reapplication with `refresh` and `stack`
replacement policies. Stack authoring appends `max_stacks` and
`refresh_on_stack` to `KernelStatusEffectDefinition`; status queries and remote
presentation append stack counts, and `StatusUpdated` represents an in-place
stack, expiry, or attribution change. Packet schema 24 appends stack count to
the reliable full-state record and best-effort remote status event. Snapshot
schema remains 17.

ABI version 72 introduced the authoritative status lifecycle, stored
instigator attribution, reliable owner full-state query/synchronization, and
best-effort remote applied/removed events. Packet schema 23 added the dedicated
status state packet without changing snapshot schema 17.

ABI version 61 replaces the specialized actor, projectile, item, and entity
template fields in `RenderEntityState` with one `template_id`. Its namespace is
derived from the render entity: Actors use the Actor Template ID, Projectiles
use the Projectile Template ID, Item-backed Props use the Item Template ID, and
pure Props or other entity-template-backed objects use the Entity Template ID.
`collider_template_id` remains separate because it is resolved per rendered
entity, while `item_instance_id` remains the stable runtime Item identity.

ABI versions 62 through 65 add skeleton asset and locomotion definitions,
complete local-pose presentation types, and the read-only
`Kernel_GetSkeletonRenderStates` / `Kernel_GetSkeletonRenderStatesAtTime`
queries. Skeleton pose buffers remain caller-owned and are not added to the
snapshot or network packet schema.

ABI version 66 replaces fixed-cycle leg phases with displacement-threshold
gait authoring. Support feet remain anchored in world space until the grounded
home derived from root movement exceeds `step_threshold_meters`; failed
grounding queries preserve the previous anchor. It also adds the read-only
`Kernel_GetSkeletonBindPose` query and
`KERNEL_CAPABILITY_SKELETON_BIND_POSE`. The query returns the native Ozz bind
pose for an asset id/content-hash pair in caller-owned storage.

Entity root transforms crossing the ABI are native right-handed, Y-up world
transforms. Skeleton bind and procedural transforms are native right-handed,
Y-up bone-local transforms. The kernel performs no Unity-specific conversion.
Consumers must separately map world transforms into their scene convention and
bone-local transforms into the basis of the imported presentation skeleton;
those two mappings need not be the same raw component operation.

Reliable spawn metadata supplies the client projection without changing
snapshot packets.

ABI version 60 adds authoritative lifecycle and population policy for temporary
pure Props. `KernelPropDefinition` carries `lifetime_ticks` and a resolved
population group id; `KernelGameplayCatalogDefinition` carries
`KernelPropPopulationRuleDefinition` entries. Gameplay catalog version 8
authors rules with `prop_population_rules` and per-Prop `lifecycle` blocks.
`ice_block` expires after 900 ticks and belongs to the
`temporary_deployable` group, which is capped at 256 live entities. V1
overflow is fixed to deterministic oldest-first eviction by spawn tick and
NetId; no `overflow` authoring field is accepted. Expiry and capacity eviction
publish authoritative despawns without executing `on_destroy_entity`.
Packet schema 21, snapshot schema 17, and protocol 3 are unchanged.

ABI version 59 replaces the Item throw speed with a trajectory Projectile
Template reference and adds the same reference to pure Prop policy. Gameplay
catalog version 7 rejects the removed `throw.speed` field. Identity-preserving
Thrown Props inherit only movement model, speed, and gravity; packet schema 21,
snapshot schema 17, and protocol 3 are unchanged.

ABI version 58 replaces `KernelGameplayRequest::semantic_button` with the
explicit `domain_action` contract, removes Item and Prop input mappings, and
makes template capabilities the authoritative action allowlist.
`KernelGameplayRequestStatus_NoAction` remains reserved but is not emitted.
Packet schema 21 carries the direct domain action byte; gameplay catalog
version 6 rejects the removed Item `input` and Prop tap/hold mapping fields.
There is no ABI 57, packet 20, or catalog 5 adapter.

ABI version 55 aligns the player-input naming contract with gameplay requests:
`KernelPlayerInput`, `KernelActionIntent`, `KernelActionInput`, and
`Kernel_SubmitPlayerInput` replace their unprefixed or abbreviated names. This
is a source and dynamic-symbol breaking rename; the struct layouts and player
input packet bytes remain unchanged.

ABI version 45 decouples sparse `uint8_t` weapon catalog IDs from fixed actor
inventory storage. `KERNEL_MAX_WEAPON_SLOTS == 4u` is the runtime inventory
capacity; it is not a maximum catalog weapon ID. `KernelServerEntityState` and
`KernelCombatStateDefinition` expose `active_weapon_slot`,
`weapon_slot_count`, `weapon_ids[4]`, per-slot ammo, and per-slot reserve
magazines. Callers must resolve catalog weapon metadata through each slot's
`weapon_ids` entry instead of indexing state directly by weapon ID.

ABI version 44 expands the authored weapon catalog for the grenade launcher and
its projectile/action templates. This revision increased the then-dense weapon
catalog capacity from seven to eight entries. ABI 45 immediately replaced that
dense-capacity ABI model with sparse IDs and four runtime inventory slots.

ABI version 43 adds `KernelGameplayCatalogLoadOptions` and reports its size
through `KernelAbiInfo::gameplay_catalog_load_options_size`.
`Kernel_LoadGameplayCatalog` now accepts optional load options so a catalog and
its verified `KernelStaticCollisionSceneConfig` can be installed as one
lifecycle operation. `out_static_scene_rejected` distinguishes catalog success
from a rejected static-scene attachment. This revision is a breaking function
signature and public-struct-layout change.

ABI version 42 adds `KernelLocalActionResultReason_Cooldown = 12` for projected
Primary Fire admission. Action result records and public struct sizes remain
unchanged; packet schema remains 16 and snapshot schema remains 14.

ABI version 41 adds `KernelNetworkStatsConfig` to `KernelConfig` and extends
`KernelNetworkStats` with per-channel bytes, owner-result reconciliation,
remote-presentation filtering/budget, batch, timeout, duplicate, latency,
zero-id, and collision counters. `KernelNetworkStatsMode_Default` resolves to
Basic; Off disables all network counters/timing; Detailed adds serialization,
deserialization, and owner-result latency timing. Zero-valued limits resolve to
1,200 B action packets, 250 ms remote expiry, 8 KiB/s per client, and
256 KiB/s server aggregate. `KernelAbiInfo::network_stats_config_size` reports
the new configuration layout. Packet schema remains 16, snapshot schema remains
14, and the owner-result and remote-presentation records remain 12 B and 20 B.

ABI version 40 replaces discrete Fire/Reload button input with the 8-byte
`KernelActionIntent` start contract and 8-byte `KernelActionInput` hold/release contract.
`KernelPlayerInput::action_intent` and `KernelPlayerInput::action_input` are the only native
Fire/Reload entry points. `KernelAbiInfo` reports both struct sizes and
`KERNEL_CAPABILITY_ACTION_INTENTS`; `KernelWeaponMechanicsDefinition` requires
explicit Fire and Reload action template ids. Packet schema version 16 carries
the new input fields, while snapshot schema remains 14. ABI 40 provides no ABI
39 or packet 15 adapter.

The client-side `Kernel_SubmitPlayerInput` contract is intent sampling rather than an
immediate simulation or transport step. Client and listen-local input sequence
ids are native-owned; the kernel coalesces repeated submits and emits at most
one prediction/input packet per fixed tick. Callers should submit intent before
`Kernel_Update` to minimize latency. Reversing that order delays a change by one
frame without changing movement speed. Servers retain the last movement intent
for up to 250 ms so a missing input packet does not change authoritative speed.

ABI version 39 adds separate owner-correction and remote-presentation polling
surfaces: `Kernel_PollLocalActionResults` returns reliable authoritative Fire
results, while `Kernel_PollRemoteActionPresentationEvents` returns best-effort
cosmetic events. Packet schema version 15 adds their independent batch message
types and leaves snapshot schema version 14 unchanged.

ABI version 28 adds gameplay catalog bundle synchronization. Servers register
an immutable bundle and manifest before listening. Clients may fetch that
manifest and bundle over the reliable session channel, load the catalog through
the existing memory API, and explicitly continue the normal handshake. Bundle
bytes remain native-owned until copied into a caller-owned buffer with
`Kernel_CopyGameplayCatalogBundle`; the kernel never retains caller output
buffer pointers.

ABI version 31 adds `KernelProjectileCollisionQueryMode` and
`KernelProjectileMechanicsDefinition::collision_query_mode`. Projectile
gameplay hit detection now derives collision geometry from the resolved
`KernelColliderTemplateDefinition` where supported, while `damage_shape`
continues to describe how confirmed hits apply damage.

ABI version 32 changes projectile time-related mechanics parameters to tick
units: standard projectile lifetime uses `lifetime_ticks`, beam damage uses
`damage_per_tick`, and homing turn rate uses `max_turn_degrees_per_tick`.

ABI version 33 adds entity template catalog data for component-driven server
materialization. `KernelEntityTemplateDefinition` describes actor and
server-only director entities, `KernelEntityAiDefinition` carries AI controller
and director spawn policy fields, and `KernelServerEntityCreateInfo` gains
`entity_template_id` while keeping `actor_template_id` as the legacy actor
metadata path. Director entities use `KernelEntityType_Director`, receive a
normal `NetId` for lifecycle/query ownership, and are excluded from snapshot and
render-state output when materialized with the server-only component flag.

ABI version 34 adds neutral visibility reporting. `KernelAgentVisionConfig`
now includes `max_visible_neutrals`, and `KernelVisionStateView` reports
`visible_neutrals` separately from allies and hostiles. Neutral actors are
observable by AI but are not selected as default attack targets.

ABI version 35 replaces reserve ammo counters with reserve magazine counters.
`KernelServerEntityState` and `KernelCombatStateDefinition` expose
`reserve_magazines`, and `KernelWeaponMechanicsDefinition` includes authored
`reserve_magazines` policy from weapon templates. Reloading consumes one spare
full magazine and refills ammo to `magazine_size`, including partial reloads.

Packet schema version 11 adds the pre-handshake gameplay catalog manifest,
bundle request/chunk, and synchronization error messages. Existing handshake,
welcome, protocol, and snapshot compatibility checks remain authoritative
after catalog synchronization completes.

ABI version 3 adds server-only gameplay scaffolding for external dedicated
server logic. The kernel exposes generic entity create/destroy, transform,
velocity, persistent state write, and entity query functions. These functions
are intentionally not enemy-specific, and they fail when used from client mode.

ABI version 4 added projectile prediction reconciliation metadata.

ABI version 5 replaces the coarse `client_tick` fire marker with
`client_action_time_us`, a client-local monotonic action time. The server uses
session clock sync to convert this timestamp into the server timeline before
lag compensation.
`action_instance_id` is the client-originated prediction correlation token for
projectiles and future predicted actions. `RenderEntityState` now exposes a
client-local `uint64_t entity_id` that presentation layers can use as their
stable object key across predicted-to-authoritative binding; `net_id` remains
the server-authoritative id and may be `0` before binding. Snapshot packets do
not transmit `entity_id`.

ABI version 5 also reserves additive input button bits for server-side
defensive correction: `InputButton_Dodge` cancels eligible pending
server-originated player damage, and `InputButton_Parry` reduces it. These bits
reuse `client_action_time_us` for rollback timing and do not change public
struct layout.

ABI version 6 adds PingPong session clock-sync packets. Dedicated servers use
the latest clock offset sample to convert client-local action timestamps to
server time. Action times outside the accepted 100ms compensation window are
clamped, not rejected, before rewind selection.

ABI version 7 adds `Kernel_GetRenderStatesAtTime`, allowing clients to pass a
client-local render timestamp and receive kernel-interpolated render states.
The legacy `Kernel_GetRenderStates` remains available and uses the kernel's
current client-local time. Clients estimate their snapshot render clock offset
from incoming PingPong packets without changing the session packet wire format.
Local-owned predicted projectiles are excluded from the remote interpolation
path after authoritative binding; their snapshots are fast-forwarded to the
local prediction timeline and used as correction targets. Remote entities,
including remote projectiles with no local prediction match, continue to render
from the delayed snapshot interpolation timeline.

ABI version 8 adds `hp` and `max_hp` to `RenderEntityState`, and adds
`max_hp` to `KernelServerEntityState` and replicated entity snapshots. Unity
clients can read player and enemy health directly from the render-state stream
returned by `Kernel_GetRenderStates` or `Kernel_GetRenderStatesAtTime`.

ABI version 9 moves gameplay-owned combat and weapon data out of the engine.
The kernel exposes generic mechanism configuration structs for combat state,
weapon mechanics, and projectile mechanics. Server gameplay layers configure
entities through `Kernel_ServerSetEntityCombatState`,
`Kernel_ServerSetEntityWeaponMechanics`,
`Kernel_ServerClearEntityWeaponMechanics`, and validate weapon mechanism data
with `Kernel_ServerValidateMechanicsConfig`. The engine executes movement,
weapon, projectile, damage, snapshot, and transport mechanisms, but does not
own rifle/rocket/enemy tuning defaults.

ABI version 10 adds weapon metadata query, area-effect weapons, projectile
response fields, projectile damage shape fields, and collision mask fields.
Area effects remain server-authoritative gameplay entities and are queried
through API metadata rather than new snapshot payload fields.

ABI version 11 adds beam weapon mechanics and `Kernel_ServerGetBeamState`.
Beam runtime uses dedicated server-owned beam entities and a DPS accumulator;
beam render metadata is queried through the ABI rather than replicated through
new packet fields.

ABI version 12 adds fire-and-forget homing projectile mechanics and
`Kernel_ServerGetHomingState`. Homing boost can be deterministically predicted
for presentation, while guided and lost-target phases remain server
authoritative and use existing snapshot position/velocity fields.

ABI version 13 through 16 add the gameplay catalog, projectile spawn batching,
debug records, collider shape query, benchmark stats, and network stats
surfaces used by the current native runtime.

ABI version 17 removes deprecated free-form-only gameplay catalog load errors,
adds structured `KernelGameplayCatalogLoadResult` status/error fields, and
replaces public ABI struct `bool` fields with fixed-width `uint32_t` flags.

ABI version 18 adds `RenderEntityState::status`, the
`KERNEL_VISUAL_FLAG_HP_UNKNOWN` render flag, and
`Kernel_PollEntityLifecycleEvents`. Render states report only renderable
entities as active, predicted, or stale. Out-of-range/despawn/destroy
notifications are delivered through the dedicated lifecycle event queue so
presentation callers do not infer lifecycle from a missing or stale snapshot.

ABI version 19 adds `RenderEntityState::projectile_template_id` and
`RenderEntityState::collider_template_id`, and adds
`Kernel_GetProjectileTemplates`, `Kernel_GetColliderTemplates`, and
`Kernel_GetColliderBindings`. The read-back functions return the loaded
catalog definitions that the kernel accepted through `Kernel_LoadGameplayCatalog`
or `Kernel_LoadGameplayCatalogFromMemory`; passing `NULL` or a zero capacity
returns the available count without copying. Projectile collider resolution uses
`ProjectileState::projectile_template_id` to select
`KernelProjectileTemplateDefinition::collider_template_id`.

ABI version 20 adds `KernelCombatStateDefinition::collider_template_id` so
actor entities receive their resolved actor-template collider through the same
combat-state path that already carries actor health, movement, hitbox, and
loadout data. Collider catalog bindings are deprecated: accepted gameplay
catalogs must set `collider_binding_count == 0`, and
`Kernel_GetColliderBindings` is retained for symbol compatibility but always
returns `0`. Collider resolution is now single-path by entity family: actors use
their actor template's resolved collider template id, and projectiles use their
projectile template's resolved collider template id. Client/debug tooling should
consume the resolved `RenderEntityState::collider_template_id` rather than
falling back to an `entity_type` binding.

ABI version 21 adds the stable kernel perception query surface for Vision
System v1. Server gameplay configures agent perception with
`Kernel_ServerSetEntityVisionConfig` and may clear it with
`Kernel_ServerClearEntityVisionConfig`. Presentation and gameplay consumers can
read cone perception state through `Kernel_QueryVisionState`, including visible
allies and hostiles, current hostile candidate, last seen target, and last known
target position. The view intentionally excludes variable behavior/controller
state.

ABI version 22 adds `KernelVec4`, compresses collider shape-specific template
and debug view data into `KernelColliderTemplateDefinition::shape_params` and
`KernelColliderShapeView::shape_params`, and adds cone vision colliders through
`KernelColliderShapeType_Cone`, `KernelColliderPurpose_Vision`, and
`KERNEL_COLLISION_LAYER_AGENT_VISION`. `KernelAgentVisionConfig` now references
the vision cone by `vision_collider_template_id`, and
`KernelVisionStateView` returns that id with the resolved actor collider id.
Visual debuggers should read `Kernel_QueryVisionState` for runtime perception
and `Kernel_GetColliderTemplates` for the referenced cone parameters.

ABI version 23 unifies player and enemy entity categories under actor entities.
Actor-specific classification is exposed through `actor_type` fields while the
entity type remains `kActor` for player, agent, and future actor variants.

ABI version 25 adds `KernelActorTemplateDefinition`, actor-template catalog
load/read-back through `KernelGameplayCatalogDefinition::actor_templates` and
`Kernel_GetActorTemplates`, `RenderEntityState::actor_template_id`, and
`Kernel_ServerSetEntityActorTemplate`. Actor templates embed
`KernelAgentVisionConfig` directly; remote clients use the replicated
`actor_template_id` plus the local catalog's actor collider and vision collider
template ids to reconstruct visual debugger shapes. `Kernel_QueryVisionState`
on pure clients derives debug-only vision origin and forward from actor
position, actor rotation, and the actor template's `vision.local_origin` /
`vision.local_forward`.

Snapshot schema version 6 refines the v2 sectioned snapshot payload. Actor
records cover player, enemy, and future AI bot entities with optional owner,
rotation, and hp fields. Projectile records split into compact snapshots and
hybrid-correction snapshots; projectile rotation is omitted from the wire and
derived from velocity for render state reconstruction.

Snapshot schema version 7 adds projectile and collider template ids to both
compact projectile snapshots and hybrid-correction projectile snapshots so
clients can reconstruct exact render collider metadata from snapshot state.

Snapshot schema version 10 replaces per-frame actor collider and vision debug
payload with optional `actor_template_id` on actor records. Clients combine the
actor template id, `Kernel_GetActorTemplates`, `Kernel_GetColliderTemplates`,
`Kernel_QueryVisionState`, and `Kernel_QueryColliderShapes` to draw actor hit
and vision shapes without receiving visible target lists or per-frame
world-space vision origin/forward payload.

`Kernel_QueryColliderShapes` treats a `NULL` query as no filters. Within
`KernelColliderShapeQuery`, `entity_net_id == 0`, `entity_type_filter == 0`,
and `purpose_mask == 0` also mean no filter for that dimension. Persistent
`hit` and `damage` colliders are queryable for the lifetime of their render
entity on clients and for the lifetime of their gameplay entity on servers.
Transient colliders, such as segment or beam volumes, are queryable while their
`lifetime_ticks` / `remaining_ticks` keep them active.

`Kernel_QueryVisionState` treats a `NULL` query as no filters. Within
`KernelVisionStateQuery`, `agent_net_id == 0` and `entity_type_filter == 0`
also mean no filter for that dimension. Host/server kernels return runtime
perception state. Pure clients return template-derived visual-debug state for
replicated actors that carry an actor template id with embedded vision config;
visible hostile/ally lists and current target state remain server-local.

The current projectile interaction foundation is internal C++ engine state. It
does not add Kernel C ABI functions, does not change public struct layout, and
does not require an ABI version bump beyond v12.

Consumers pass a `struct_size`-style byte size to `Kernel_GetAbiInfo`. The call
returns `false` if the output pointer is null or the provided size is smaller
than the current `KernelAbiInfo` layout.

ABI version 72 adds `KernelStatusEffectView`, `Kernel_QueryStatusEffects`, and
the `StatusRemoved` remote presentation event. Packet schema version 23 adds a
reliable full-replacement status state packet for the owning player; snapshot
schema remains version 17 because active statuses are not part of the
high-frequency actor snapshot. The packet carries at most 32 records and is
ordered by a monotonic state revision, so clients discard stale replacements
without delta-gap recovery.

Status queries are authoritative on dedicated/listen servers and for the local
player on a pure client. Remote client entities are explicitly best effort:
their cache is driven by relevance- and budget-limited `StatusApplied` /
`StatusRemoved` presentation events and may locally expire an entry using its
catalog duration. Status lifecycle authoring continues to expose
`event.instigator`; the runtime stores both its NetId and peer attribution when
the status is applied.

## Exported Symbols

`libnetwork_kernel.dylib` exports only the public `Kernel_*` and
`GameServer_*` symbols on macOS. Internal C++ symbols and third-party dependency
symbols are hidden by linker export flags. `dynamic_abi_smoke_test` verifies
this with `nm -gU`.

## macOS Dependencies

Build the optimized macOS dylib with:

```text
bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
```

It produces:

```text
bazel-bin/engine/src/kernel/signed/libnetwork_kernel.dylib
```

`//engine/src/kernel:network_kernel_shared_unsigned` preserves the unsigned
debug dylib at `bazel-bin/engine/src/kernel/libnetwork_kernel.dylib`.

The macOS build links against Homebrew OpenSSL:

```text
/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib
/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
```

External consumers must either run on a machine with those libraries available
at those install names or package equivalent runtime dependencies with adjusted
install names in a later packaging step.

## Unity Package

The repository currently stores packed Unity UPM artifacts under
`plugins/output`; it does not keep an expanded
`plugins/com.network-example.kernel` source tree checked out. The latest
artifact is:

```text
plugins/output/com.network-example.kernel-0.6.9.tgz
```

That package contains handwritten C# P/Invoke declarations, ABI layout
validation, client/host/LAN samples, the gameplay catalog bundle, and native
plugins for macOS and Windows x86_64. Its managed constants mirror
`KERNEL_ABI_VERSION == 45u` and `KERNEL_MAX_WEAPON_SLOTS == 4u`.

Unity resolves the C# import name `network_kernel` to
`libnetwork_kernel.dylib` on macOS and `network_kernel.dll` on Windows. Package
consumers must still call `Kernel_GetAbiInfo` and validate ABI version,
capabilities, and public struct sizes before creating a kernel. The supported
way to rebuild, stage, verify, and pack the expanded package is the repository's
`unity-plugin-package-builder` workflow; generated staging content is
intermediate output rather than the source of truth.
