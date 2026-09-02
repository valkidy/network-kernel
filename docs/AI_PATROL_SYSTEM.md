# AI Patrol System

Status: **Implemented for flat terrain. Placement reachability deferred — see
"Deferred: placement reachability" for what that costs and when it comes due.**

A patrol is a squad the world produces on its own schedule: it spawns somewhere
in an authored area, walks one route across the map, engages what it sees on the
way, and retires when it is done. It is not a mission step, and nothing waits for
it.

The model is deliberately one-shot. A patrol is not a garrison walking a beat
forever; it is an encounter the player may or may not run into, and it is meant
to be avoidable. That single decision is why retirement is mandatory rather than
an optimisation, and why a route is a route rather than a loop.

## Ownership

Patrols live entirely in `game_server`. **No kernel ABI change was needed for any
of it**, including the navigation layer.

```text
catalog patrols:      -> gameplay_config.cc   (parsed, validated, hashed)
PatrolDirector        -> when / what / where, spawns via Kernel_ServerCreateEntity
PatrolGroupRuntime    -> the route, the formation, each member's slot
AgentChaserController -> walks to its slot, chases, breaks off, returns
PatrolNavigation      -> Detour, loaded from the catalog bundle
```

The kernel owns entity storage, ticking, transport, snapshots and relevance. It
does not know squads exist, and it does not know a navmesh exists — its movement
is driven by the character controller. Grouping is a `game_server` struct, not an
ECS component; the director is not an entity in the world.

Three alternatives were weighed and rejected when this was decided:

- **Kernel owns grouping** — expose `GameplayGroupMembership` through
  `KernelServerEntityState`. Cheaper than it looks (that struct is not on the
  wire and already supports tail extension), but it puts a gameplay concept in
  the kernel.
- **game_server infers grouping** from spawn tick and position. Rejected: the
  spawn path can legitimately deliver a squad across several ticks, and two
  sources spawning on one tick merge silently.
- **game_server tags entities with opaque metadata the kernel stores.** Coherent
  only while the kernel never interprets the tag; the moment it must, this
  collapses into the first option at higher cost.

Authoring going through the catalog is not the same thing as going through the
kernel ABI. `AgentSentryConfig` has always been parsed, unknown-key-checked and
hashed while living entirely outside the kernel — patrols follow it.

## Layers

| Layer | File | Owns |
|---|---|---|
| Director | `game_server/patrol_director.{h,cc}` | when to spawn, what to spawn, where, route selection, retirement, budget |
| Squad | `game_server/patrol_group_runtime.{h,cc}` | the route, progress along it, formation, per-member slots |
| Member | `game_server/agent_chaser_controller.{h,cc}` | walking to its slot, perception, pursuit, breaking off, returning |
| Navigation | `game_server/patrol_navigation.{h,cc}` | snapping points onto walkable ground, finding routes |

Ordering inside `AgentRuntimeManager::tick` is load-bearing:

```text
patrol_director_.tick()   creates entities
sync_agents_from_kernel() discovers them
patrol_groups_.tick()     assigns slots
dispatch_controllers()    acts on them
```

The director runs **before** the resync. The other order drops every member of a
squad on the tick it was spawned, because the group runtime prunes members it
cannot find in the agent list.

## Behaviour worth knowing

**One member in a fight stops the squad.** Walking on and leaving them to catch
up reads as a squad abandoning its own, and it makes the leash useless — the slot
being chased would run away exactly as fast as the pursuit dragged the member off
it. A member *walking back* is not a member fighting, so the squad does move on
during a return. That is why a slot has to be a moving target rather than the
spot the member left.

**A squad in a fight is never retired**, whichever rule would have retired it.
Despawning enemies out from under the player shooting at them is worse than any
population it would have saved. The retirement linger stops counting while the
squad is fighting, so a squad that finishes its route mid-engagement does not
have its linger quietly run out during the fight.

**An empty server is not "everyone is infinitely far away."** Read that way, the
distance retirement rule would have a server with nobody on it sweep away every
patrol it spawns, on the tick it spawns them. No players means no distance
retirement.

**`kReturn` exists because it refuses to chase.** Every other exit from a pursuit
runs through losing sight of the target, so without a state that declines to
re-acquire, an agent that broke off would re-aggro on the next tick and never get
back. It ends on reaching its slot *or* on returning to where it broke
formation — the second condition is what stops a short chase from leaving the
agent unwilling to engage for the rest of the leg.

