# Game Server v1

## Overview

Game Server v1 adds a thin authoritative gameplay layer on top of the existing
network kernel. The dedicated server starts through:

```bash
bazel run --config=macos //app:app -- --mode=dedicated_server --port=7777
```

The host/listen server path uses the same gameplay layer:

```bash
bazel run --config=macos //app:app -- --mode=host_server --port=7777
```

The `game_server` library owns enemy gameplay decisions. The kernel continues
to own entity storage, ticking, transport, snapshots, relevance filtering, and
client render state output.

## Architecture Boundaries

`game_server` is intentionally outside the kernel ABI. It talks to the kernel
only through the public server-side C API:

- `Kernel_ServerCreateEntity`
- `Kernel_ServerDestroyEntity`
- `Kernel_ServerGetEntityState`
- `Kernel_ServerSetEntityVelocity`
- `Kernel_ServerSetEntityState`
- `Kernel_ServerSubmitEntityInput`
- `Kernel_ServerQueryEntities`

`Kernel_ServerSubmitEntityInput` is an in-process native server hook for
server-owned gameplay entities. It is not exposed through the Unity managed
binding and does not require a `KERNEL_ABI_VERSION` bump.

For Unity plugin consumption, the same dylib also exposes a small
`GameServer_*` bridge ABI. That bridge owns an opaque game-server handle,
accepts polled `KernelEvent` values, ticks the native `GameServer`, exposes the
managed enemy count for smoke checks, and can despawn all managed enemies. The
bridge has its own `GAME_SERVER_ABI_VERSION` and does not change
`KERNEL_ABI_VERSION`.

## Tick Flow

The dedicated and host/listen server loops run gameplay on the same simulation
thread as the kernel owner:

1. `Kernel_Update(dt)` polls transport, processes input, advances kernel
   simulation, and publishes snapshots at the configured snapshot rate.
2. `Kernel_PollEvents()` drains kernel events.
3. `GameServer::handle_event()` receives events such as `PlayerJoined` and
   `EntityDestroyed`.
4. `GameServer::tick(dt)` runs `EnemyManager` and `EnemyAIController`.
5. Enemy AI writes velocity and animation state back through the kernel API.

Enemy velocity written in step 5 is integrated by the kernel on the next
`Kernel_Update()`. This keeps all world mutation on the server simulation
thread.

## Enemy AI Blackboard

Game Server v1 uses `AIContext` as Blackboard v1. The enemy gameplay adapter
rebuilds this tick-local read-only context before ticking the hardcoded enemy
tree. Current and planned enemy feature keys include:

- `hasVisibleEnemy`
- `hp01`
- `nearestEnemyId`
- `hasAmmo`
- `isReloading`
- `enemyDistance`
- `isAtTarget`

The blackboard should contain perception and decision inputs only. Persistent
state such as ammo counts, reload timers, fire intervals, patrol anchors, and
patrol direction remains in GameServer or kernel-owned gameplay state, then is
projected into the next tick's `AIContext` as scalar facts.

## Enemy v1 Behavior

`EnemyManager` spawns exactly one enemy after the first `PlayerJoined` event.
The initial enemy state is deterministic:

- entity type: enemy
- position: `{6, 0, 0}`
- rotation: identity
- animation: idle
- server gameplay HP: `240`
- enemy rifle damage: `5`
- enemy rifle magazine: `3`
- enemy rifle reserve ammo: `6`
- enemy rifle reload duration: about `1s`

Repeated ticks do not create additional enemies while the managed enemy exists.
If the v1 enemy is explicitly destroyed, it is not automatically respawned.

`EnemyAIController` uses the hardcoded EnemySoldier YAML tree and supports five
observable behavior bands:

- Patrol: no player is inside the 12 meter visibility range; the enemy moves
  along a deterministic route around its spawn anchor.
- Attack: a player is visible and HP is above 50%; the enemy stops moving and
  submits one rifle fire input per second toward the nearest player.
- Reload: when the enemy attack branch has no rifle ammo and reserve ammo
  remains, it submits a reload input and resumes firing after reload completes.
- Flee: a player is visible and HP is below 10%; the enemy moves away from the
  nearest player until no player is visible.
- Hold: a player is visible and HP is between 10% and 50%; the enemy stops
  moving and does not request help in v1.

Visual moving state is derived by the kernel from non-zero velocity.

## Not In v1

This version does not include:

- pathfinding or obstacle avoidance
- spawn waves or respawn policy
- random spawn tables
- config files or CLI tuning for AI constants
- Unity managed bindings for server-owned entity input or enemy ammo/debug state

## Verification

Run the focused game server tests:

```bash
bazel test --config=macos -c opt //game_server:enemy_ai_controller_test
bazel test --config=macos -c opt //game_server:enemy_manager_test
```

`enemy_manager_test` covers both listen/host server mode and dedicated server
mode.

Run the kernel regressions that cover the server entity API and replication:

```bash
bazel test --config=macos -c opt //engine/src/tests/kernel_tests:kernel_api_test
bazel test --config=macos -c opt //engine/src/tests/kernel_tests:listen_server_test
```

Build the app entrypoint:

```bash
bazel build --config=macos -c opt //app:app
```

For the standard repository verification pass, use the Bazel helper:

```bash
.agents/skills/bazel-build/scripts/run-bazel-build.sh macos test
```
