# Network Kernel Development Roadmap

> macOS-first C++ co-op shooter network kernel  
> Transport: GameNetworkingSockets  
> ECS: EnTT  
> Math: glm  
> Protocol: FlatBuffers  
> Build System: Bazel  
> Plugin Boundary: C ABI

---

# Milestone Status

| Milestone | Name | Status |
|---|---|---|
| M1 | Headless Simulation | Completed |
| M2 | Loopback Listen Host | Pending |
| M3 | Real Network Transport | Planning |
| M4 | Authoritative Combat | Pending |
| M5 | Lag Compensation & Smoothing | Pending |
| M6 | Engine Plugin Boundary | Implemented |

---

# M1 — Headless Simulation

Status: Completed

## Goal

Build a headless authoritative simulation kernel.

## Completed Scope

### Runtime

- Dedicated server mode
- Fixed tick loop
- Single-thread architecture

### World

- EnTT ECS world
- Entity creation / destruction
- NetworkIdentity
- Transform
- Velocity
- Health

### Simulation

- Player movement
- Enemy entities
- Projectile entities

### Combat

- Basic hitscan

### Build

- Bazel targets working
- macOS build verified

## Deliverables

```text
//app/dedicated_server
```

---

# M2 — Loopback Listen Host

Status: Pending

## Goal

Complete client/server data flow within a single process.

## M2.1 Runtime Mode

### Tasks

Implement:

```cpp
Kernel_StartListenServer()
```

Support:

```text
Client + Server in same process
```

### Acceptance

- Listen server starts successfully
- Local player entity can spawn

---

## M2.2 Loopback Transport

### Tasks

Add:

```text
LoopbackTransport
```

Support:

```text
Client -> Server packet queue
Server -> Client packet queue
```

### Acceptance

- No real network required
- Packets flow through loopback transport

---

## M2.3 Input Pipeline

### Tasks

Complete:

- PlayerInput
- InputPacket
- inputSeq
- clientTick

### Acceptance

- Local input reaches authoritative server

---

## M2.4 Snapshot Pipeline

### Tasks

Complete:

- Snapshot builder
- Snapshot receiver
- Render state export

### Acceptance

- Client receives authoritative snapshots
- Render states are available

## Deliverables

```text
//app/host_server
```

---

# M3 — Real Network Transport

Status: Planning

## Goal

Replace the loopback-only listen host path with a real GameNetworkingSockets
transport path that can run a dedicated server and a separate example client on
localhost.

M3 should preserve the current deterministic server simulation shape: the server
remains authoritative, clients send input, and snapshots are the only replicated
world state sent back to clients.

## Current Baseline

- `ITransport` already defines client/server start, send, and poll semantics.
- `LoopbackTransport` proves the M2 input/snapshot flow in a single process.
- `NetworkSimulatorTransport` can exercise unreliable delivery behavior without
  real sockets.
- `Kernel_StartClient()` is still a stub.
- `GameNetworkingSockets` is declared in `WORKSPACE`, but the wrapper currently
  exposes headers only.
- Existing packet support covers `InputPacket` and `SnapshotPacket`; session
  messages are listed in `MessageType` but do not yet have encoders/decoders.

## Non-Goals

- No NAT traversal, lobbies, matchmaking, or Steam identity integration.
- No rollback/prediction/interpolation; those stay in M5.
- No binary plugin boundary changes; those stay in M6.
- No gameplay expansion beyond proving remote movement and replicated snapshots.

## Architecture Direction

Use a concrete transport implementation behind `ITransport`:

```text
KernelEngine
  -> ITransport
      -> LoopbackTransport
      -> NetworkSimulatorTransport
      -> GnsTransport
```

The kernel should not include GameNetworkingSockets headers directly. All GNS
types and lifetime management should stay in `transport/src/gns_transport.cc`
and `transport/public/gns_transport.h`.

Transport events should be normalized into the existing event model:

- GNS connection accepted -> `TransportEventType::kConnected`
- GNS connection closed/problem detected -> `TransportEventType::kDisconnected`
- GNS message received -> `TransportEventType::kMessage`

