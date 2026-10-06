# Projectile / Collider Data Ownership

Status: **Adopted** (2026-10-06). Migration in progress on
`claude/rebuild-projectile-collider-data-ownership`.

`COLLIDER_POLICY_REQUIREMENTS.md` already says a collider template owns "reusable
shape geometry". The catalog does not hold to that today: an area effect's
height, a hitscan's reach and a melee swing's shape each live somewhere else,
and some collider fields are silently ignored. This document states the rule
as one principle, lists where the catalog breaks it, and what changes to fix
that.

## Decision

**P1: geometry belongs to the collider.** Whatever answers "where does this
reach, and how big is it" -- radius, half height, beam length and width, swing
shape -- is authored once, on the collider template, and nowhere else.

**One exception: hitscan range stays on the weapon.** `max_range` is a balance
number on the gun rather than the shape of anything, and a segment has no
geometry of its own beyond the ray the weapon fires.

## Rules

| # | Rule |
|---|---|
| R1 | **Geometry lives on the collider.** Shape and every dimension of what a hit query covers. One source per value: no template-side override, no fallback that guesses a value from another field. |
| R2 | **Behaviour lives on the projectile.** Who it hits (`collision_mask`), how it moves (`speed`, `motion`, gravity), timing (`lifetime_ticks`, intervals), damage, falloff, sync mode and triggers. A projectile carries no length or size. |
| R3 | **Firing lives on the weapon.** Fire mode, cadence, ammo, which projectile, and -- the exception above -- hitscan `max_range`. A weapon carries no shape. |
| R4 | **Every authored field is read or rejected.** The catalog compiler refuses a collider field its consumer would ignore (e.g. `half_extents` on a sphere used as an area, or a shape the consumer cannot query) instead of dropping it. |
| R5 | **No collision, no collider.** A projectile with `collision_mask: none` (markers, fuses) may leave `collider_template` out. |
| R6 | **Sharing a collider is a statement.** By default a collider is named for, and used by, one consumer. Two consumers share one only when resizing one must resize the other. |
| R7 | **One YAML key per concept.** |

R1 is about *hit queries*. The response to a hit -- how far an impulse
throws, whether a pull moves a hovering unit -- is not geometry and stays
whatever the systems compute from the target's own data. No per-template
special case is added to shape a response.

## Where the catalog stands

| Kind | Example | Reach comes from | Ignored or overridden |
|---|---|---|---|
| Standard projectile | rocket, grenade, homing | collider (it is the sweep shape) | nothing -- already follows P1 |
| Area effect | tornado, shockwave, frag, meteor blast | radius: collider; shape and height: projectile (`area_shape`, `half_height`) | collider `center`, `half_extents`; a non-sphere collider becomes a sphere of its largest half extent |
| Beam | drone beam, beam rifle, sentry beams | collider (oriented box: length = `half_extents.z * 2`, width = max(x, y); `kernel.cc` ~1545) | the documented `beam: {length, radius}` block is overwritten by the box |
| Hitscan | rifle, shotgun | weapon `max_range`; the segment collider is an id only | projectile `rifle_shot` / `shotgun_shot` also names a segment collider |
| Melee | grunt claw, grunt slam | weapon `melee_collider` (cone) | projectile names `melee_swing_marker`, a segment with no geometry |
| Marker / fuse | meteor, sky laser, storm markers | -- (`collision_mask: none`) | `projectile_sphere` required anyway |

Other findings:

- **Area radius is derived twice, by different rules.** game_server's
  `collider_template_radius_for_area` (`gameplay_config.cc`) takes `x` for a
  sphere or segment and the largest half extent otherwise; the kernel's
  `collider_template_radius` falls back to the largest half extent only when
  the radius is zero; then `mechanics.area_effect.radius` overrides both.
- **Shared collider:** `area_effect_sphere` is both `fire_floor_area` and
  `rocket_explosion`.
- **Three keys for one concept:** a projectile's kind is read from
  `projectile_type`, `type` or `kind`. `fire_floor_area` uses `type`, the rest
  `kind`.

## What Unity reads

Surveyed 2026-10-06 in `unity-network-example` (branch
`claude/aim-pitch-airborne`, fb9d6b5) and the kernel package it resolves
(`dev-latest`, PackageCache 115d2378). Unity never parses the catalog YAML; it
gets collider data from the kernel in two ways.

**1. The live query.** `Kernel_QueryColliderShapes` → `KernelColliderShapeView`
(`shape_type`, `shape_params`, world centre and rotation, segment endpoints,
purpose).

