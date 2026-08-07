# Collider Policy And Implementation Status

Status: **Implemented baseline**

This document defines the current collider authoring, runtime, query, and
physics-backend policy. It replaces the earlier implementation-planning
requirements.

## Core Invariant

```text
Kernel_QueryColliderShapes reports active runtime gameplay collider instances.
It does not synthesize or override shapes from templates at query time.
```

Unity and other clients may retain or style query results for visualization,
but the native runtime owns gameplay shape, transform, layer, purpose, and
lifetime.

## Runtime Model

Runtime collision uses `ColliderInstance` records containing:

- stable collider and source-template IDs;
- owner/entity/actor identity;
- shape, purpose, layer, and hit-zone data;
- local and world transforms;
- box, sphere, capsule, and segment parameters;
- lifetime and remaining ticks;
- damage-resolution and enabled state;
- conservative world bounds.

Templates are immutable authoring/catalog inputs. Runtime instances are
gameplay state. `PhysicsWorld` is the Jolt-backed query and character-movement
backend; kernel/simulation remains the authority that creates, updates, enables,
and removes gameplay collider state.

## Template Ownership And Resolution

| Template | Owns |
|---|---|
| `weapon_template` | Fire mode, action/cadence policy, ammo, and references to projectile/segment templates. |
| `projectile_template` | Movement, sync mode, lifetime, damage behavior, collision query mode, and collider reference. |
| `collider_template` | Reusable shape geometry plus purpose/layer defaults. |
| `entity_template` | Actor/director composition, including hit, movement, and vision collider references. |

Resolution is single-path by entity family:

- actor hit, movement, and vision shapes come from the entity/actor template;
- projectile, beam, and area-effect shapes come from projectile templates;
- transient rifle/shotgun segments use their referenced collider templates;
- the static world comes from the catalog's cooked Jolt scene;
- generic `entity_type -> collider` bindings are deprecated and rejected.

Runtime and debug consumers use resolved template IDs carried by entity/render
state. They must not fall back to a second generic binding table.

## Supported Shapes

| Gameplay shape | Runtime use |
|---|---|
| AABB / oriented box | Actor hit volumes, beams, and box projectiles |
| Sphere | Projectiles, overlaps, and area effects |
| Capsule | Character movement and capsule queries |
| Segment | Hitscan/shotgun traces and query-only rays |
| Cone | Vision range/FOV debug and query semantics |
| Static triangle mesh | Jolt static-world collision |

Segments and cones are query/gameplay concepts rather than persistent Jolt body
shapes. See `docs/JOLT_RECAST_COLLISION_CONTRACT.md` for backend mapping.

## Weapon And Effect Rules

- Rifle and shotgun produce short-lived segment instances, including misses.
- Segment damage is guarded against repeated application while the debug
  instance remains alive.
- Beam entities use oriented-box collision and action-driven refresh/cadence.
- Projectile templates choose the collision query mode and resolved collider.
- Explosions and persistent area damage are authoritative area-effect entities
  with their own lifetime, interval, falloff, and collider template.
- Homing orientation may be presentation-only when the gameplay collider is a
  sphere; non-spherical homing shapes must use the authoritative world pose.

## Query API

`Kernel_QueryColliderShapes` returns active runtime instances filtered by
entity, entity type, and purpose.

Zero means no filter:

- null query: all active instances;
- `entity_net_id == 0`: all entities;
- `entity_type_filter == 0`: all entity types;
- `purpose_mask == 0`: all purposes.

The query reports the same resolved size, transform, purpose, and layer used by
gameplay for that tick. Persistent hit/damage colliders live with their gameplay
entity. Transient segment/beam/area records remain queryable while their
lifetime is active.

## Physics And Performance

Jolt `PhysicsWorld` supplies the broad phase and narrow-phase queries used by
movement, weapons, projectiles, beams, and area effects. Filtering by layer,
purpose, owner/source, enabled state, and ignored entity occurs before gameplay
consumes results. Gameplay applies deterministic ordering where query order
could otherwise change outcomes.

The current runtime supports:

- static cooked mesh collision;
- object upsert, transform, enable/disable, and removal;
- ray casts, shape casts, and overlaps;
- grounded/kinematic/`CharacterVirtual` movement;
- local prediction against the verified static scene;
- collision-query statistics.

## Completed From The Original Plan

- Independent `projectile_templates` YAML schema.
- Independent collider templates and deterministic ID resolution.
- Actor/entity hit, movement, and vision collider references.
- Runtime `ColliderInstance` registry and query output.
- Capsule and movement collider support.
- Jolt-backed broad/narrow phase and static collision.
- Generalized projectile collision query modes.
- `KernelColliderShapeView` ABI support for packed shape parameters.

## Remaining Design Boundaries

- Multiple simultaneous authoritative hit colliders per actor require a focused
  component/materialization design.
- Material penetration, target thickness, and surface response metadata are not
  generalized collider features yet.
- Non-spherical homing collision should be added only with a gameplay need for
  orientation-critical hits.
- New public query fields require an ABI revision; do not add template/debug
  metadata speculatively.
- Unity visual retention, colors, grouping, and rendering remain presentation
  policy.
