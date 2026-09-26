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
1. Client sends KernelPlayerInput to the server.
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

### Starved actors

A snapshot is a send set, not the world. Past the budget most actors are left
out of most snapshots (measured at 80 acting agents: half of every agent's
snapshot intervals, even within 10 m), so interpolating between the two
buffered snapshots around the render time has nothing to say about them.

Remote actors are therefore drawn from their own samples:

```text
1. Find the actor's own previous and next sample anywhere in the buffer.
2. Both known:
       interpolate position, rotation and velocity across the gap;
       keep flags, hp and the action timeline from the older sample
       until the last snapshot interval before the newer one.
3. Only the previous sample known:
       carry it along its last horizontal velocity for at most 0.25 s,
       then hold. Never extrapolate vertically or while dead.
4. Only a newer sample known:
       the actor just became relevant; draw it at that sample.
```

Props and projectiles keep the pairwise result: a prop's moves are pickups and
placements, which interpolating across a gap would draw as a slide.

### Knockback anchors

While an `ImpulseLockout` stands, the authority ignores the actor's controller:
horizontal velocity carries and gravity pulls until the actor lands or the
lockout expires. The state at the end of the tick it was struck is therefore
the whole flight, and it is the tick the send set is least likely to cover.

```text
Server, the tick an impulse arms a lockout:
    ActorImpulseBatch on the reliable-event channel, relevance-filtered,
    never sent to the actor's own owner:
        position, velocity, gravity, height it was struck from,
        lockout ticks remaining.

Client:
    replay the flight with knockback_flight_position_at
        (the solver's per-tick sum, exact on every tick);
    end it with knockback_flight_ticks
        (first landing, or the lockout ceiling for a flat slide);
    re-base on any later in-flight snapshot sample and bend toward it,
        so a collision the replay cannot know is absorbed, not snapped;
    after the flight, hold where it came down until a later sample arrives.
```

The owner predicts its own knockback from the lockout block in its own
snapshot record instead.

### Endings on the world timeline

Reliable records arrive about one interpolation delay before the world
timeline reaches the tick they describe. A record that ends or starts
something drawn on that timeline takes effect when the render instant reaches
its tick, not when it arrives:

```text
Thrown prop, flight ended (landed, caught, placed):
    the new state is applied, but the flight keeps being drawn from its
    anchor, and reported InFlight, until the render instant reaches the tick.

Thrown prop or server-only projectile, destroyed or expired:
    the whole despawn -- removal, EntityDestroyed, the lifecycle event the
    view is removed on -- is held until the render instant reaches the tick.
    Leaving relevance is not an ending and is applied at once.

Server-only projectile spawned (a bottle's blast, a remote melee hit):
    not drawn until the render instant reaches its spawn tick.
    The local player's own are drawn at once.
```

A thrown prop the client can anchor is also left out of the snapshot send set
while it is in flight: the render pass never uses those samples, and the slot
goes to an actor.

## Local-Owned Deterministic Projectiles

Rocket and grenade projectiles are deterministic projectiles.

When the local player fires one:

```text
1. Client immediately creates a predicted projectile.
2. Client sends KernelPlayerInput with InputButton_Fire and client_action_id.
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

## Render Clock and AI Movement Intent (Next)

Status: the next piece of work after the endings above are validated in play.
The render clock comes first; AI movement intent only if measurement still
calls for it afterwards. The implementation plan, together with the related
combat-event, presentation-budget, projectile-collision and local-throw items,
is `docs/REMOTE_PRESENTATION_NEXT_IMPLEMENTATION_PLAN.md`.

### Two kinds of waiting for snapshots

```text
One entity missing from snapshots, the stream still arriving:
    the send set had no slot for it. Handled: starved actors are drawn from
    their own samples, knockback flights and thrown props from their anchors.

The whole stream late by more than the interpolation delay:
    network jitter, loss, a biased clock-offset estimate, a server hitch.
    Not handled.
