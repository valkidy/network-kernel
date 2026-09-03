# AI Patrol System

Status: **Implemented for flat terrain, and both directors have since moved out
of the kernel. Placement reachability is deferred — see "Deferred: placement
reachability" for what that costs and when it comes due.**

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
| Director | `game_server/src/patrol_director.{h,cc}` | when to spawn, what to spawn, where, route selection, retirement, budget |
| Squad | `game_server/src/patrol_group_runtime.{h,cc}` | the route, progress along it, formation, per-member slots |
| Member | `game_server/src/agent_chaser_controller.{h,cc}` | walking to its slot, perception, pursuit, breaking off, returning |
| Navigation | `game_server/src/patrol_navigation.{h,cc}` | snapping points onto walkable ground, finding routes |

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
in `//game_server/tests/test_mesh_assets`, which is deliberately **not**
`//game_server/shipping_catalog/mesh_assets` — that package globs into the shipping catalog
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

## Done: the directors moved out of the kernel

Both directors live in `game_server` now, and the kernel has none. This was not
waiting on a feature, and the reason to do it was not that one arrived.

The repository had come to hold two contradictory answers to "how do I build a
system that spawns things": patrol, which is `game_server` and touches no kernel
ABI, and the `game_rule` / `world_rule` directors, which were ECS components,
kernel validation, and four structs in the kernel ABI. Whoever added the next
population system would have read both, and the older one was larger and looked
more settled. A wrong design left in place is not inert; it is a worked example,
and it teaches.

### What it took

| Step | Outcome |
|---|---|
| Make the gate real | `game_rule_director_test` was 517 lines and 64 `assert()`, all compiled out under `-c opt`. None had ever been evaluated. |
| Move the world rule | `game_server/src/world_rule_director.{h,cc}` |
| Move the game rule | `game_server/src/game_rule_director.{h,cc}` |
| Delete the ABI | `KERNEL_ABI_VERSION` 85 -> 86, a removal |

The gate came first on purpose. A behaviour-preserving port needs behaviour it
can preserve against, and there was none: a suite that evaluates nothing would
have made the port feel safe and proved nothing.

### What the move taught, which reading would not have

**Two behaviour differences only tests found.** The agent count a world rule
works against has to come from the entity list with no validity filter, because
an agent created this tick is not valid until physics finalises -- counting the
`game_server` way had the rule spawn a replacement for the agent it had just
made. And the interval is wall clock, not time spent short: the kernel held an
absolute `next_tick`, so a rule at full strength for longer than its interval
replaces a casualty on the tick it appears.

**Synchronous spawning deleted a mechanism rather than porting it.** The kernel
dispatched creates through a command queue, so a group could not know when it
was fully populated: hence `pending_spawn_count`, hence `sealed`, hence an
empty-but-unfilled group not reading as cleared. `Kernel_ServerCreateEntity`
returns the net id, so a node is activated, its group filled and its members
recorded inside one call. `sealed` was carried over at first, and a mutation
removing it changed no behaviour -- because it cannot. A flag nothing can
observe is a flag that will eventually be believed.

**Extracting config at load quietly moved a decision.** Pulling the directors
out of the catalog during load meant that rewriting
`preload_director_template_ids` no longer disabled anything -- which is how a
test disables a director. Every authored director is translated now, and the
preload list is applied where the directors are constructed, so it stays the
live decision it always was.

**The ABI got smaller.** Nothing outside `engine` and `game_server` consumed any
of it, so the removal needed no coordination. The estimate that said otherwise
was wrong, and so was the one that called the port a rewrite of mission flow: it
is the same DAG, the same conditions and the same effects, on the other side of
the boundary.

### Where the coverage went

Behaviour moved with the mechanism rather than being deleted with it.
`//game_server:world_rule_director_test` covers filling a target, the wall-clock
interval, and the golden-angle ring and its cursor.
`//game_server:game_rule_director_test` covers the player gate, a cleared wave
opening the next, a join waiting for every branch rather than the first one
home, and a spawn that cannot happen failing the rule. The kernel tests that
drove the old mechanism are gone.

## Other open items

- **The layering criterion.** `//tools:check_layering_boundaries` enforces the
  dependency direction; the other direction -- a gameplay concept added to a
  kernel or world header -- is a review criterion in
  `docs/GAME_SERVER_V1.md`, deliberately not an automated check. The director
  machinery is the worked example of why: every one of its types arrived as an
  addition to a header, not as a dependency edge.
- **`AgentRuntime::controller_type` is write-only in the kernel.** Its only
  reader was the director tick. The kernel still materialises it and still
  validates the ABI field, and nothing consumes either. Removing it is not quite
  free, because `ai.controller_type == None` is also the gate deciding whether
  `AgentRuntime` is attached at all, so it wants its own change.
- **`enforce_prop_population_limit` is still in the kernel**
  (`simulation/src/systems.cc`). A ceiling on how many of something may exist is
  a rule, and this is the same rule `patrol_budget` states in `game_server`. It
  cannot move alone: props are spawned by kernel-internal trigger actions, so
  `game_server` has no point at which to apply it. Prop spawning would have to
  move first.
- **Pressure and credit**, the other options on the WHEN and WHAT axes.
- **Assertions compiled out under `-c opt`.** Five files in this area were
  found with every check inside `assert()`, which this suite compiles away, so
  none of them had ever been evaluated. All five are converted now
  (`agent_chaser_controller_test`, `world_test`, `game_rule_director_test`,
  `simulation_command_dispatcher_test`, `entity_lifecycle_system_test`). Three
  of the five were hiding something: two turned out to be passing for the wrong
  reason, and the last was hiding two stale assertions -- a `visual_flags`
  equality that broke when `derived_visual_flags` gained its grounded/falling
  bits, and a snapshot check from when creation built the snapshot itself.
  Neither was a product bug, and neither could have been found while the file
  reported PASSED. Worth assuming there are more elsewhere in the tree: grep for
  `assert(` in a test that has no `require`.

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
