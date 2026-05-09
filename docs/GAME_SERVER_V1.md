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
- `Kernel_ServerQueryEntities`

The current v1 implementation does not add or rename kernel API symbols.

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

## Enemy v1 Behavior

`EnemyManager` spawns exactly one enemy after the first `PlayerJoined` event.
The initial enemy state is deterministic:

- entity type: enemy
- position: `{6, 0, 0}`
- rotation: identity
- animation: idle

Repeated ticks do not create additional enemies while the managed enemy exists.
If the v1 enemy is explicitly destroyed, it is not automatically respawned.

`EnemyAIController` supports two states:

- Idle: no target in chase range, velocity is zero, animation state `0`.
- Chasing: nearest player is within 12 meters, velocity points toward that
  player at 2.5 meters per second, animation state `1`.

Visual moving state is derived by the kernel from non-zero velocity.

## Not In v1

This version does not include:

- attacks or damage decisions
- pathfinding or obstacle avoidance
- spawn waves or respawn policy
- random spawn tables
- config files or CLI tuning for AI constants
- new kernel ABI surface

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