Channels should map to GNS send lanes or message flags only inside
`GnsTransport`; higher layers continue using `ChannelId`.

## M3.1 Server Transport

### Tasks

- Add `GnsTransport` implementing `ITransport`.
- Initialize and shut down GameNetworkingSockets through RAII.
- Start a listen socket on the requested port.
- Poll connection status callbacks and accept incoming clients.
- Assign stable `PeerId` values for accepted connections.
- Maintain bidirectional maps between `PeerId` and GNS connection handles.
- Convert close/problem events into transport disconnect events.
- Add focused transport tests around peer mapping and message dispatch where
  real sockets are not required.

### Acceptance

- Dedicated server can bind to `127.0.0.1:7777`.
- Server reports a connected peer when a local client connects.
- Server can receive a message from that peer.
- Disconnect removes peer mappings and emits one disconnect event.

---

## M3.2 Client Transport

### Tasks

- Implement `GnsTransport::StartClient(const char* address)`.
- Parse host/port strings consistently with current C ABI inputs.
- Connect to a remote server through GameNetworkingSockets.
- Emit connected/disconnected transport events for the client side.
- Send messages to the server through a reserved server peer id.
- Add retry-free failure behavior: bad address or failed connect should return
  `false` and leave the transport stopped.

### Acceptance

- Example client connects to the dedicated server on localhost.
- Client can send one reliable session message and one unreliable input message.
- Client stop/teardown does not leak active connections.

---

## M3.3 Session Protocol

### Tasks

- Add session packet encode/decode helpers alongside the existing M2 packet
  helpers, or rename the packet module if the M2 name becomes misleading.
- Implement:
  - `Handshake`: protocol version, schema version, client nonce.
  - `Welcome`: assigned peer id, server tick rate, snapshot rate.
  - `Disconnect`: reason code.
  - `PingPong`: nonce/client time/server time for basic liveness.
- Route `ChannelId::kSession` through reliable send mode.
- Reject mismatched protocol/schema versions before spawning a player.
- Add protocol tests for valid packets, truncated packets, CRC failures, and
  version mismatch.

### Acceptance

- Version mismatch is rejected
- Disconnect is handled correctly
- A successful handshake produces a `Welcome` packet with an assigned peer id.
- Ping/pong round trip works over loopback or the network simulator before GNS
  integration is considered complete.

---

## M3.4 Input Replication

### Tasks

- Make `Kernel_StartClient()` create a client transport path instead of returning
  a stub error.
- Keep `Kernel_SubmitInput()` as the public client input entry point.
- In client mode, encode `InputPacket` and send it to the server over
  `ChannelId::kInput` with `SendMode::kUnreliable`.
- On the server, map input peer ids to spawned player entities after handshake.
- Ignore or error-report input from unauthenticated peers.
- Preserve `input_seq` and `client_action_time_us` for M5 prediction/ack work.

### Acceptance

- A separate example client can move its authoritative server-side player.
- Server accepts input only after session handshake is complete.
- Existing listen-server input tests still pass.

---

## M3.5 Snapshot Replication

### Tasks

- Send `SnapshotPacket` from dedicated server to every connected client on
  `ChannelId::kSnapshot`.
- In client mode, decode snapshots from the transport and store the latest
  client snapshot.
- Make `Kernel_GetRenderStates()` return state rebuilt from server snapshots
  when running as a network client.
- Keep snapshot sends unreliable by default.
- Add a minimal server/client integration test using loopback or simulator first,
  then add an optional localhost smoke path for GNS.

### Acceptance

- Remote authoritative state is visible
- Snapshot `last_processed_input_seq` reflects consumed client input.
- Client render state changes after server simulation ticks.

---

## M3.6 Example Client

### Tasks

- Add:

```text
//app/example_client
```

- Start `KernelMode_Client`.
- Connect to `127.0.0.1:7777` by default.
- Submit scripted movement input for a short run.
- Log connection, welcome, snapshot, and render-state output.

### Acceptance

- Running the dedicated server and example client in separate processes shows:
  - client connected
  - player joined
  - input consumed
  - snapshots received
  - render states printed from authoritative snapshots