**The leash is measured from where the member broke formation**, not from the
route, so "how far a pursuit may drag a patrol" stays a property of the pursuit
while the squad moves on.

**A route that walks far further than the line between its ends is rejected** and
another start/end pair is drawn. See the measurements below for why.

## Authoring

Catalog top level, beside `player:` and `enemy:`. There is no template for a
patrol to hang off, because a patrol never reaches the kernel as a director.

```yaml
navigation_mesh:
  # Must name the same source mesh as static_collision_scene, or squads path
  # over one shape and walk into another.
  entry_path: mesh_assets/recast/plane_200x200.navmesh

patrol_budget:
  # Across every definition, which per-definition ceilings cannot express.
  # 0 is unbounded.
  max_live_agents: 48

patrols:
  - id: 1
    name: grunt_sweep
    area:
      shape: rect                 # rect | circle
      center: {x: 0.0, y: 0.0, z: 0.0}
      half_extents: {x: 40.0, y: 0.0, z: 40.0}   # circle takes `radius` instead
    seed: 7717
    interval_ticks: 900
    max_live_groups: 2
    count: {min: 4, max: 6}
    composition:
      - entity_template: chaser_grunt
        min: 4
        max: 6
    formation_spacing_meters: 1.5
    advance_speed_meters_per_second: 1.25
    waypoint_radius_meters: 0.5
    despawn_linger_ticks: 300
    despawn_distance_meters: 120.0
    max_detour_ratio: 4.0
    route_attempts: 8
```

And on an agent's entity template, read only when that agent is in a squad:

```yaml
ai:
  patrol:
    slot_radius_meters: 1.2
    input_magnitude: 0.5
    leash_meters: 18.0
    leash_resume_meters: 6.0
```

### Composition semantics

The bands are **floors plus capacity**, not a second opinion about the total.
`count: 8-10` with `[A: 2-8, B: 4-20]` is unsatisfiable read any other way — the
floors sum to 6 and the ceilings to 28, and neither is 8 to 10. Every entry gets
its minimum, and the remainder goes to whoever still has room, one at a time.

A definition whose floors do not fit the smallest squad, or whose ceilings cannot
fill the largest, **fails to load**. An unsatisfiable patrol is a catalog error,
not a squad that quietly comes out the wrong size.

### `seed`

A real PRNG seed, unlike `seed` in the older spawners, where it is a starting
index into a golden-angle sequence. Every draw comes from it and the count of
squads the definition has already spawned, so a definition replays identically
and a squad's size can be named before it exists.

## The three axes

WHEN, WHAT and WHERE are separate axes rather than one mode enum, because the
answers are independent — a quick simulation setup wants a fixed cadence and an
explicitly sized squad; a paced encounter system wants accumulated pressure and a
credit budget; and every mixture of those is a sensible thing to author.

| Axis | Implemented | Not yet |
|---|---|---|
| WHEN | `fixed_interval` | spawn pressure with per-objective heat |
| WHAT | explicit count bands | credit budget with weighted spawn cards |
| WHERE | `rect`, `circle` | relative to players or points of interest |

Only one option per axis exists today. Separating them is what makes adding the
others a change to one axis rather than to a mode matrix.

## Measured basis

`//game_server:patrol_nav_bench` (manual) measures whether a route can be a
straight line, on three terrains. It is why this system paths rather than drawing
chords, and it is the tool to re-run when the terrain changes.

Fraction of random point pairs whose straight line stays on the navmesh:

| terrain | all | 50-100 m | 100 m+ |
|---|---|---|---|
| `plane_200x200` (ships today) | 100% | 100% | 100% |
| `undulating` (rolling) | 30.3% | 18.0% | 9.5% |
| `obstructed_field` (walls, pits) | 16.9% | 5.1% | **0%** |

Detour ratio when the straight line fails: rolling p50 1.01 / p90 1.05; walled
p50 1.23 / p90 2.04 / **max 18.10** — two points 20 m apart needing a 270 m walk.
That worst case is what `max_detour_ratio` exists to reject.

Coverage of an authored rectangle: 99.7% / 96.1% / 89.7% already walkable, and
snapping within 1 m recovers about half the loss on walled terrain (89.7% ->
94.9%). That recovered band is Recast's 0.6 m agent-radius erosion.

`obstructed_field` is generated by `tools/make_obstructed_field_obj.py` and baked
in `//game_server/test_mesh_assets`, which is deliberately **not**
`//game_server/mesh_assets` — that package globs into the shipping catalog
bundle, and a terrain built to be measured should not be downloaded by clients.

