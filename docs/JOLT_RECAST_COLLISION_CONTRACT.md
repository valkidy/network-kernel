# Jolt / Recast Collision Contract

Status: **Implemented runtime foundation**

This document describes the current native collision, character movement, and
mesh-cooking boundary as of 2026-07-23. The earlier Phase 1 integration-only
contract has been superseded by runtime Jolt collision and packaged static
world data.

## Ownership

| Layer | Responsibility |
|---|---|
| `engine/src/geometry` | Import and validate canonical triangle meshes; cook backend artifacts. |
| `engine/src/physics` | Own Jolt initialization, static scenes, collision objects, queries, and `CharacterVirtual`. |
| Kernel/simulation | Own authoritative transforms, fixed-tick movement, projectile hit policy, prediction, and replication. |
| `game_server` | Own gameplay catalog references, asset selection, AI policy, and collider template authoring. |
| Unity/presentation | Consume render and debug data; never become collision authority. |

Gameplay owns the meaning of a collision. Jolt supplies spatial facts such as
ray hits, shape casts, overlaps, grounding, and character movement results.
Recast supplies cooked navigation data; it does not own actor transforms or
gameplay decisions.

## Asset Pipeline

The source mesh is imported into a backend-neutral `CanonicalTriangleMesh`
containing positions, triangle indices, and bounds. Import validation rejects
empty meshes, malformed indices, non-finite vertices, and degenerate triangles.

The mesh cooker produces independent artifacts:

```text
source mesh
  -> CanonicalTriangleMesh
      -> Jolt static collision artifact (*.joltmesh)
      -> Recast/Detour navigation artifact (*.navmesh)
```

The default gameplay bundle packages both artifact types under:

```text
mesh_assets/jolt
mesh_assets/recast
```

`gameplay_catalog.yaml` selects the authoritative Jolt artifact through
`static_collision_scene`. The catalog bundle and its mesh artifacts are
downloaded and verified before a client continues the normal handshake.

## Runtime Static Collision

`Kernel_SetPhysicsConfig` configures native physics before runtime start.
`Kernel_SetStaticCollisionScene` installs a validated static scene explicitly,
while `Kernel_LoadGameplayCatalog(..., KernelGameplayCatalogLoadOptions*)` can
attach the scene as part of catalog installation.

The static scene contract requires:

- a non-empty artifact within `KERNEL_STATIC_COLLISION_SCENE_MAX_BYTES`;
- non-zero scene and collider IDs;
- the terrain collision layer;
- artifact schema/backend compatibility;
- installation during the allowed pre-start lifecycle phase.

Dedicated-server and listen-host startup load the static scene before gameplay
catalog synchronization is considered complete. Client actor prediction that
uses blocking requires the same verified static scene.

## Character Movement

Jolt `CharacterVirtual` is the native character controller for configured
character movement. The physics layer supports create/update, remove, reset,
and fixed-tick move operations.

Kernel movement remains authoritative:

- server actors advance on fixed simulation ticks;
- grounding, slope, step, gravity, and snap policy come from entity movement
  definitions and collider templates;
- the first physics-finalized transform is applied when an actor is spawned;
- client-local `CharacterVirtual` prediction replays unacknowledged fixed-tick
  input against the verified static scene;
- render smoothing is presentation-only and does not change physics truth.

`KernelActorBlockingMode_Predicted` enables predicted actor blocking/session
rules. It does not transfer gameplay authority to the client.

## Query And Projectile Contract

The physics layer exposes deterministic, filtered:

- closest/all ray casts;
- closest/all shape casts;
- overlap queries;
- object enable/disable and transform updates.

Segments and vision cones remain gameplay/query concepts rather than persistent
Jolt body shapes. Vision uses a range query, gameplay-owned FOV test, and
occlusion raycast. Deterministic projectile gameplay remains server
authoritative; supported local predicted deterministic projectiles may query
the client static scene for presentation and correction.

## Shape Mapping

| Gameplay shape | Jolt representation |
|---|---|
| AABB / oriented box | `BoxShape` plus world pose |
| Sphere | `SphereShape` |
| Capsule | `CapsuleShape` |
| Static triangle mesh | restored Jolt `MeshShape` |
| Segment | ray/segment query only |
| Cone | range candidates plus gameplay FOV/occlusion tests |

Collider templates are authoring/catalog data. Runtime collision and
`Kernel_QueryColliderShapes` must resolve through the active actor, projectile,
movement, or static-scene collider path rather than a generic entity-type
fallback.

## Filtering

- Collision layer answers what an object is.
- Collision mask answers what a query or effect may detect.
- Broad-phase layers are private backend optimizations.
- Camp, faction, and relationship remain gameplay state.
- Owner/source exclusions are per-object or per-query filters, not new physics
  layers.
- Query results are sorted/filtered deterministically before gameplay consumes
  them.

## Current Boundaries

Implemented:

- Jolt static scene loading and query world;
- cooked `.joltmesh` restoration;
- Jolt ray, shape-cast, and overlap queries;
- grounded, kinematic, and `CharacterVirtual` actor movement;
- server-authoritative grounding and collision;
- client-local character prediction against verified static collision;
- deterministic predicted projectile/static-scene collision;
- Recast/Detour navmesh cooking and bundle packaging.

Not claimed by this contract:

- general-purpose dynamic rigid-body gameplay;
- client-authoritative physics;
- runtime navmesh path-following for current sentry/director AI;
- hot-swapping static collision after runtime start;
- Unity-owned collision or direct Jolt/Detour object access;
- a cross-version stable cooked-artifact format.
