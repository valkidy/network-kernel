# Game Server API Scaffold

This document marks the C++ kernel surface prepared for a later Game Server v1
task. The current task does not implement EnemyManager or EnemyAI inside the
kernel.

## Boundary

The game server layer should own gameplay policy:

- enemy spawn rules
- enemy despawn rules
- target selection
- AI state transitions
- movement decisions

The kernel owns authoritative world mutation, replication, transport, snapshots,
and render/query data.

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

## Follow-Up Game Server v1 Task

The next task can add a `game_server` layer that calls the prepared kernel API:

- `EnemyManager` calls entity create/destroy.
- `EnemyAIController` queries players/entities.
- AI writes velocity, transform, animation state, and visual flags back to the
  kernel.
- Unity remains presentation-only and does not run AI or network tick logic.
