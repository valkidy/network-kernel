# Game Server API Scaffold — Historical Record

Status: **Implemented and superseded**

This file records the server-side API scaffold that preceded Game Server v1. It
is retained because older tasks and commits reference this path. It is not the
current game-server behavior or complete Kernel server API reference.

Use:

- `docs/GAME_SERVER_V1.md` for current game-server runtime behavior;
- `docs/NETWORK_KERNEL_ABI.md` for the public ABI contract;
- `engine/src/kernel/public/kernel_api.h` for the authoritative function list;
- `docs/HYBRID_AI_TREE_FRAMEWORK_IMPLEMENTATION.md` for current AI integration.

## Historical Contribution

The scaffold established the boundary that still applies:

```text
game_server owns gameplay policy and AI interpretation
kernel/simulation owns world state, fixed-tick mutation, physics,
combat, transport, replication, prediction, and render/query output
```

It introduced generic server-only operations for:

- entity creation and destruction;
- transform and velocity writes;
- persistent entity-state writes;
- single-entity and filtered entity queries.

These functions provided the first external gameplay layer without introducing
enemy-specific types into the kernel ABI.

## Current Server API Families

The current ABI extends the original scaffold with:

| Capability | Representative API |
|---|---|
| Lifecycle | `Kernel_ServerCreateEntity`, `Kernel_ServerDestroyEntity` |
| State/query | `Kernel_ServerGetEntityState`, `Kernel_ServerQueryEntities` |
| Transform/movement | `Kernel_ServerSetEntityTransform`, `Kernel_ServerSetEntityVelocity` |
| Health/combat | `Kernel_ServerSetEntityHealth`, `Kernel_ServerSetEntityCombatState` |
| Actor metadata | `Kernel_ServerSetEntityActorTemplate` |
| Perception | `Kernel_ServerSetEntityVisionConfig`, `Kernel_QueryVisionState` |
| Weapons | `Kernel_ServerSetEntityWeaponMechanics`, `Kernel_ServerGetEntityWeaponMechanics` |
| Server-owned actor control | `Kernel_ServerSubmitEntityInput` |

All world mutation remains server-only, C ABI-safe, and single-owner. Gameplay
code does not build snapshots or send replication packets directly.

## Current Game-Server Flow

```text
Kernel_Update
  -> Kernel_PollEvents
  -> AgentRuntimeManager / Director bootstrap
  -> AiPerceptionAdapter
  -> AgentSentryController
  -> ActorIntentExecutor
  -> Kernel_ServerSubmitEntityInput or validated server API
```

The game server now supports:

- gameplay catalog v2 and pre-handshake bundle synchronization;
- data-driven action, weapon, projectile, entity, and collider templates;
- server-only Director entities and entity-template-driven spawn policy;
- generic AI intents with game-server perception/execution adapters;
- sentry attack/reload through the authoritative action/weapon path;
- Jolt-based static collision, grounding, and character movement;
- Unity-facing `GameServer_*` bridge packaging.

## Replication Contract

The game-server layer receives these kernel-owned behaviors:

- reliable entity lifecycle;
- authoritative snapshot and render-state output;
- per-client relevance filtering;
- gameplay catalog synchronization;
- owner prediction correction and remote action presentation;
- entity-template and collider metadata synchronization;
- out-of-range lifecycle handling that preserves the client session.

## Historical Limitations No Longer Current

The original scaffold described Game Server v1 as future work and excluded
attacks, damage, AI runtime, and navigation/physics integration. Those statements
must be read only as historical scope. Current sentry agents use the native
weapon path, generic AI adapters exist, Director entities are implemented, and
Jolt/Recast artifacts are part of the gameplay bundle.

Production pathfinding, encounter/mission directors, squad tactics, and
client-side enemy prediction remain outside the current runtime.