---

## M3 Validation Plan

Run the fast deterministic tests first:

```text
bazel test //tests/protocol_tests:all //tests/transport_tests:all //tests/kernel_tests:all --noenable_bzlmod
```

Then verify the M3 binaries:

```text
bazel build //app/dedicated_server:dedicated_server //app/example_client:example_client --noenable_bzlmod
```

Finally run a manual localhost smoke:

```text
bazel run //app/dedicated_server:dedicated_server --noenable_bzlmod
bazel run //app/example_client:example_client --noenable_bzlmod
```

## M3 Risks

- The current `GameNetworkingSockets` Bazel wrapper may need compiled sources
  and transitive dependencies, not only headers.
- Client/server role state in `KernelEngine` is currently biased toward
  listen-server and dedicated-server paths; client mode needs a clean branch.
- Session protocol should be introduced before remote player spawning, otherwise
  peer identity will be fragile.
- GNS callbacks may produce events in bursts; event queues should remain bounded
  by `KernelConfig::max_events` before this becomes user-facing.

## Deliverables

```text
//app/dedicated_server
//app/example_client
//transport:transport
//tests/protocol_tests
//tests/transport_tests
//tests/kernel_tests
```

`//transport:transport` should include the new `GnsTransport` implementation
once M3 transport work begins.

---

# M4 — Authoritative Combat

Status: Pending

## Goal

Complete the core shooter combat loop.

## M4.1 Weapon System

### Tasks

Complete:

- Ammo
- Cooldown
- Reload state

### Acceptance

- Server validates fire rate

---

## M4.2 Rifle Hitscan

### Tasks

Complete:

- Fire command
- Server raycast
- Damage
- Hit event

### Acceptance

- Targets receive damage

---

## M4.3 Shotgun

### Tasks

Complete:

- Pellet spread
- Multi-raycast

### Acceptance

- Multiple pellet hits are applied

---

## M4.4 Projectile

### Tasks

Complete:

- Grenade spawn
- Projectile simulation
- Sweep collision

### Acceptance

- Projectile travels and impacts correctly

---

## M4.5 Explosion

### Tasks

Complete:

- Sphere overlap
- Damage falloff

### Acceptance

- AoE damage works correctly

## Deliverables

```text
Rifle
Shotgun
Grenade
Explosion
Damage events
```

## ABI Notes

- Keep combat data behind `Kernel_PollEvents()` and C-compatible public structs.
- Candidate post-spike ABI extensions: weapon id, fire sequence, hit position,
  damage amount, explosion radius, and compact flags for damage source metadata.
- Do not expose simulation internals, STL containers, GLM values, or EnTT ids.

---

# M5 — Lag Compensation & Smoothing

Status: Pending

## Goal

Support fair shooting and smooth synchronization under latency.

## M5.1 History Buffer

### Tasks

Store:

```text
HitVolumeSnapshot
```

Every server tick.

### Acceptance

- Historical hitboxes can be queried

---

## M5.2 Server Rewind

### Tasks

Calculate rewind tick using:

- action time
- inputSeq
- server receive time

### Acceptance

- Rewind tick is clamped correctly

---

## M5.3 Rewind Hitscan

### Tasks

Perform raycast against history buffer.

### Acceptance

- Moving targets can be hit under latency

---

## M5.4 Client Prediction

### Tasks

Complete:

- Local movement prediction
- Input history
- Reconciliation

### Acceptance

- Minimal correction popping

---

## M5.5 Interpolation

### Tasks

Complete:

- Remote snapshot interpolation

### Acceptance

- Smooth remote movement

---

## M5.6 Network Simulation

### Tasks

Use:

```text
NetworkSimulatorTransport
```

Test:

- Latency
- Packet loss
- Jitter
- Packet reordering

### Acceptance

System remains stable under:

```text
100ms latency
5% packet loss
30ms jitter
```

## Deliverables

```text
Lag compensation
Prediction
Reconciliation
Interpolation
```

## ABI Notes

- Preserve `PlayerInput::input_seq` and `PlayerInput::client_action_time_us` as
  the public prediction and server-ack bridge.