## Deferred: placement reachability

**Deferred on purpose. The shipping map is a flat plane, so none of what follows
can happen yet. It comes due when the terrain stops being flat — procedural
terrain, or buildings.**

Two questions are unanswered today, and both are about spawning a squad somewhere
it should not be:

### 1. Unreachable islands (rooftops)

Recast will happily produce walkable navmesh on top of a building. A squad
spawned there is on valid walkable ground, can route around the roof, and can
never come down. `PatrolNavigation::snap` cannot tell the difference: "is this
walkable" and "is this reachable from where players are" are different questions.

This has **never been measured**, because `obstructed_field` avoids it by
construction: its walls are 1 m thick, and Recast's 0.6 m agent-radius erosion
removes a 1 m wall top entirely, so it produces no rooftop islands. That was the
right choice for measuring connectivity — a rooftop island would have polluted
it — but it means the rooftop question is open.

Closing it needs, in order:

1. A test terrain with thick walls and flat roofs, alongside `obstructed_field`.
2. Extending `patrol_nav_bench` to report what fraction of walkable area is in
   the same connected component as the largest one. The bench already measures
   pairwise connectivity; component analysis is the same query aggregated
   differently.
3. If the fraction is material, a reachability filter at spawn: reject a start
   point that cannot route to a reference point, most likely a player's position
   or the map's main component.

Step 3 needs no new kernel API. It is `find_route` against a reference point,
which already exists.

### 2. Dynamic obstacles

The navmesh is baked from the static collision scene only. Props, `ice_block`,
and entity colliders are not on it. So navigation can answer "is this ground
walkable" but not "is this spot occupied right now" or "is this route blocked
right now".

Closing it **does** need a kernel change, and it is the only one this whole
system has needed:

- `physics::PhysicsWorld` already has `ray_cast_all`, `ray_cast_closest`,
  `shape_cast_all`, `shape_cast_closest`, `overlap_all` and `move_character`,
  all with a `CollisionQueryFilter` — layer masks, object-kind masks, ignore
  ids. It is used internally by the weapon, projectile and item systems.
- **None of it is exposed to `game_server`.** `kernel_api.h` has no raycast, no
  overlap, no ground probe, no line-of-sight query. `Kernel_QueryVisionState`
  returns an agent's vision-cone *result*, not a general query.

So the work is an ABI wrapper, not an implementation. Suggested shape, smallest
first:

1. `Kernel_ServerRaycast` — the minimum, and enough for a ground probe and a
   line-of-sight check.
2. An overlap query. This one needs a decision: exposing
   `CollisionShapeDescriptor` through the ABI is a much larger surface than
   naming an existing `collider_template_id`, and the collision-mask vocabulary
   is already data-driven in the catalog (`collision_mask: terrain |
   static_obstacle | actor`), so the template route is likely the right one.

Timing is safe: `PhysicsWorld` is query-only and `game_server` runs on the same
thread as the kernel, so a query inside a controller tick has no synchronisation
problem.

### Why deferring is safe

The failure mode is not gradual. On flat ground every one of these questions has
the same answer it would have with the work done, which is why the system is
correct today and why the work cannot be validated today either — a reachability
filter on a flat plane rejects nothing, and a test of it would assert nothing.

The trigger is explicit: **the first terrain that is not flat, or the first
building.** At that point, re-run `patrol_nav_bench` on the real terrain before
designing anything. Its numbers are what decided every route question so far.

## Other open items

- **The layering criterion.** `//tools:check_layering_boundaries` enforces the
  dependency direction; the other direction -- a gameplay concept added to a
  kernel or world header -- is a review criterion in
  `docs/GAME_SERVER_V1.md`, deliberately not an automated check.
- **The two director systems.** See "Planned: move the directors out of the
  kernel" below. Patrol is a third population system, on the correct side of the
  boundary; the kernel-side directors are the ones that move.
- **`enemy:` in the catalog** sets `override_director_spawn`, which replaces
  every world-rule director's spawn config wholesale. It does not affect patrols,
  which never reach the kernel as a director, but it does mean the world-rule
  director's own authored spawn is inert whenever `enemy:` is present.
- **Pressure and credit**, the other options on the WHEN and WHAT axes.

## Planned: move the directors out of the kernel

Unlike placement reachability, this is **not** waiting on anything. It is
scheduled work, and the reason is not that a feature needs it.

