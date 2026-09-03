# AI Framework Refactor Requirements

Status: **MVP implemented; retained as architecture requirements**

The completed implementation is summarized in
`docs/HYBRID_AI_TREE_FRAMEWORK_IMPLEMENTATION.md`. Normative ownership and
dependency rules in this file remain current. Sections that describe the
"first refactor" record the implemented migration sequence rather than pending
work.

## 1. Goal

Build a generic AI framework that can support both actor-level AI and
world/director-level AI without hardcoding project identities such as player,
enemy, pal, or NPC into the engine AI layer.

The framework must support:

- Per-actor AI, such as sentry, combat agent, companion, pal, or assisted player.
- World/director AI, such as spawn director, encounter controller, mission
  director, or pacing director.
- Hardcoded runtime flow first, with a path toward node registries,
  data-driven graph topology, and designer-authored behavior assets later.
- Intent-based execution, where the AI framework decides what it wants to do,
  while `game_server` interprets how that intent maps to gameplay.

The central rule is:

```text
AI framework produces scoped intents.
game_server interprets scoped intents.
Gameplay systems own validated mutation.
```

## 2. Current Codebase Constraints

The current repository already has important boundaries that this refactor must
preserve.

`engine/components/ai` is an independent generic library. It currently depends
only on standard C++ and `yaml-cpp`, and boundary tests forbid it from depending
on `engine/src`, `game_server`, or `app`.

Therefore:

- `engine/components/ai` must not include kernel, world, simulation, game server,
  transport, Unity, app, or gameplay-specific headers.
- `engine/components/ai` must not output `PlayerInput`, `ActorInput`,
  `SimulationCommand`, `KernelVec3`, `KernelHandle`, or game-specific entity
  structures.
- `engine/src` modules should not depend on `engine/components/ai`.
- Gameplay integration should live in `game_server` or a future adapter package
  outside `engine/src`.

The current codebase already has:

- `PlayerInput`, which acts as the current concrete actor input type.
- `simulation::Command`, which supports both `kSubmitInput` and world mutation
  commands such as create, destroy, transform, velocity, and state changes.
- `KernelCommandSource`, which already distinguishes internal, player input,
  AI, control plane, and test command sources.
- Kernel RPC authority/phase metadata for control-plane world mutation.
- A sentry controller that already converts AI fire/reload behavior into
  `PlayerInput` with `KernelCommandSource_AI`.

These existing pieces should be treated as the migration foundation rather than
replaced wholesale.

## 3. Core Principles

### 3.1 Single Owner of Gameplay Truth

Gameplay truth must have exactly one authoritative owner.

Examples:

- Position, velocity, and facing belong to gameplay state.
- HP and alive state belong to combat/actor runtime.
- Ammo, cooldown, and reload timers belong to weapon/gameplay systems.
- Camp, faction, and relationship are gameplay facts.

AI blackboard data may cache read-only facts for a tick or decision window, but
it must never become a second source of truth.

Bad:

```cpp
blackboard.ammo = 30;
weapon_runtime.ammo = 12;
```

Good:

```cpp
blackboard.weapon_status = ReadWeaponStatusFromGameplayState();
```

### 3.2 Intent Before Gameplay Mutation

The AI decision layer does not mutate gameplay state. It outputs intent.

The executor/adapter layer validates that intent and translates it into the
appropriate execution path.

```text
AI Decision -> Intent -> game_server Executor -> ActorInput or SimulationCommand
```

### 3.3 Scoped Output Paths

AI framework output must not be restricted to `PlayerInput`.

The correct rule is:

```text
Actor-level AI output should default to an ActorInput / PlayerInput-like pipeline.
AI framework output as a whole must not be limited to PlayerInput.
```

Actor-level intent examples:

- `AttackTarget(target_id)`
- `Reload()`
- `MoveTo(position)`
- `LookAt(target_id)`
- `UseAbility(slot)`
- `RetreatFrom(target_id, desired_distance)`

These should default to:

```text
Intent -> game_server ActorIntentExecutor -> ActorInput -> simulation kSubmitInput
```

World/director-level intent examples:

- `SpawnAgent(spawn_rule_id)`
- `DespawnGroup(group_id)`
- `StartEncounter(encounter_id)`
- `AssignObjective(group_id, objective_id)`
- `ChangeMissionPhase(phase_id)`

These should use:

```text
Intent -> game_server DirectorIntentExecutor
       -> validated, authority-controlled SimulationCommand or gameplay request
```

The AI framework defines the intent language. `game_server` decides the concrete
execution path.

### 3.4 No Empty Semantic Functions

If a requested semantic capability has no supporting gameplay data, query,
system, or executor, do not implement an empty placeholder.

Return an unsupported capability report instead.

Examples of unsupported capabilities until the codebase has supporting systems:

- `FindCover`
- `FlankTarget`
- `RequestBackup`
- `AssignGroupObjective`
- `NearestFriendlyAgent`

The report should distinguish:

- Missing query
- Missing gameplay system
- Missing executor
- Missing data
- Missing action support

### 3.5 Data-Driven Ready, Not Data-Driven First

The first refactor should not start with a full designer-authored graph system.

Recommended order:

```text
hardcoded runtime flow
  -> generic framework types
  -> node / executor registries
  -> data-driven graph topology
  -> designer-authored behavior assets
```

Names and boundaries must be data-driven ready from the beginning, but the MVP
may keep behavior flow hardcoded.

## 4. Layer Responsibilities

### 4.1 `engine/components/ai`: Generic AI Framework

This layer owns AI abstractions only.

Responsibilities:

- Blackboard / context abstraction
- Intent abstraction
- Intent status
- Runtime status
- Decision node status
- Scheduler
- Capability registry
- Executor interface definitions
- Node/action registry definitions
- Data-driven behavior loading and validation, when introduced

Forbidden in this layer:

- `PlayerInput`
- `ActorInput`
- `SimulationCommand`
- `KernelHandle`
- `KernelVec3`
- `World`
- `entt`
- `game_server`
- `enemy`, `player`, `pal`, `weapon template id`, `projectile type`
- Unity prefab or app-specific details

This layer may define generic ids and generic values:

```cpp
using EntityId = std::uint32_t;
using RuntimeId = std::uint32_t;
```

But it must not assume what those ids mean in gameplay.

### 4.2 `engine/src/kernel` and `engine/src/simulation`: Gameplay Truth and Execution

This layer owns authoritative gameplay mutation.

Responsibilities:

- World state
- Actor transform, velocity, health, weapon state, reload, cooldown, and alive state
- Vision state generation
- Actor input execution
- Simulation command queue and command dispatch
- Damage, projectile, beam, area effect, and weapon simulation
- Authority and phase validation for control-plane mutation

This layer must not know about AI decision trees or behavior graph semantics.

### 4.3 Vision System

Vision is a gameplay/system truth provider, not an AI decision layer.

It belongs in `engine/src/kernel` / `engine/src/simulation` because it reads:

- World state
- Entity transforms
- Colliders
- Vision templates
- Camp/faction configuration
- Relationship classification

It outputs authoritative vision facts such as:

- Visible hostiles
- Visible allies
- Current target candidate
- Last seen target
- Last known target position
- Relation to target

### 4.4 `game_server`: Gameplay AI Adapter Layer

This layer interprets generic AI concepts using concrete gameplay systems.

Responsibilities:

- Build AI facts from kernel/gameplay queries.
- Translate vision and gameplay state into blackboard/context facts.
- Implement actor-level intent executors.
- Implement director/world-level intent executors.
- Validate target, weapon, spawn, objective, and encounter rules.
- Convert actor-level intents to `ActorInput`.
- Convert director/world-level intents to authorized gameplay requests or
  `SimulationCommand`.
- Own concrete runtime instances such as sentry, combat agent, companion, spawn
  director, and encounter director.

Example future components:

- `GameServerPerceptionAdapter`
- `CombatIntentExecutor`
- `DirectorIntentExecutor`
- `SentryAgentRuntime`
- `CompanionAgentRuntime`
- `SpawnDirectorRuntime`
- `AgentSpawnDirector`

## 5. ActorInput and PlayerInput Rename

The current concrete type is named `PlayerInput`, but the intended concept is
broader than a human player.

The refactor should introduce the concept of `ActorInput`.

Definition:

```text
ActorInput is the authoritative per-actor control input consumed by simulation.
```

It should represent:

- Movement intent
- Look/facing/aim intent
- Fire/reload/ability buttons
- Selected weapon or action slot
- Client or AI action metadata when needed

Migration requirement:

- Short term: keep `PlayerInput` ABI-compatible and treat it as the current
  concrete `ActorInput` implementation.