- Candidate post-spike ABI extensions: acknowledged input sequence, snapshot
  interpolation flags, local correction vector, and clamped rewind tick metadata.
- Keep history buffers and rewind queries internal to the kernel.

---

# M6 — Engine Plugin Boundary

Status: Implemented

## Goal

Package the kernel as a native engine plugin with stable C ABI boundaries, a
Bazel-built dynamic library, an externally consumable export surface, and a
thin Unity C# package.

## Scope

Included:

- Stabilize the public C ABI around opaque handles and plain C data structs.
- Produce `libnetwork_kernel.dylib` from the Bazel build.
- Validate that an external native consumer can load and call exported symbols
  without depending on internal C++ headers.
- Document and provide the Unity C# P/Invoke package scope for M6.4.

Deferred:

- Unreal wrapper and actor synchronization.
- Host migration, matchmaking, Steam integration, anti-cheat, mobile packaging,
  and backend orchestration.
- New gameplay protocol features beyond ABI extensions required by M4/M5.

## M6.1 C ABI Stabilization

### Tasks

Implemented:

- Keep `KernelHandle` opaque in `kernel_api.h`.
- Keep public data in `kernel_types.h` limited to fixed-width integer types,
  floats, bools, enums, and POD structs.
- Add `KernelAbiInfo`, `KERNEL_ABI_VERSION`, and capability flags.
- Add `Kernel_GetAbiInfo()` for native consumers.
- Document ownership, capacity, lifetime, and ABI compatibility rules in public
  headers and `docs/NETWORK_KERNEL_ABI.md`.

### Acceptance

- No STL, EnTT, glm, FlatBuffers, or C++ ownership types cross the ABI.
- Public headers compile from a C or C-compatible consumer translation unit.
- ABI metadata reports public struct sizes and capability flags.

---

## M6.2 Dynamic Library

### Tasks

Implemented shared-library target:

```text
//engine/src/kernel:network_kernel_shared
```

Produce:

```text
libnetwork_kernel.dylib
```

- Keep exported symbols limited to the public `Kernel_*` API surface and the
  Unity-consumable `GameServer_*` bridge surface.
- Ensure the library links with the existing transport, simulation, sync, world,
  and protocol targets.
- Keep third-party dependency linkage documented for macOS in `docs/NETWORK_KERNEL_ABI.md`.

### Acceptance

- Bazel builds the dynamic library on macOS.
- `nm` or an equivalent symbol check shows the intended exported `Kernel_*` and
  `GameServer_*` symbols.
- The library can be copied beside an external consumer executable and loaded at
  runtime.

---

## M6.3 Export Interfaces

### Tasks

Implemented and validated the engine-facing runtime calls:

- `Kernel_Create()`
- `Kernel_Destroy()`
- `Kernel_StartClient()`
- `Kernel_StartListenServer()`
- `Kernel_StartDedicatedServer()`
- `Kernel_Update()`
- `Kernel_SubmitInput()`
- `Kernel_GetRenderStates()`
- `Kernel_PollEvents()`
- `Kernel_GetAbiInfo()`

Add an external native consumer smoke test that only includes public headers and
loads the dylib through the platform dynamic-loader API.

### Acceptance

- Stable memory ownership for input buffers, render-state buffers, and event
  buffers.
- External consumer can create a kernel, run at least one update, submit input,
  poll events, and read render states.
- Failure cases return explicit false/zero results rather than leaking C++
  exceptions through the ABI.
- Dynamic ABI smoke test verifies the macOS dylib exports only `Kernel_*`
  symbols.

---

## M6.4 Unity Package

Status: Implemented.

### Tasks

Implemented a Unity-facing package:

- C# P/Invoke declarations and mirror structs for native kernel ABI v7.
- C# P/Invoke declarations and mirror structs for Game Server bridge ABI v1.
- Runtime split into Core, Client, and Host SDK surfaces with native handle
  ownership isolated from Unity presentation code.
- Smoke, Client, and Host samples under `Samples~`.
- Managed ABI smoke under `Tests~` plus an Editor ABI smoke runner.
- Package builder that builds, stages, verifies, and packs the macOS UPM
  package through
  `.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh`.

