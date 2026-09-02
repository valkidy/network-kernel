# Game Server v1

## Overview

Game Server v1 adds a thin authoritative gameplay layer on top of the existing
network kernel. The dedicated server starts through:

```bash
bazel run --config=macos //app:app -- --mode=dedicated_server --port=7777
```

By default, dedicated server mode loads the Bazel-generated
`game_server/gameplay_catalog_bundle/bundle.zip` and registers it for gameplay
catalog sync. This is the preferred gameplay-data iteration flow for Unity and
native clients: update gameplay data, rebuild the bundle/app, restart the
dedicated server, and let clients sync the server bundle before handshake.

To run the legacy YAML-only flow, pass `--gameplay-catalog=path/to/catalog.yaml`
without `--gameplay-catalog-bundle`. That mode keeps catalog sync disabled and
is not the recommended dedicated gameplay test path.

The host/listen server path uses the same gameplay layer:

```bash
bazel run --config=macos //app:app -- --mode=host_server --port=7777
```

The `game_server` library owns enemy gameplay decisions. The kernel continues
to own entity storage, ticking, transport, snapshots, relevance filtering, and
client render state output.

## Gameplay Catalog Bundle

Build the default bundle through Bazel:

```bash
bazel build --config=macos //game_server/gameplay_catalog_bundle:bundle.zip
```

Dedicated server mode automatically searches these bundle locations when no
explicit `--gameplay-catalog-bundle` or legacy `--gameplay-catalog` is passed:

```text
game_server/gameplay_catalog_bundle/bundle.zip
bazel-bin/game_server/gameplay_catalog_bundle/bundle.zip
$RUNFILES_DIR/network-example/game_server/gameplay_catalog_bundle/bundle.zip
$RUNFILES_DIR/_main/game_server/gameplay_catalog_bundle/bundle.zip
<argv0>.runfiles/network-example/game_server/gameplay_catalog_bundle/bundle.zip
<argv0>.runfiles/_main/game_server/gameplay_catalog_bundle/bundle.zip
```

If none of those files exists, the dedicated server fails fast and asks for a
built bundle or an explicit `--gameplay-catalog-bundle` path. This avoids
starting a server that can run gameplay locally but cannot provide the bundle
remote clients need for catalog sync.

The compressed sync bundle has a 1 MiB hard limit. Server startup logs the
registered bundle size, estimated 32 KiB chunk count, and cache-miss protocol
bytes. Clients log whether the catalog came from cache or download and the
actual received bundle bytes. Transport framing, encryption, and retransmits
are not included in the protocol-byte estimate.

See `docs/GAMEPLAY_DATA_SYNC_POLICY.md` for the current split between
server-synced tuning data and long-lived static data.

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

That list is enforced, not just documented. `//tools:check_layering_boundaries`
reads `game_server/BUILD.bazel` and `engine/components/ai/BUILD.bazel` and fails
on a dependency outside an allowlist. The compiler already stops gameplay code
including kernel headers -- `world/public/components.h` is not on game_server's
include path -- so what the check is for is the edit that would make those
includes start working. Adding a dependency there is a decision, and it should
look like one.

### The direction the check cannot see

A dependency edge is not the only way a boundary is crossed. The other way is a
gameplay concept added to `engine/src/world/public/components.h` or to the kernel
ABI, which no dependency check will ever notice, because there is no new edge.

That is how the director machinery ended up in the kernel: `DirectorRuntime`,
`GameRuleRuntime`, `GameRuleGroupRuntime`, `GameplayGroupMembership` and the
`KernelGameRule*` ABI structs described waves, elimination conditions and spawn
shapes, none of which the kernel's own responsibilities need. Every one of them
arrived as a type added to a header, which is why no dependency check would have
caught any of it.

They have since been moved to `game_server` and deleted from the kernel
(`docs/AI_PATROL_SYSTEM.md` records the migration). The reason to move them was
not that a feature needed it. It was that the repository had come to hold two
contradictory answers to "how do I build a system that spawns things", and the
older, larger one was the one a newcomer would copy. A wrong design left in
place is not inert; it is a worked example, and it teaches.

There is no automated check for it, on purpose. Any such check is a word
blocklist plus a baseline of existing exceptions, and the baseline gets edited by
exactly the person who is about to add the next exception. So it is a review
criterion instead. Before adding a type to a kernel or world header, ask:

- **Would the kernel still need this if the game were a different game?**
  `Transform`, `Velocity`, `Health`, `WeaponState`, `ProjectileState`, colliders
  and the action system all survive that question: they are validated mutation
  and replication. "This wave is cleared", "this squad is eight strong", "spawn
  on a circle of radius R" do not.
- **Does the kernel have to interpret it, or only store it?** Something the
  kernel only stores and hands back belongs to whoever reads it. If nothing in
  the kernel reads it, it should not be there at all -- `World`'s tombstone set
  was written by the kernel and read by nothing for exactly as long as it
  existed.
- **Is this a rule or a mechanism?** A ceiling on how many of something may
  exist is a rule. A query that answers how many exist is a mechanism. The first
  belongs in `game_server`; the second belongs in the kernel and should be
  generic enough that the kernel does not know what it is counting.

The worked example is the patrol system: its grouping, budget, cadence,
composition and routing are all `game_server`, its configuration goes through the
catalog and the catalog hash, and it needed no kernel ABI change at all -- not
even for navigation.

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