```

The second stalls everything at once. `client_render_server_time_us` clamps
the render instant to the newest buffered snapshot, and every world-timeline
consumer reads that one instant: interpolation, knockback flights, thrown-prop
anchors, and the endings held back for it. Anchored motion needs no new samples
to be drawn, but it still asks the render clock what time it is, so it stops
with everything else and jumps when the next snapshot arrives (measured on a
thrown prop: held at 24.0 m, then 27.2 m in one frame).

### Planned: let the one render clock run ahead, bounded

```text
1. Let the shared render instant pass the newest snapshot by at most a cap
   (start from the 0.25 s actors are already extrapolated for). Everything
   that can advance without samples keeps moving: anchors, knockback flights,
   predicted projectiles, actor extrapolation, and held-back endings are
   released on the same clock.
2. When snapshots resume, never step the clock backwards: run it slightly
   slow until the buffer is ahead of it again.
3. Size the interpolation delay from measured arrival jitter (jitter_us)
   instead of a fixed two snapshot intervals.
```

It has to be the one clock. Advancing a single consumer on its own breaks
ordering: a thrown bottle given its own unclamped time flies past the point
where its destroy -- still waiting on the clamped clock -- removes it.

Client only; no ABI or packet change.

### Planned, gated: AI movement intent

Replicate what an agent is doing (route, destination, speed) instead of where
it is, so the client moves it between samples. Gates, in order:

```text
1. In-play validation shows the remaining jitter is actor motion, not stalls.
2. A benchmark measures drawn-versus-true position per distance band at
   40 / 80 / 200 moving, turning agents.
3. The cheap fixes are tried first: Hermite interpolation across a gap using
   both samples' velocities (client only), and a larger per-player snapshot
   budget (already server-selectable).
```

Scope, which is why it is gated: the kernel has no notion of intent -- routes
live in `game_server` (`patrol_navigation`, `patrol_director`, the chaser) and
the kernel sees one move input per agent per tick. It needs a new C API for
`game_server` to hand intent to the kernel (ABI bump, both export lists, the
Unity managed mirrors), controller changes, a packet schema bump, and
client-side route following. Order by how rarely intent changes: patrol
routes, then sentries, chasers last.

What it does and does not buy:

```text
With the clock above, intent makes a late stream mostly invisible: an agent
keeps walking its route. Without it, intent still stalls like everything else.

A change inside a late window -- a turn, a hit, a death -- arrives already in
the past and becomes a correction instead of a stall. Intent travels on the
reliable channel, so under packet loss head-of-line blocking can make that
correction later than a snapshot would have been.

The interpolation delay stops being a hard requirement and becomes a buffer
that trades latency for fewer corrections.
```

## Homing Projectiles (Deferred)

Status: deferred until a homing weapon is designed and equipped. The
`homing_missile` projectile, weapon, and fire/reload actions are authored, but
no player or agent loadout uses them, so the work below cannot be validated in
play yet.

Current behavior, which is known to jitter under crowd load:

```text
Server:
    boost straight for boost_ticks, then lock the nearest valid target in the
    lock cone, turn at most max_turn_degrees_per_tick toward it, accelerate to
    max_speed, and fly straight once the target is lost.

Client:
    projectile_position_at has no homing branch, so a remote homing projectile
    is drawn as a straight line from its spawn record and pulled onto the real
    path only by hybrid snapshot corrections (straight-line extrapolation,
    capped at 0.2 s). Projectiles carry 1/8 of an actor's send weight, so in a
    crowd a correction can be up to a second apart: straight, then a snap.
```

Planned when the weapon exists, following the knockback anchor pattern:

```text
1. Server sends a reliable guidance record only when the guidance phase
   changes (lock-on, target lost, retarget): projectile, tick, phase,
   target net_id, position, velocity. Two or three per missile.
2. Client steers between records with the server's own turn-rate and
   acceleration rules, shared as a simulation function like
   knockback_flight_position_at, toward the target's position.
3. Hybrid snapshot corrections and the existing correction offset absorb
   the remaining error.
4. Packet schema bump; server and client rebuilt together.
```

Known limit to design around: the replay cannot be exact. A remote homing
projectile is fast-forwarded to the present while a remote target is drawn
about one interpolation delay in the past, so steering toward the drawn target
aims at the wrong instant. A local-player target is predicted in the present
and lines up, which is also the case players notice most.

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