### Acceptance

- Unity loads the dylib on macOS through P/Invoke.
- Unity-facing runtime code can start client/listen-host paths, submit input,
  poll events, read render states including render-at-time data, and use the
  native Game Server bridge without unsafe lifetime violations.
- The package remains thin: gameplay behavior stays in the native kernel.
- Required validation is hybrid: package-builder verification and C# compile
  checks are expected to pass; Unity Editor batchmode smoke is available but
  environment-dependent and may be skipped when local licensing/headless setup
  blocks Editor startup.

## Deliverables

```text
Dynamic library
C ABI
External native consumer smoke test
Unity C# package
UPM package tarball
ABI documentation
```

## ABI Spike Result

The native plugin boundary has a validated shape before completing the Unity
wrapper:

- Preserve the current opaque `KernelHandle` and `extern "C"` API.
- Build `//engine/src/kernel:network_kernel_shared` as
  `libnetwork_kernel.dylib`.
- Test an external consumer that only uses public headers and resolves exported
  symbols dynamically.
- Record M4/M5 ABI extension candidates before changing public signatures.
- Expose `Kernel_GetAbiInfo()` for ABI version, public struct sizes, and
  capability flags.

M6.1 through M6.4 are implemented.

## M6 Validation Plan

Run the core verification first:

```text
bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
bazel test //engine/src/tests/...
```

Then verify plugin consumption:

```text
External native consumer loads libnetwork_kernel.dylib
External native consumer calls Kernel_Create/Update/Destroy
External native consumer validates Kernel_GetAbiInfo
nm -gU shows the required Kernel_* exported symbols on macOS
nm -gU shows GameServer_* bridge symbols on macOS
Package builder verifies ABI version mirrors and required exports
C# package compiles against Unity references
Managed ABI smoke validates kernel ABI v7, Game Server bridge ABI v1, server
entity operations, render-at-time output, projectile metadata, and NetworkHost
Unity Editor smoke runner validates ABI sizes and listen-server/host paths when
local Unity batchmode licensing/headless setup is available
```

## M6 Risks

- ABI struct changes after Unity binding work can force mirrored C# struct
  churn.
- macOS dynamic-library dependency paths may need explicit packaging guidance.
- Keeping exceptions, allocations, and ownership entirely behind the C ABI needs
  focused failure-path tests.
- Unity package scope can expand quickly; keep this milestone to runtime SDK,
  samples, managed smoke, and UPM packaging. Prefab/entity mapping, animation,
  camera, scene object registries, pooling, and UI remain demo-project scope.
- Unity Editor batchmode can be blocked by local licensing/headless setup. On
  the current development machine, Unity 6000.4.3f1 starts but licensing times
  out with `LicenseClient-kasaki doesn't exist` and
  `com.unity.editor.headless was not found`.

---

# Authoritative Weapon Current Status

The native weapon foundation is currently at `KERNEL_ABI_VERSION == 18u` with
seven weapon slots. Server-owned systems exist for DamagePipeline, YAML weapon
templates, AreaEffect, Beam, Homing v1, collision query helpers, projectile hit
processing, and internal ProjectileInteractionSystem v1.

ProjectileInteractionSystem v1 is internal-only. It can match projectile pairs
by weapon id, destroy consumed projectiles, and optionally spawn an
authoritative AreaEffect. It does not load YAML reaction catalogs, expose
reaction metadata through Kernel/GameServer APIs, emit a dedicated
ProjectileReactionEvent, or apply damage directly.

Unity validation is currently not aligned with native weapon status. The
workspace package source under `plugins/com.network-example.kernel` is missing
the expanded package files, and the packaged artifact under `plugins/output`
contains managed bindings for an older ABI. Native Bazel tests remain the
authoritative validation signal until the Unity package is restored and synced
to ABI v17.

---

# Post-M6 (Future Scope)

Not included in current scope:

- Unreal prototype
- Host migration
- Matchmaking
- Anti-cheat
- Steam integration
- Rollback netcode
- Mobile support
- Backend orchestration

---
