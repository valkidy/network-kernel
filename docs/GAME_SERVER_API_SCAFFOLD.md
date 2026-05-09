# Game Server API Scaffold

This document marks the C++ kernel surface prepared for a later Game Server v1
task. The current task does not implement EnemyManager or EnemyAI inside the
kernel.

## Status

M7 is a handoff milestone only. The kernel now exposes the generic server-side
API needed by an external game server layer, but this repository still does not
contain a `game_server/` implementation.

## Boundary

The game server layer should own gameplay policy:

- enemy spawn rules
- enemy despawn rules
- target selection
- AI state transitions
- movement decisions

The kernel owns authoritative world mutation, replication, transport, snapshots,
and render/query data.

Do not add enemy policy, AI decision trees, spawn pacing, or attack behavior to
the kernel. Those belong in the next Game Server v1 task.

## Prepared Kernel API

The public C ABI now exposes generic server-side operations for a future game
server layer:

- create a server entity with `Kernel_ServerCreateEntity`
- destroy a server entity with `Kernel_ServerDestroyEntity`
- write authoritative transform with `Kernel_ServerSetEntityTransform`
- write authoritative velocity with `Kernel_ServerSetEntityVelocity`
- write persistent render/gameplay state with `Kernel_ServerSetEntityState`
- query one entity with `Kernel_ServerGetEntityState`
- query entities by type with `Kernel_ServerQueryEntities`

These functions are server-only. They fail in client mode and use C ABI-safe
structs only.

## Intended Server Tick Flow

The future dedicated server process should keep world mutation on one simulation
thread:

```text
Kernel_Update()
Kernel_PollEvents()
EnemyManager::Tick()
  -> query players/entities
  -> create/destroy enemies
  -> write enemy velocity/transform/state
```

The kernel publishes replicated snapshots and relevance updates from its own
update path. The game server layer should not build snapshots or send network
messages directly.

## API Mapping For Game Server v1

Use this mapping when implementing the follow-up task:

| Game Server Need | Kernel API |
|---|---|
| Spawn enemy | `Kernel_ServerCreateEntity` with `EntityType::kEnemy` |
| Despawn enemy | `Kernel_ServerDestroyEntity` |
| Move enemy by velocity | `Kernel_ServerSetEntityVelocity` |
| Teleport/correct enemy transform | `Kernel_ServerSetEntityTransform` |
| Set idle/chasing animation state | `Kernel_ServerSetEntityState` |
| Find nearest player | `Kernel_ServerQueryEntities` with player type |
| Read enemy/player state | `Kernel_ServerGetEntityState` |

`Kernel_ServerCreateEntity` currently supports enemy creation for this scaffold.
Additional server-created entity types should be added only when a concrete game
server feature needs them.

## Replication Behavior Already Provided

The game server layer gets these behaviors without owning replication logic:

- authoritative entity lifecycle flows through reliable spawn/despawn
- server-written transform, velocity, animation state, and visual flags are
  exported through snapshots/render states
- persistent state flags are replicated through `RenderEntityState`
- per-client AOI filtering controls enemy/projectile relevance
- out-of-range entities despawn with `KernelDespawnReason_OutOfRange`

## Follow-Up Game Server v1 Task

The next task can add a `game_server` layer that calls the prepared kernel API:

- `EnemyManager` calls entity create/destroy.
- `EnemyAIController` queries players/entities.
- AI writes velocity, transform, animation state, and visual flags back to the
  kernel.
- Unity remains presentation-only and does not run AI or network tick logic.

## Follow-Up Checklist

- Add a server-side `EnemyManager` outside `engine/src/kernel`.
- Keep `Enemy` as game-server-owned bookkeeping with `net_id`, target, and AI
  state.
- Spawn enemies through `Kernel_ServerCreateEntity`; store returned `net_id`.
- Query players with `Kernel_ServerQueryEntities` each tick or from a cached
  player list updated by kernel events.
- Implement v1 AI states only: idle and chasing.
- Write velocity through `Kernel_ServerSetEntityVelocity`; use transform writes
  only for corrections or spawn placement.
- Write animation state/visual flags through `Kernel_ServerSetEntityState`.
- Despawn through `Kernel_ServerDestroyEntity`; remove local bookkeeping after
  success.
- Verify clients see enemy spawn/despawn/movement through existing render state
  and snapshot replication.

## Out Of Scope For The Follow-Up Scaffold

- Unity plugin or C# binding changes
- client-side enemy prediction
- attacks, damage, or melee behavior
- navigation mesh/pathfinding
- new network transports
- kernel-owned EnemyManager or EnemyAI classes