- Medium term: introduce `ActorInput` naming in internal C++ APIs where ABI churn
  is acceptable.
- Long term: rename public ABI only through a versioned compatibility plan.

Do not block the AI refactor on a full public ABI rename. The AI design should
use the term `ActorInput`, while adapters may still fill the existing
`PlayerInput` struct.

## 6. Intent Model

### 6.1 Intent Scope

Every intent has a scope.

Required initial scopes:

```cpp
enum class IntentScope {
    kActor,
    kDirector,
    kWorld,
};
```

Actor-scoped intents target one controllable actor.

Director-scoped intents coordinate gameplay at a group, encounter, or pacing
level.

World-scoped intents request world mutation such as spawning or despawning.

### 6.2 Intent Context

Intent context describes behavior domain, not identity.

Prefer:

- `CombatContext`
- `CompanionContext`
- `DirectorContext`
- `SpawnDirectorContext`

Avoid:

- `EnemyContext`
- `PlayerContext`
- `PalContext`

Enemy/player/pal are gameplay roles, camp, relationship, or controller source.
They should not become generic framework types.

### 6.3 Intent Status

Executors must report status because execution can span multiple ticks.

Required statuses:

```cpp
enum class IntentStatus {
    kRunning,
    kSucceeded,
    kFailed,
    kInterrupted,
};
```

Examples of running states:

- Rotating toward target
- Moving toward a position
- Waiting for cooldown
- Reloading
- Waiting for fire commit
- Waiting for spawn validation

## 7. Perception and Blackboard

### 7.1 Perception Adapter

Perception for AI should be implemented in `game_server` as an adapter over
gameplay truth.

It reads:

- Kernel vision state
- Entity state
- Weapon state
- HP and alive state
- Camp/faction/relationship facts
- Distance and last-known-position facts

It writes read-only AI facts into an AI context/blackboard.

Example facts:

- `visible_hostile`
- `nearest_hostile_id`
- `last_known_target_position`
- `target_distance`
- `hp_ratio`
- `weapon_status`
- `has_ammo`
- `is_reloading`

Perception does not decide and does not mutate gameplay.

### 7.2 Blackboard

Blackboard is fact storage for decision making.

The initial implementation may keep using `AIContext`, but the design should
prepare for typed blackboards:

```cpp
Blackboard<CombatContext>
Blackboard<CompanionContext>
Blackboard<DirectorContext>
Blackboard<SpawnDirectorContext>
```

Blackboard facts may be cached from gameplay truth, but they are not truth.

## 8. Runtime Ownership

### 8.1 Actor Runtime

Actor AI runtime belongs in `game_server`, not in `engine/src`.

The first version may use a simple map owned by `game_server`:

```cpp
std::unordered_map<NetId, CombatAgentRuntime>
```

A future ECS-backed version may introduce components, but those components
should live in a gameplay adapter package or game server layer, not in the
generic AI framework.

Example runtime contents:

- Runtime id
- Self actor id
- Blackboard/context
- Current intent
- Intent status
- Decision timers
- Tick scheduler
- Target memory

### 8.2 Director Runtime

Director runtime may not correspond to a world entity.

It may be held directly by `game_server`:

```cpp
SpawnDirectorRuntime spawn_director_;
EncounterDirectorRuntime encounter_director_;
```

Director runtime may produce world/director-scoped intents, but those intents
must still be validated before they become simulation commands.

## 9. Executor Rules

### 9.1 Actor Intent Executor

Actor intent executors translate actor-scoped intents into `ActorInput`.

Example:

```text
AttackTarget
  -> ValidateTarget
  -> Build aim direction
  -> Choose fire or reload button
  -> Fill ActorInput
  -> SubmitInput command
```

The executor should not directly set ammo, cooldown, hp, or projectile state.
Those remain owned by simulation.

### 9.2 Director Intent Executor

Director intent executors translate director/world-scoped intents into
authorized gameplay requests.

Example:

```text
SpawnAgent
  -> Validate spawn rule
  -> Select spawn point
  -> Build create info
  -> Enqueue authorized SimulationCommand::kCreateEntity
```

Director executor must not bypass validation or directly mutate world
containers.

### 9.3 Command Source and Authority

Commands generated by AI should preserve source information.

Actor AI should use an AI source for actor input.

Director/world AI should use an explicit source, such as AI or internal game
server source, and should pass through a single validation point before command
enqueue.

