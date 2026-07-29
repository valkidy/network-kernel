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
`KERNEL_ABI_VERSION == 55u`.

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

ABI version 55 aligns the player-input naming contract with gameplay requests:
`KernelPlayerInput`, `KernelActionIntent`, `KernelActionInput`, and
`Kernel_SubmitPlayerInput` replace their unprefixed or abbreviated names. This
is a source and dynamic-symbol breaking rename; the struct layouts and player
input packet bytes remain unchanged.

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

## Unity Prototype

The Unity package lives at:

```text
plugins/com.network-example.kernel
```

The intended package provides handwritten C# P/Invoke declarations for the
public `Kernel_*` surface, C# mirror structs for public ABI data, an
`IDisposable` wrapper for `KernelHandle`, local player info helpers, an Editor
ABI smoke runner, and minimal listen-server/client smoke samples.

On macOS, the Unity package includes the built dylib at:

```text
plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib
```

Unity resolves the C# import name `network_kernel` to `libnetwork_kernel.dylib`
on macOS. The Editor smoke runner calls `Kernel_GetAbiInfo`, validates
`Marshal.SizeOf<T>()` against native struct sizes, starts listen-server mode,
checks `Kernel_GetLocalPlayerInfo`, updates the kernel, submits one input,
polls events, reads render states, and destroys the handle.

Current caveat: the workspace package source under
`plugins/com.network-example.kernel` contains the minimal Vision v1 managed ABI
mirror and layout checks, but the native dylib is not staged in the package in
this branch. The old packaged artifact under `plugins/output` may contain
managed bindings for an earlier ABI and should not be treated as authoritative.
