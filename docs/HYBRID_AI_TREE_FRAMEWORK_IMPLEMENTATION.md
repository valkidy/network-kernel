# Hybrid AI Tree Framework Implementation

## Purpose

This document turns the Hybrid AI Tree Framework draft into an implementation
plan for an independent engine component. The framework should behave like a
small third-party library inside this repository: it has its own Bazel package,
has no dependency on `engine/src`, `game_server`, or app internals, and can be
integrated by gameplay code through explicit adapters.

Recommended branch name:

```bash
git switch -c codex/hybrid-ai-tree-framework
```

## Draft Review

The draft has the right core shape:

- `Tick(context) -> NodeStatus` keeps execution state separate from utility
  scores.
- `AICommandBuffer` keeps tree decisions decoupled from gameplay mutation.
- Cached `AIContext` features keep expensive perception outside tree execution.
- YAML validation before runtime instancing is the correct safety boundary.
- `Running` and `Halt` are enough to prepare for async action nodes.

The main changes needed before implementation are scope and boundary changes:

- Natural-language scenario analysis should not be part of the C++ runtime MVP.
  It should be an offline authoring layer that produces analysis reports and
  YAML, while runtime only loads validated YAML.
- Capability checking should run before `NodeFactory` instancing. The factory
  should not discover missing scenario capabilities by failing halfway through
  tree creation.
- `UtilitySelector` needs a deterministic score contract: scores are sampled
  once per tick, invalid candidates are skipped, ties use YAML child order, and
  the selected child status is returned without trying another candidate in the
  same tick.
- Async state should live in the tree instance. Node definitions can be shared
  later, but MVP nodes are instance-owned to keep `Running` and `Halt` simple.
- YAML type names should be canonical and registry-backed. Prefer
  `Composite.Selector`, `Condition.HasVisibleEnemy`, `Action.AttackTarget`,
  and `Score.AttackWhenHealthy` over mixing bare and categorized names.
- The MVP should separate generic framework nodes from project-specific enemy
  leaf nodes. The framework provides interfaces and simple sample leaves;
  `game_server` owns any kernel/entity-specific adapters.

## Architecture

Implement the framework as a new component package:

```text
engine/components/ai/
  include/ai/
  src/
  BUILD.bazel

engine/components/tests/ai_tests/
  BUILD.bazel
  *_test.cc
```

`engine/components` is reserved for reusable engine-adjacent libraries that do
not depend on the runtime modules under `engine/src`. Future components, such
as physics, should follow the same shape:

```text
engine/components/physics/
  include/physics/
  src/
  BUILD.bazel
```

The package namespace is:

```cpp
namespace network_example::ai
```

Do not include `components` in the namespace. The component path is an ownership
boundary, not part of the public API name.

Use component-local include paths without exposing the internal folder layout:

```cpp
#include "ai/ai_context.h"
#include "ai/ai_tree.h"
```

The `//engine/components/ai:ai` Bazel target should expose only public headers
under `include/ai`. Non-public headers and all `.cc` files stay hidden inside
the target implementation.

The dependency direction must remain:

```text
game_server or future gameplay layer
  -> engine/components/ai
      -> C++ standard library
      -> yaml-cpp for YAML parsing
```

`engine/components/ai` must not depend on:

- `//engine/src/...`
- `//game_server/...`
- `//app/...`

The first implementation branch should also keep existing `engine/src` modules
from depending on `//engine/components/ai`. Integration should happen in
`game_server` or a future adapter package outside `engine/src`.

Allowed dependencies:

- C++ standard library
- `@yaml_cpp` if YAML parsing is implemented in the first branch

Forbidden dependencies:

- any `//engine/src/...` target
- any `//game_server/...` target
- any `//app/...` target

Gameplay integration happens through adapters that build `AIContext`, tick an
`AITreeInstance`, and translate emitted `AICommand` values into game/server
operations.

Adapter direction is intentionally one-way:

```text
game_server
  -> //engine/components/ai:ai
  -> //engine/src/kernel:kernel_api

//engine/components/ai:ai
  -> no //engine/src deps
```

## Runtime Data Flow

```text
Perception / gameplay feature extraction
    -> AIContext
    -> AITreeInstance::tick()
    -> NodeStatus + AICommandBuffer
    -> gameplay command adapter
    -> kernel or gameplay systems
```

Tree nodes only read `AIContext` and append commands. They do not query the
world, mutate gameplay state, allocate long-running jobs, or call kernel APIs.

## MVP File Layout

### Core Public Headers

Create:

