# Jolt / Recast Collision Contract (Phase 1)

## Scope and Coordinate Contract

Phase 1 proves native C++ integration without replacing the gameplay collision
runtime, kernel public ABI, or Unity plugin. OBJ is the only supported asset
format. Imported positions use engine units, Y-up, and an identity transform.
Jolt and Recast consume the same validated `CanonicalTriangleMesh`, then create
independent backend data. No Jolt cooked shape or Detour navmesh bytes are
persisted as a production format.

The smoke fixture is
`RecastDemo/Bin/Meshes/dungeon.obj` from the pinned Recast Navigation 1.6.0
archive. The OBJ contains no inline license header and is not copied into this
repository; the external wrapper exposes only that file as runfile data.

## Shape Mapping

| Gameplay shape | Jolt body shape | Query meaning | Phase 1 decision |
|---|---|---|---|
| AABB | `BoxShape` | overlap/cast target | Long term this converges to Box + pose; the legacy enum is unchanged. |
| OrientedBox | `BoxShape` + rotation | overlap/cast | It is not a separate Jolt shape. |
| Sphere | `SphereShape` | overlap/cast | Direct mapping. |
| Capsule | `CapsuleShape` | overlap/cast | Supported by the contract; gameplay migration is deferred. |
| StaticTriangleMesh | `MeshShape` | static query target | Implemented by the Phase 1 smoke. `MeshShape::MustBeStatic()` enforces static use. |
| Segment | none | ray/segment query | Query-only; it never creates a Jolt body shape. |
| Cone | none | range + FOV query | Query-only; it never creates a Jolt body shape. |
| ConvexHull | `ConvexHullShape` | body/query | Recorded for future runtime work only. |

Jolt `MeshShapeSettings` receives indexed, counter-clockwise triangles from the
canonical mesh. Jolt documents mesh triangles as single-sided for simulation;
callers that intentionally query back faces must opt into the corresponding
ray/cast setting. The Phase 1 smoke creates a static body and verifies a
downward narrow-phase raycast.

## Query Contract

`Segment` maps to a ray or segment query and not to a stored body. A gameplay
vision `Cone` uses this ordered flow:

1. Query Jolt for range candidates.
2. Apply the gameplay-owned FOV/angle test.
3. Use a Jolt raycast for occlusion.

Jolt supplies spatial candidates and occlusion facts, while gameplay owns the
meaning of the cone. The visual debugger likewise renders gameplay-defined
shapes; it does not use Jolt `DebugRenderer` in Phase 1.

## Filtering Mapping

- Gameplay collision layer answers "what is this?"
- Gameplay collision mask answers "what should this detect?"
- Faction/team is gameplay state and is not automatically a physics layer.
- An adapter compiles gameplay layer/mask values into Jolt `ObjectLayer` plus
  `ObjectLayerPairFilter` and `ObjectVsBroadPhaseLayerFilter` behavior.
- Jolt `BroadPhaseLayer` is a backend optimization. Its values do not enter
  gameplay templates or the public ABI.
- Owner/source exclusion belongs in per-body or per-query filtering, such as a
  body/shape filter. It does not create owner-specific object layers.

The smoke uses one static object layer and one broad-phase layer only to prove
the backend APIs. Those numeric values are not gameplay constants.

## Canonical Mesh and Navigation

The canonical mesh contains only float positions, 32-bit triangle indices, and
bounds. It contains no Jolt, Recast, Detour, render, or Unity types. Import
validation rejects empty meshes, malformed or out-of-range indices, non-finite
vertices, and degenerate triangles. OBJ faces with position-only, slash-delimited,
positive, or negative indices are triangulated as a fan.

Recast rasterizes the canonical positions/indices and produces its own polygon
and detail meshes. Detour then builds an in-memory navmesh and performs an A-to-B
path query. That backend data is independent from Jolt's `MeshShape` even though
both originate from the same canonical input.

## Explicitly Deferred

- Existing faction-bit and collision registry migration.
- Public ABI shape enum/layout migration.
- Production `BodyShape` / `QueryShape` headers.
- Runtime body ownership, lifecycle, and ECS transform synchronization.
- Stable collider ID / Jolt `BodyID` / `SubShapeID` lookup.
- Character movement, grounding, dynamic rigid bodies, and runtime AI use.
- A production-stable Jolt cooked format or Detour asset format.
- GLB/glTF import.

No runtime collision replacement occurs in Phase 1.