Control-plane RPC commands already have authority and phase checks. Director AI
does not need to be an RPC caller, but it should follow the same spirit:

- Validate authority.
- Validate phase.
- Validate gameplay rule.
- Enqueue command.
- Report result or failure.

## 10. Naming Rules

Prefer:

- `Actor`
- `Agent`
- `AIRuntime`
- `AgentRuntime`
- `CombatAgent`
- `CompanionAgent`
- `DirectorRuntime`
- `SpawnDirector`
- `Blackboard<CombatContext>`
- `Intent<DirectorContext>`
- `ActorInput`
- `ActorIntentExecutor`
- `DirectorIntentExecutor`

Avoid in generic framework:

- `EnemyManager`
- `EnemyRuntime`
- `EnemyBlackboard`
- `EnemyIntent`
- `EnemyAttackPlayer`
- `nearestEnemy`
- `targetPlayer`
- `PlayerInput` as the generic AI output type

Relationship naming should use:

- `hostile`
- `friendly`
- `neutral`
- `camp`
- `faction`
- `relationship`

## 11. MVP Implementation Status

| Requirement | Status |
|---|---|
| Generic intent, intent scope, and intent status types | Implemented |
| `AIContext` read-only fact container | Implemented |
| Capability registry and structured unsupported reports | Implemented |
| Registry-backed nodes, scores, YAML loading, and validation | Implemented |
| Selector, sequence, utility, running, and halt semantics | Implemented |
| `game_server` perception adapter | Implemented |
| Actor intent executor for Attack/Reload | Implemented |
| Sentry `Intent -> Executor -> PlayerInput` path | Implemented |
| Agent runtime naming and Director entity bootstrap | Implemented |
| Entity-template-driven Director spawn policy | Implemented |
| Public rename from `PlayerInput` to `ActorInput` | Deferred; ABI remains `PlayerInput` |
| Generic mission/encounter Director executor | Deferred |
| Designer graph tooling and advanced AI semantics | Deferred |

The current implementation keeps `PlayerInput` as the concrete ABI envelope
while generic framework types remain independent of kernel/game-server types.
New semantics still require perception data, a validated executor, and an
authoritative gameplay system.

## 12. Minimal Validation Scenario

The first validation scenario should be actor-level sentry behavior.

Flow:

```text
AI sentry spawn
  -> idle / patrol
  -> perception sees hostile actor
  -> blackboard updates visible_hostile and target facts
  -> decision outputs AttackTarget(target_id)
  -> actor executor validates target
  -> actor executor builds ActorInput / PlayerInput
  -> simulation consumes SubmitInput
  -> weapon system checks ammo/cooldown/reload
  -> gameplay state updates ammo/cooldown/projectile
```

This scenario must answer:

- Which system produced each fact?
- Which system consumed each fact?
- Which data is gameplay truth?
- Which data is AI fact cache?
- Which intent is running?
- When does it fail?
- When does it interrupt?
- When does it succeed?

## 13. Non-Goals for First Refactor

Status note: "Mission director" and "Squad tactics" below are no longer
non-goals -- both shipped, in `game_server`, without a kernel ABI addition. The
mission director is `game_server/src/game_rule_director.{h,cc}` and the squad is
the patrol system; `docs/AI_PATROL_SYSTEM.md` covers both. The ownership model
in section 14 held up under them and is unchanged.

Do not include these in the first refactor:

- Full Utility AI
- Full data-driven behavior graph
- LLM prompt-to-intent compiler
- Squad tactics
- Cover system
- Flanking
- Request backup
- Mission director
- Multi-agent coordination
- Full public ABI rename from `PlayerInput` to `ActorInput`

Unsupported semantic functions must remain unsupported until the required
queries, gameplay systems, and executors exist.

## 14. Summary

The refactor should establish this ownership model:

```text
engine/components/ai owns decision abstractions.
game_server owns gameplay interpretation.
kernel/simulation owns gameplay truth and validated mutation.
blackboard owns read-only facts.
intent owns desired action.
executor owns translation and execution status.
ActorInput owns per-actor control output.
SimulationCommand owns validated world/director mutation.
```

The final architecture should let sentry agents, companions, pals, spawn
directors, and encounter directors share one generic AI runtime pattern without
forcing all AI output through `PlayerInput`, and without letting the AI
framework depend on gameplay-specific types.
