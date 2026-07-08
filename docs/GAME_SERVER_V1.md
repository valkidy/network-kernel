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
4. `GameServer::tick(dt)` runs `AgentRuntimeManager` and
   `AgentSentryController`.
5. Agent sentry behavior reads kernel vision state and writes stationary
   velocity, animation state, and fire input requests back through the kernel
   API.

Agent velocity written in step 5 is integrated by the kernel on the next
`Kernel_Update()`. This keeps all world mutation on the server simulation
thread.

## Agent Vision And Sentry

Game Server v1 no longer uses the old hardcoded actor behavior tree. The stable
perception core lives behind the kernel `Kernel_QueryVisionState` ABI, while
the temporary game-server behavior is an Agent-named stationary sentry.

`KernelVisionStateView` is perception data only: visible hostile/ally lists,
current target candidate, last seen target, last known target position, vision
collider template id, and resolved actor collider template id. Visual debugger
code can read the cone dimensions from `Kernel_GetColliderTemplates` using the
vision collider template id. It does not contain sentry behavior state or final
attack decisions.

## Agent v1 Behavior

`AgentRuntimeManager` bootstraps configured server-only Director entities after
the first `PlayerJoined` event. Director AI owns initial and replacement spawn
work. The default sentry agent state is deterministic:

- entity type: actor
- actor type: agent
- camp: enemy side
- position: `{6, 0, 0}`
- rotation: identity
- animation: idle
- server gameplay HP: `240`
- enemy rocket damage: `5`
- enemy rocket magazine: `3`
- enemy rocket reserve magazines: `6`
- enemy rocket reload duration: about `1s`

Repeated ticks do not create additional enemies while the managed enemy exists.
If the v1 enemy is explicitly destroyed, it is not automatically respawned.

`AgentSentryController` supports three observable behavior bands:

- Idle: the agent is stationary and has no visible target.
- Alert: a visible hostile has been observed; if it remains visible for 3
  seconds, the agent enters Attack. If the target is unseen for 5 seconds, the
  agent returns to Idle.
- Attack: the agent remains stationary and requests fire through the existing
  weapon input path while vision still reports a target. Weapon cooldown,
  mechanics, projectile spawning, and damage stay in kernel weapon systems.

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
bazel test --config=macos -c opt //game_server:agent_sentry_controller_test
bazel test --config=macos -c opt //game_server:agent_runtime_manager_test
```

`agent_runtime_manager_test` covers both listen/host server mode and dedicated server
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
