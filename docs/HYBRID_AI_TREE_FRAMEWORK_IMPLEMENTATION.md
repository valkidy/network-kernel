# Hybrid AI Tree Framework Implementation

Status: **Implemented**

This document describes the implemented reusable AI component and its current
game-server integration as of 2026-07-23. The original phased implementation
plan is complete; remaining items are explicitly listed under Current
Boundaries.

## Architecture

The generic component is:

```text
//engine/components/ai:ai
```

Its dependency direction is:

```text
game_server adapters
  -> engine/components/ai
      -> C++ standard library
      -> yaml-cpp
```

`engine/components/ai` does not depend on `engine/src`, `game_server`, `app`,
transport, Unity, EnTT, or kernel public types. A dependency-boundary test
protects this rule.

The generic framework owns decisions and intents. `game_server` translates
those intents into validated gameplay input. Kernel/simulation remains the only
owner of authoritative state mutation.

```text
gameplay perception
  -> AIContext
  -> AITreeInstance::tick
  -> IntentBuffer
  -> game_server executor
  -> actor input or validated server operation
  -> kernel/simulation
```

## Implemented Component Surface

Public runtime types:

- `AIValue` — typed scalar values.
- `AIContext` — read-only feature storage for one decision tick.
- `NodeStatus` — success, failure, and running state.
- `AINode` — tick/halt interface.
- `AITreeInstance` — owns the root and running-node state.
- `ScopedIntent` and `IntentBuffer` — actor/director/world output envelope.
- `IntentStatus` — running, succeeded, failed, and interrupted.
- `AIScheduler` — fixed-rate decision tick helper.

Construction and authoring types:

- `CapabilityRegistry` and structured `CapabilityReport`;
- `NodeFactory`;
- YAML parsing, validation, and tree construction;
- `ScenarioAnalysisResult` and `YamlGenerationResult` data contracts.

Scenario analysis remains an offline contract. The runtime does not call an LLM,
parse unrestricted natural language, or generate C++ source.

## Implemented Nodes

Composites:

- `Composite.Selector`
- `Composite.Sequence`
- `Composite.UtilitySelector`

Conditions:

- `Condition.HasVisibleHostile`
- `Condition.HpAbove`
- `Condition.HpBelow`
- `Condition.HasAmmo`
- `Condition.IsAtTarget`

Actions:

- `Action.Patrol`
- `Action.MoveTo`
- `Action.AttackTarget`
- `Action.FleeFromTarget`
- `Action.RequestHelp`
- `Action.Reload`
- `Action.StopMovement`

Scores:

- `Score.AttackWhenHealthy`
- `Score.FleeWhenCriticalHp`
- `Score.RequestHelpWhenInjured`

Selector/sequence running state, halt transitions, utility selection, invalid
scores, and YAML capability validation are deterministic and test-covered.

## YAML Contract

YAML uses canonical registry-backed type names:

```yaml
tree: SentryCombat
root:
  type: Composite.Selector
  children:
    - type: Composite.Sequence
      children:
        - type: Condition.HasVisibleHostile
        - type: Action.AttackTarget
          target: nearestHostileId
    - type: Action.Patrol
```

Validation rejects missing roots, unknown node/score/feature names, invalid
composite shapes, missing parameters, and incompatible scalar types before
runtime instancing.

## Game-Server Integration

The current adapter layer includes:

- `AiPerceptionAdapter`, which converts kernel vision and gameplay state into a
  read-only `AIContext`;
- `ActorIntentExecutor`, which validates actor-scoped intents and converts
  supported attack/reload behavior into the authoritative actor input path;
- `AgentSentryController`, which owns the current sentry decision flow;
- `AgentRuntimeManager`, which observes agent actors and bootstraps configured
  server-only Director entities;
- entity-template-driven Director spawn policy.

The adapter preserves `IntentScope`. Actor intent does not mutate ammo, HP,
projectiles, or transforms directly. Those consequences remain validated kernel
and simulation work.

## Tests

Focused test targets cover:

- context and typed value access;
- scoped intents;
- selector, sequence, utility, running, and halt behavior;
- leaf nodes and score functions;
- node factory construction;
- YAML loading and validation;
- capability and scenario reports;
- scheduler timing;
- forbidden dependency directions;
- game-server perception and actor intent execution.

Focused commands:

```text
bazel test --config=macos -c opt //engine/components/tests/ai_tests:all
bazel test --config=macos -c opt //game_server:actor_intent_executor_test
bazel test --config=macos -c opt //game_server:agent_sentry_controller_test
bazel test --config=macos -c opt //game_server:agent_runtime_manager_test
```

## Current Boundaries

Not implemented or not claimed complete:

- a visual behavior-graph editor;
- runtime LLM calls or natural-language compilation;
- GOAP, squad tactics, cover selection, flanking, or backup gameplay;
- multithreaded tree execution;
- generic mission/encounter director executors;
- production runtime Recast path-following for agents;
- a public ABI rename from `PlayerInput` to `ActorInput`;
- arbitrary gameplay mutation from generic AI nodes.

New gameplay semantics require all three pieces before registration: a
perception fact/query, an intent executor, and an authoritative gameplay system.
Unsupported semantics must produce capability errors rather than empty action
nodes.