The repository currently contains two contradictory answers to "how do I build a
system that spawns things": patrol, which is `game_server` and touches no kernel
ABI, and the `game_rule` / `world_rule` directors, which are ECS components,
kernel validation, and four structs in the kernel ABI. The next person adding a
population system will read both. The older one is larger and looks more
established, so it is the one likely to be copied — and copying it produces
another system on the wrong side of the boundary, which then has to be moved too.

A wrong design left in place is not inert. It is a worked example, and it is
teaching.

### What moves

| Location | What |
|---|---|
| `engine/src/world/public/components.h` | `DirectorRuntime`, `WorldRuleRuntime`, `GameRuleRuntime`, `GameRuleGroupRuntime`, `GameplayGroupMembership`, `DirectorKind`, `GameRuleStatus`, `GameRuleNodeState` |
| `engine/src/simulation/src/systems.cc` | the `SpawnGroup` / `SpawnAgent` executors, the golden-angle placement, DAG advancement |
| `engine/src/kernel/src/kernel.cc` | game-rule graph validation and storage |
| `engine/src/kernel/public/kernel_types.h` | `KernelGameRuleDefinition`, `...NodeDefinition`, `...EdgeDefinition`, `...SpawnGroupEffectDefinition`, `KernelDirectorKind`, and the three `KERNEL_MAX_GAME_RULE_*` limits |
| `engine/src/simulation/src/command_dispatcher.cc` | the group bookkeeping in the `kCreateEntity` branch |
| `game_server/gameplay_config.cc` | stops compiling the graph into the kernel catalog and keeps it in `game_server` |

Roughly 230 lines across four engine files, plus about 100 in the config loader.

**This shrinks the ABI rather than growing it.** Nothing outside `engine` and
`game_server` consumes any of it — no C# or Unity binding references
`KernelGameRule*` or `KernelDirectorKind`, so the removal needs no coordination.

One consequence worth naming: `AiControllerType` in the kernel exists **only** to
decide whether an entity is a director the kernel should tick
(`systems.cc:1442`). Everywhere else the kernel needs to know an entity is an
agent, which `AgentTag` and `actor_type` already say, and controller routing has
always lived in `game_server`'s `AgentControllerBinding`. When directors leave,
that enum is vestigial.

### Sequence

1. **Make the gate real.** Done. `game_rule_director_test` (517 lines, 64
   assertions) and `simulation_command_dispatcher_test` (10 more) were entirely
   `assert()`, which `-c opt` compiles away, so a "passing" suite was evaluating
   nothing. They are `require()` now, all 74 pass, and the gate was
   mutation-checked. A behaviour-preserving port needs behaviour it can preserve
   against, and there was none.
2. **Port the world-rule director first.** It is the smaller of the two, its
   "keep a population topped up" job is what `PatrolDirector` already does, and
   the catalog's `enemy:` block makes its authored spawn inert today — so it is
   the one with the least behaviour to preserve.
3. **Port the game-rule director.** Behaviour-preserving: the same DAG, the same
   conditions, the same effects, in `game_server`. Gated on the test from step 1.
4. **Remove the ABI structs and the vestigial `AiControllerType`**, and drop the
   catalog's `enemy:` override with them.

Steps 2 and 3 each want the same treatment patrol got: mutation-check every
behaviour, because a green test in this area has been wrong four times.

## Tests

| Target | Covers |
|---|---|
| `//game_server:patrol_navigation_test` | loading, snapping, partial-path rejection, detour rejection |
| `//game_server:patrol_group_runtime_test` | cursor advance, formation rotation, hold-while-engaged, casualties |
| `//game_server:patrol_director_test` | composition draws, validation, spawning, determinism, retirement, budget, navigable routing |
| `//game_server:agent_chaser_controller_test` | slot following, return trajectory, leash |
| `//game_server:patrol_nav_bench` | manual; route viability per terrain |

Every behaviour above was mutation-checked — the implementation was broken
deliberately and the test confirmed to fail on the assertion that names it. Three
mutations initially **survived**, each because a test was passing for the wrong
reason; those cases were rewritten. Treat that as the standard here rather than
as an anecdote: a green test in this area has been wrong three times.

This repository has no CI, and its test suites are not green. Gate on **the
failure set being unchanged**, never on "all tests pass". Baseline while this was
built: `//game_server/...` 1 failure (`agent_runtime_manager_test`),
`//engine/src/tests/...` 4 failures (`beam_client_reconstruction`,
`entity_lifecycle_system`, `network_stats`, `beam_roundtrip`).