```text
engine/components/ai/include/ai/node_status.h
engine/components/ai/include/ai/ai_value.h
engine/components/ai/include/ai/ai_context.h
engine/components/ai/include/ai/ai_command.h
engine/components/ai/include/ai/ai_node.h
engine/components/ai/include/ai/ai_tree.h
engine/components/ai/include/ai/node_factory.h
engine/components/ai/include/ai/capability_registry.h
engine/components/ai/include/ai/yaml_loader.h
engine/components/ai/include/ai/scenario_analysis.h
engine/components/ai/include/ai/ai_scheduler.h
```

Responsibilities:

- `node_status.h`: `enum class NodeStatus { kSuccess, kFailure, kRunning };`
- `ai_value.h`: typed scalar values for feature and command parameters.
- `ai_context.h`: read-only feature bag passed into ticks.
- `ai_command.h`: command object and command buffer.
- `ai_node.h`: base node interface with `tick()` and `halt()`.
- `ai_tree.h`: owns the root node and tree instance state.
- `node_factory.h`: registry-backed YAML node construction.
- `capability_registry.h`: supported features, nodes, scores, and query names.
- `yaml_loader.h`: YAML parsing, validation, and tree loading entry points.
- `scenario_analysis.h`: structured input/output types for offline scenario
  analysis results. This is a data contract only in MVP.
- `ai_scheduler.h`: fixed-rate tree ticking helper.

### Core Sources

Create:

```text
engine/components/ai/src/ai_context.cc
engine/components/ai/src/ai_command.cc
engine/components/ai/src/composite_nodes.cc
engine/components/ai/src/condition_nodes.cc
engine/components/ai/src/action_nodes.cc
engine/components/ai/src/score_functions.cc
engine/components/ai/src/ai_tree.cc
engine/components/ai/src/node_factory.cc
engine/components/ai/src/capability_registry.cc
engine/components/ai/src/yaml_loader.cc
engine/components/ai/src/scenario_analysis.cc
engine/components/ai/src/ai_scheduler.cc
```

Keep sample conditions, actions, and scores intentionally generic. They should
exercise the framework but not encode game-server policy beyond scalar feature
checks and command emission.

Private helper headers, if needed, should live under `engine/components/ai/src/`
and must not be included by downstream users. The `cc_library` should use
`strip_include_prefix = "include"` so public consumers include `ai/*.h` without
knowing the repository path.

## Public API Decisions

### AIContext

Use a typed feature bag for MVP:

```cpp
using AIValue = std::variant<bool, int, float, std::uint32_t, std::string>;
```

`AIContext` exposes typed lookups:

```cpp
bool has_feature(std::string_view name) const;
std::optional<bool> get_bool(std::string_view name) const;
std::optional<float> get_float(std::string_view name) const;
std::optional<std::uint32_t> get_uint32(std::string_view name) const;
```

MVP feature names:

```text
hp01
hasVisibleEnemy
hasAmmo
isAtTarget
nearestEnemyId
enemyDistance
allyNearbyCount
coverScore
dangerScore
```

### AICommandBuffer

Use a generic command envelope so the AI package stays independent:

```text
AICommand
  type: string
  params: map<string, AIValue>
```

Initial command types:

```text
Patrol
MoveTo
AttackTarget
FleeFromTarget
RequestHelp
StopMovement
```

Gameplay code decides how these map to velocity, animation, target selection,
or kernel calls.

### AINode

Use instance-owned nodes:

```cpp
class AINode {
public:
    virtual ~AINode() = default;
    virtual NodeStatus tick(const AIContext& context,
                            AICommandBuffer* commands) = 0;
    virtual void halt(const AIContext& context,
                      AICommandBuffer* commands) = 0;
};
```

`halt()` must be idempotent. Composite nodes call `halt()` on running children
when control flow switches away from them.

### Composite Nodes

MVP composites:

- `Composite.Selector`
- `Composite.Sequence`
- `Composite.UtilitySelector`

Selector rules:

- Tick children in YAML order.
- Return the first `Success` or `Running`.
- Return `Failure` if all children fail.
- Halt the previously running child if a different child becomes active.

Sequence rules:

- Tick children in YAML order.
- Return the first `Failure` or `Running`.
- Return `Success` if all children succeed.
- Resume at the running child on the next tick.

UtilitySelector rules:

- Each child has `name`, `score`, and `node`.
- Evaluate every registered score once per tick.
- Skip candidates whose score function reports invalid input.
- Pick the highest score.
- Break ties by YAML order.
- Tick only the selected child.
- Halt the previous selected child when selection changes.
- Return `Failure` if no candidate is selectable.

### Conditions, Actions, and Scores

Initial conditions:

```text
Condition.HasVisibleEnemy
Condition.HpAbove
Condition.HpBelow
Condition.HasAmmo
Condition.IsAtTarget
```

Initial actions:

```text
Action.Patrol
Action.MoveTo
Action.AttackTarget
Action.FleeFromTarget
Action.RequestHelp
Action.StopMovement
```

Initial scores:

```text
Score.AttackWhenHealthy
Score.FleeWhenCriticalHp
Score.RequestHelpWhenInjured
```

Condition parameters should be explicit in YAML. Example:

```yaml
type: Condition.HpAbove
value: 0.5
```

Action nodes emit commands and return:

- `Success` for one-shot commands such as `AttackTarget` and `RequestHelp`.
- `Running` for long-running commands such as `MoveTo` and `FleeFromTarget`
  until `isAtTarget` or another completion feature says they are done.

## YAML Format

Use canonical categorized type names:

```yaml
tree: EnemySoldierAI

root:
  type: Composite.Selector
  children:
    - type: Composite.Sequence
      name: Combat
      children:
        - type: Condition.HasVisibleEnemy
        - type: Composite.UtilitySelector
          name: CombatDecision
          children:
            - name: Attack
              score: Score.AttackWhenHealthy
              node:
                type: Action.AttackTarget
                target: nearestEnemyId

            - name: Flee
              score: Score.FleeWhenCriticalHp
              node:
                type: Action.FleeFromTarget
                target: nearestEnemyId

            - name: RequestHelp
              score: Score.RequestHelpWhenInjured
              node:
                type: Action.RequestHelp
                target: nearestEnemyId

    - type: Action.Patrol
```

YAML validation must reject:

- missing `tree`
- missing `root`
- unknown `type`
- composite nodes without valid `children`
- utility children without `score` or `node`
- unknown feature references
- unknown score names
- invalid scalar parameter types

## NodeFactory and Capability Registry

Use two registries:

```text
CapabilityRegistry
  - supported feature names
  - supported command names
  - supported node type names
  - supported score function names
  - supported query names for authoring reports

NodeFactory
  - node constructors
  - score constructors/functions
```

`CapabilityRegistry` is used for authoring validation and structured missing
capability reports. `NodeFactory` is used only after validation succeeds.

Failure shape:

```yaml
status: unsupported
missing_features:
  - visibleEnemies
  - enemyHp
missing_nodes:
  - Query.VisibleEnemies
  - Selector.LowestHpEnemy
  - Blackboard.SetTarget
missing_scores: []
suggested_extensions:
  - Add perception output: visibleEnemies[]
  - Add feature accessor: enemy.hp01
  - Add query node: Query.VisibleEnemies
  - Add selection node: Selector.LowestHpEnemy
  - Add blackboard key: targetEnemy
```

## Scenario-to-YAML Workflow

MVP supports scenario analysis as data contracts, not as an embedded natural
language engine.

```text
Scenario text
    -> external rule-based or LLM-assisted analyzer
    -> ScenarioAnalysisResult
    -> CapabilityRegistry validation
    -> YAML generation result
    -> YAML validation
    -> runtime tree instance
```

The C++ runtime owns:

- `ScenarioRequirement`
- `ScenarioAnalysisResult`
- `YamlGenerationResult`
- capability validation helpers

The C++ runtime does not own:

- prompt templates
- LLM calls
- natural-language parsing
- automatic C++ node generation

This keeps runtime deterministic and testable.

## Scheduler

`AIScheduler` should be a small helper, not a global system.

Inputs:

- tree id or handle
- tick interval seconds
- accumulated elapsed time

Behavior:

- Accumulate `dt`.
- Tick when accumulated time reaches the configured interval.
- Run at most one AI tick per scheduler update in MVP.
- Return whether a tick happened.

Suggested initial intervals:

```text
near combat: 0.05 to 0.10 seconds
mid-distance: 0.20 to 0.50 seconds
far/offscreen: 1.00 to 2.00 seconds
boss/elite: 0.033 to 0.05 seconds
```

Movement and animation remain frame-rate dependent outside this framework.

## Third-Party Dependency

Add `yaml-cpp` for YAML parsing:

```text
third_party/yaml_cpp.BUILD
WORKSPACE http_archive(name = "yaml_cpp", ...)
```

Expose the smallest useful Bazel target:

```python
cc_library(
    name = "yaml_cpp",
    hdrs = glob(["include/**/*.h"]),
    srcs = glob(["src/**/*.cpp"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
```

If avoiding a new dependency becomes important, postpone YAML loading and test
the runtime through programmatic tree construction first. Do not hand-roll a
general YAML parser.

## Implementation Phases

### Phase 1: Runtime Core

Deliver:

- `NodeStatus`
- `AIValue`
- `AIContext`
- `AICommand` and `AICommandBuffer`
- `AINode`
- `AITreeInstance`
- programmatic construction tests

Acceptance:

