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
| M3 | Real Network Transport | Pending |
| M4 | Authoritative Combat | Pending |
| M5 | Lag Compensation & Smoothing | Pending |
| M6 | Engine Plugin Boundary | Pending |

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

Status: Pending

## Goal

Integrate GameNetworkingSockets.

## M3.1 Server Transport

### Tasks

Complete:

```text
GnsTransport::StartServer()
```

Support:

- Accept connection
- Disconnect
- Peer mapping

### Acceptance

- Server accepts remote clients

---

## M3.2 Client Transport

### Tasks

Complete:

```text
GnsTransport::StartClient()
```

### Acceptance

- Client connects to server successfully

---

## M3.3 Session Protocol

### Tasks

Implement packets:

- Handshake
- Welcome
- Disconnect
- PingPong

### Acceptance

- Version mismatch is rejected
- Disconnect is handled correctly

---

## M3.4 Input Replication

### Tasks

Client sends:

```text
InputPacket
```

Server consumes input.

### Acceptance

- Remote player movement works

---

## M3.5 Snapshot Replication

### Tasks

Server sends:

```text
SnapshotPacket
```

Client receives snapshots.

### Acceptance

- Remote authoritative state is visible

## Deliverables

```text
//app/dedicated_server
//app/example_client
```

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

- RTT
- inputSeq
- clientTick

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

---

# M6 — Engine Plugin Boundary

Status: Pending

## Goal

Package the kernel as an engine plugin.

## M6.1 C ABI Stabilization

### Tasks

Complete:

- kernel_api.h
- kernel_types.h
- Opaque handles

### Acceptance

- No STL types across ABI

---

## M6.2 Dynamic Library

### Tasks

Build:

```text
libnetwork_kernel.dylib
```

### Acceptance

- External applications can link successfully

---

## M6.3 Export Interfaces

### Tasks

Complete:

- Kernel_Update()
- Kernel_SubmitInput()
- Kernel_GetRenderStates()
- Kernel_PollEvents()

### Acceptance

- Stable memory ownership

---

## M6.4 Unity Prototype

### Tasks

Complete:

- C# P/Invoke wrapper

### Acceptance

- Unity reads render states successfully

---

## M6.5 Unreal Prototype

### Tasks

Complete:

- Unreal module wrapper

### Acceptance

- Unreal actor synchronization works

## Deliverables

```text
Dynamic library
C ABI
Unity wrapper
Unreal wrapper
```

---

# Post-M6 (Future Scope)

Not included in current scope:

- Host migration
- Matchmaking
- Anti-cheat
- Steam integration
- Rollback netcode
- Mobile support
- Backend orchestration

---