| Consumer | Uses | Shapes handled |
|---|---|---|
| `NetworkDebugView.DrawColliderShape` | debug wireframes | Sphere, Aabb, OrientedBox, Segment, Cone (no Capsule case) |
| `LineOfSightGeometry.SegmentHitsShape` (via `ClientRunner`'s agent sight) | local-agent line of sight | Sphere, Aabb, OrientedBox, Segment; Cone and anything else never block |

**2. The catalog fallback.** For a render entity the live query does not
cover, `KernelColliderShapeSource.TryRebuild` rebuilds the shape from
`GetColliderTemplates` / `GetColliderBindings` / `GetProjectileTemplates`. It
reads the collider template's `shape_type`, `shape_params`, `center` and
`purpose_flags`, and the projectile's `mechanics.collider_template_id`. The
template is drawn as authored. For an area effect that is the collider sphere:
the fallback cannot know a projectile-side `half_height`, so a tornado drawn
this way would be a 3 m sphere, not its column. P1 fixes this for free: once
the cylinder is on the collider, the fallback copies it like any other shape.

**Where the tornado comes from, by mode** (read from code, not run):

- **Pure client: fallback, never the live query.** A
  `local_predicted_deterministic` projectile lives in the client's
  `predicted_projectiles_` list, not as a `world_` entity with
  `ProjectileState` (`kernel.cc` ~7556). `sync_entity_colliders_from_world`
  materializes colliders only from that `world_` view, so the tornado has no
  collider instance and the query skips it. Its render state carries
  `collider_template_id` = `tornado_column` (~7565), and `TryRebuild` draws
  that: a 3 m sphere.
- **Server / listen server: live query, and it is wrong too.**
  `materialize_projectile_collider` (`kernel.cc` ~4699) builds the instance
  from the collider template as authored -- shape, radius, half extents --
  and never reads the area effect's `area_shape` / `half_height`. The query
  reports a 3 m sphere while gameplay overlaps a 3 m x 13 m column. That
  breaks the core invariant of `COLLIDER_POLICY_REQUIREMENTS.md` ("the same
  resolved size ... used by gameplay").

Both paths are fixed by the same move: once the collider template is the
cylinder, both copy it.

**Projectile mechanics:** only `projectile_type` (debug colour) and
`collider_template_id`. Nothing in Unity reads `area_effect.*` (`area_shape`,
`half_height`, radius) or `beam.*`; the package only fills their
`struct_size`.

**Geometry copied into the client** (hardcoded, not read from data):

- `NetworkDebugView.strikeWarningRings`: marker 21 → 6 m, fuse 22 → 4 m,
  the latter "the 4 m radius of the blast", i.e. `meteor_blast_sphere`.
- `TargetedStrikePreview.actorBoxHalfExtents` (0.35, 0.9, 0.35).
- `NetworkPrefabRegistry` placeholder capsule sizes for players and agents.

**Impact on the migration:**

| Step | Unity work |
|---|---|
| 1 cylinder | C# enum mirror in the package; a Cylinder case in `DrawColliderShape` and in `SegmentHitsShape` (decide whether an area cylinder blocks sight -- it should not); `TryRebuild` needs nothing |
| 2 beam block | none |
| 4 optional colliders | none to break: `TryRebuild` skips a template id of 0, so markers stop drawing their placeholder 0.5 m sphere. The warning rings are separate and stay |
| 3, 5, 6, 7 | none |

## Migration (after confirmation, new branch)

1. **Cylinder becomes a collider shape.** Add `shape: cylinder` (`radius`,
   `half_height`) to collider templates; area effects read shape, radius and
   half height from it; `area_shape` and `half_height` leave the projectile
   (rejected after the move). Tornado's column moves to collider 39.
   - `materialize_projectile_collider` carries the cylinder into the
     collider instance, so the live query reports what gameplay overlaps.
   - The collider shape enum is ABI (`KernelColliderShapeType`), and so is
     `Kernel_QueryColliderShapes`' shape view: adding `Cylinder` is an
     additive ABI change, and the Unity debug draw needs to learn it.
2. **Beam block.** Reject an authored `beam: {length, radius}` (catalog
   error) so the oriented box is the only source. The ABI fields stay, unread,
   to avoid a breaking ABI change; `WEAPON_AUTHORING_GUIDE.md` drops the block.
3. **One radius rule.** One derivation shared by game_server and kernel, with
   R4's rejection in front of it: an area collider must be a sphere or a
   cylinder.
4. **Optional colliders.** `collider_template` becomes optional for
   `collision_mask: none` projectiles and for melee hit projectiles, whose
   shape is the weapon's `melee_collider`. Drop the placeholder references.
   - Open: melee's cone sits on the weapon, against R3. See below.
5. **Hitscan projectiles.** Decide what `rifle_shot` / `shotgun_shot` use their
   segment collider for (not yet traced) and either drop it or document it.
6. **Unshare** `area_effect_sphere` into one collider per consumer, same size.
7. **`kind` only.** Rewrite `fire_floor_area` to `kind:`; reject `type:` and
   `projectile_type:` on projectiles.

Each step changes the catalog hash, so the package's `bundle.bytes` needs a
rebuild after it. Step 1 also needs the Unity package's ABI version bumped.

## Open questions

1. **Melee shape.** The cone is on the weapon (`melee_collider`). Under R3 it
   belongs on the hit projectile's collider. Move it, or make melee a second
   documented exception?
2. **Client-side geometry copies.** Unity hardcodes three sizes the catalog
   owns (see "What Unity reads"). Should they read the catalog instead, or are
   they accepted as presentation tuning?
3. **Area hitbox report.** A cylinder area's `Hitbox` is stored as a box
   (radius, half height, radius). Should `Kernel_QueryColliderShapes` report
   the cylinder itself once the shape exists?

## Verification

- Catalog tests that load the shipped catalog (`tornado_catalog_test` and the
  catalog loader tests) stay green, and each rejected form gets a test that
  the compiler refuses it.
- Per migrated projectile: same reach before and after (same radius, height,
  beam length), asserted from the compiled mechanics.