- Context typed lookups behave correctly.
- Command buffer preserves command order.
- A simple manually constructed tree ticks deterministically.

### Phase 2: Composite Nodes

Deliver:

- `Composite.Selector`
- `Composite.Sequence`
- `Composite.UtilitySelector`
- halt/resume behavior
- deterministic tie-breaking

Acceptance:

- Selector and sequence return correct statuses across `Success`, `Failure`,
  and `Running`.
- Running child state is preserved across ticks.
- Previous running child receives `halt()` when control switches.
- UtilitySelector selects highest score and ties by YAML order.

### Phase 3: Leaf Nodes and Scores

Deliver:

- MVP condition nodes
- MVP action nodes
- MVP score functions

Acceptance:

- Condition nodes fail on missing required features instead of defaulting to
  success.
- Action nodes emit expected command names and parameters.
- Running actions can be halted and emit `StopMovement` where appropriate.
- Score functions report invalid input when required features are missing.

### Phase 4: Factory, Registry, and YAML

Deliver:

- `CapabilityRegistry`
- `NodeFactory`
- YAML loader
- validation reports
- yaml-cpp Bazel dependency

Acceptance:

- Valid sample YAML creates an executable tree.
- Unknown node, score, feature, and bad YAML shape produce structured errors.
- YAML cannot instantiate arbitrary C++ classes.

### Phase 5: Scenario Authoring Contract

Deliver:

- scenario analysis data structures
- YAML generation result data structures
- capability validation helper for scenario requirements

Acceptance:

- Supported scenario requirements pass capability validation and preserve the
  required nodes and features needed by an external YAML generator.
- Unsupported "lowest HP visible enemy" scenario returns missing features,
  missing nodes, and suggested extensions.
- No LLM or network dependency exists in runtime tests.

### Phase 6: Scheduler and Example Integration

Deliver:

- `AIScheduler`
- optional `game_server` adapter example behind a focused test

Acceptance:

- Scheduler ticks at configured intervals.
- Example adapter can build context from enemy/player state, tick a tree, and
  translate commands without adding kernel dependency to `engine/components/ai`.

## Test Plan

Create:

```text
engine/components/tests/ai_tests/ai_context_test.cc
engine/components/tests/ai_tests/command_buffer_test.cc
engine/components/tests/ai_tests/composite_nodes_test.cc
engine/components/tests/ai_tests/leaf_nodes_test.cc
engine/components/tests/ai_tests/node_factory_test.cc
engine/components/tests/ai_tests/yaml_loader_test.cc
engine/components/tests/ai_tests/capability_registry_test.cc
engine/components/tests/ai_tests/scenario_analysis_test.cc
engine/components/tests/ai_tests/ai_scheduler_test.cc
```

Add a dependency boundary test that reads `engine/components/ai/BUILD.bazel`
and fails if any dependency label starts with:

```text
//engine/src
//game_server
//app
```

The same test should scan `engine/src/**/BUILD.bazel` and fail if an existing
runtime module depends on `//engine/components/ai`. These checks are
deliberately simple: they protect the component from accidental coupling while
keeping `game_server` free to depend on the AI component through an adapter.

Focused commands:

```bash
bazel test --config=macos -c opt //engine/components/tests/ai_tests:all
bazel build --config=macos -c opt //engine/components/ai:ai
```

Repository verification:

```bash
.agents/skills/bazel-build/scripts/run-bazel-build.sh macos test
```

## Non-Goals for the First Branch

Do not implement in the first branch:

- GOAP planning
- dynamic C++ plugin loading
- visual graph editing
- protobuf or FlatBuffers AI DSL
- multithreaded AI execution
- complex blackboard query language
- runtime LLM calls
- automatic C++ source generation
- pathfinding, raycasts, visibility scans, or navmesh queries inside nodes

## Open Decisions Before Coding

Resolve these at the start of the implementation branch:

1. Whether to add `yaml-cpp` immediately or complete Phases 1-3 with
   programmatic tree construction first.
2. Whether MVP command parameters should stay generic (`map<string, AIValue>`)
   or use a fixed command union. The generic envelope is recommended for
   independence.
3. Whether `game_server` integration is part of the first branch or a follow-up
   branch. Keeping it as a follow-up is recommended unless a runtime demo is
   required.

## Done Criteria

The framework branch is ready when:

- `//engine/components/ai:ai` builds as an independent component package.
- AI tests pass with no dependency on `//engine/src`, `//game_server`, or
  `//app` internals.
- The dependency boundary test rejects forbidden component dependencies.
- Sample YAML loads, validates, and ticks.
- Unsupported capability reports are structured and deterministic.
- Running and halt behavior is covered by tests.
- Standard Bazel verification passes.
