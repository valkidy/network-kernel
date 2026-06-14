# Netcode Sync Policy

This document summarizes the runtime sync model used by the network kernel.
It follows the client-server architecture described by Gabriel Gambetta's
Fast-Paced Multiplayer series: the server owns the world, clients send inputs,
and clients hide latency with prediction and interpolation.

## Core Flow

The game runs as an authoritative client-server simulation:

```text
Clients send inputs to the server.
Server processes inputs and updates world state.
Server sends regular world snapshots to clients.
Clients predict local actions immediately.
Clients reconcile local prediction with server snapshots.
Clients interpolate known past snapshots for remote entities.
```

The server is the only authority for gameplay results:

```text
damage
hit result
death
score
authoritative entity state
```

Clients may predict presentation and local responsiveness, but they do not
decide gameplay consequences.

## Two Timelines

Clients render two timelines at the same time:

```text
Local timeline:
    the local player and local-owned predicted projectiles
    are shown near the present.

World timeline:
    remote entities are shown slightly in the past,
    using snapshot interpolation.
```

From the player's point of view:

```text
The player sees himself in the present.
The player sees other entities in the past.
```

This is intentional. It keeps local controls responsive while keeping remote
movement smooth despite network delay.

## Local Player

Local player movement uses prediction and reconciliation.

When the local player sends input:

```text
1. Client sends PlayerInput to the server.
2. Client immediately simulates the input locally.
3. Server processes the input authoritatively.
4. Server snapshots include the last processed input sequence.
5. Client drops acknowledged inputs.
6. Client replays unacknowledged inputs from the authoritative state.
```

Server state is used as a correction source, not as direct local render state.

## Remote Entities

Remote entities are not predicted by this client.

They use snapshot interpolation:

```text
1. Client stores received world snapshots.
2. Client chooses a render time slightly behind the server timeline.
3. Client interpolates between known snapshots.
4. Client renders that past world state.
```

This applies to remote players, enemies, and remote projectiles that are
replicated through snapshot sections. Snapshot sections are byte-budgeted by
encoded size, not by entity count:

```text
Actor:
    player, enemy, and future AI bot render state
    entity_type identifies the actor kind
    hp/max_hp is optional and omitted actors decode as HpUnknown

ProjectileCompact:
    net_id + position + velocity + state + flags
    used for server_snapshot_only projectile render interpolation

ProjectileHybridCorrection:
    compact projectile fields + owner_peer + spawn_tick + client_action_id
    used when snapshot data corrects a predicted deterministic projectile
```

Remote deterministic projectiles may also be event-spawned and rendered by
deterministic local presentation simulation from spawn metadata. In that mode,
the reliable spawn/despawn stream owns lifecycle while compact low-frequency
projectile snapshots remain optional correction data.

## Local-Owned Deterministic Projectiles

Rocket and grenade projectiles are deterministic projectiles.

When the local player fires one:

```text
1. Client immediately creates a predicted projectile.
2. Client sends PlayerInput with InputButton_Fire and client_action_id.
3. Server validates the fire action.
4. Server creates the authoritative projectile.
5. Server snapshots include projectile net_id, owner_peer, velocity,
   spawn_tick, and client_action_id.
6. Owner client binds the predicted projectile to the server projectile by
   owner_peer + client_action_id.
```

The owner client must not permanently render the raw snapshot position as the
current projectile position.

Instead:

```text
1. Treat the snapshot position as authoritative past state.
2. Fast-forward that state to the local prediction timeline.
3. Use the fast-forwarded state as the correction target.
4. Smooth small visual error with a render correction offset.
5. Snap only when the error is too large.
```

Remote clients do not do fire prediction for that projectile. They either:

```text
Option A:
    receive the reliable projectile spawn event,
    simulate deterministic render motion from spawn metadata,
    end presentation from lifetime/despawn.

Option B:
    receive compact or hybrid-correction projectile snapshots,
    render from the delayed interpolation timeline.
```

The chosen remote path is independent from lifecycle truth. Missing an
unreliable projectile snapshot is not a despawn signal.

## Server Projectile Authority

The server owns projectile gameplay state and damage.

For deterministic projectile state:

```text
1. Convert the client action time onto the server timeline.
2. Clamp it to the accepted compensation window.
3. Use that compensated time as the projectile spawn time.
4. Evaluate the deterministic projectile path directly at server now.
```

The server must not replay missed projectile movement frames to produce the
current transform. It fast-forwards the projectile state from spawn time to the
current server tick.

Projectile hit validation may use rewind:

```text
historical hitbox snapshots
+ deterministic projectile path segment
+ server-authoritative damage resolution
```

Rewind is for hit queries and damage validation only. It is not a visual-state
or transform replay mechanism.

## Hitscan

Hitscan weapons do not create synchronized projectile entities.

The client may play immediate local feedback:

```text
muzzle flash
recoil
tracer
sound
```

The server performs authoritative hit detection, including lag compensation
when available, and sends the resulting gameplay events.

## Physics Projectiles

Non-deterministic physics projectiles are future work.

Until a physics module exists, they should not use client rollback physics.
The intended future policy is:

```text
authoritative snapshots
+ render-side soft correction
```

## Practical Rule

Use this decision order:

```text
Local-owned and predictable:
    predict now, reconcile later.

Remote or not locally owned:
    interpolate past snapshots.

Gameplay consequence:
    trust only the server.

Pure cosmetic feedback:
    play locally.
```

The goal is simple:

```text
local actions feel immediate,
remote entities look smooth,
server remains authoritative.
```
