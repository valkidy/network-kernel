# Collider Policy Requirements

## Purpose

This document captures the agreed collider policy requirements for the next
implementation planning pass. It replaces the current split behavior where
runtime gameplay collision uses hardcoded or component-local shape data while
`Kernel_QueryColliderShapes` may report catalog template data that does not
match actual collision.

The core rule is:

```text
Kernel_QueryColliderShapes reports active runtime gameplay collider instances.
It must not synthesize or override shapes from templates at query time.
```

Unity uses `Kernel_QueryColliderShapes` only as visual debugger data. Unity may
choose how long to display shapes, how to style them, and how to group them, but
kernel shape lifetime and size must remain gameplay truth.

## Data Model

Runtime collision should be represented by `ColliderInstance` records. Field
names should use:

- `collider_id`
- `owner_net_id`
- `entity_type`
- `shape_type`
- `purpose_flags`
- `layer_mask`
- `world_center`
- `world_rotation`
- `half_extents`
- `radius`
- `segment_start`
- `segment_end`
- `lifetime_ticks`
- `remaining_ticks`
- `has_resolved_damage`

Templates are authoring inputs. Runtime instances are gameplay state. Query APIs
read runtime instances.

## Template Ownership

`projectile_template` should become an independent template category under its
own directory. `weapon_template` should describe firing behavior and reference a
`projectile_template_id` when the weapon spawns projectile-like entities.

Recommended ownership:

| Template | Owns |
| --- | --- |
| `weapon_template` | weapon id, fire mode, ammo, cooldown, reload, burst/RPM policy, references to projectile or segment collider templates |
| `projectile_template` | projectile id, movement model, sync mode, lifetime, damage behavior, collider template reference |
| `collider_template` | reusable shape geometry and purpose/layer defaults |
| `actor_template` | actor stats, movement, loadout, and hitbox-derived actor collider inputs |

Projectile collider lookup should prefer `projectile_template_id`, not plain
`EntityType::kProjectile`, because fireballs, rockets, and homing missiles can
all be projectile entities with different shapes.

## Shape Requirements

Supported gameplay shape classes should cover:

- AABB or oriented box for actors, beam volumes, and box-like projectiles.
- Sphere for fireballs, homing-fireball style missiles, and simple radial
  projectiles.
- Segment for rifle and shotgun hitscan traces.
- AOE projectile volumes for explosions and persistent area damage.

Beam collider shape is an oriented box for gameplay collision. Unity may render
it as a capsule-like laser, accepting small visual/gameplay differences for
better collision-query performance.

Homing missile data structures should preserve orientation support in snapshots,
rollback history, and collider views. First-stage gameplay colliders should
prefer sphere shapes for homing projectiles when possible, so orientation can be
visual-only until non-spherical homing collision is required.

## Weapon Effects

### Rifle And Shotgun

Rifle and shotgun effects should be modeled as short-life segment collider
instances. A segment has `length` or `max_range` and may have spread configured
as `scatter_degrees`. It does not need projectile size because it is not a physical
bullet.

All fired segments should be queryable through `Kernel_QueryColliderShapes`,
including misses. Unity owns visual debugger retention.

Default segment lifetime should be 3 ticks. This gives clients a small window to
observe fired segments while keeping kernel state short-lived. Templates may
override the value, including 10 ticks if a longer kernel-visible window is
needed.

Segment damage must be guarded so short lifetime does not cause repeated damage.
Use `has_resolved_damage` on the `ColliderInstance` for the first version. If
multi-target or per-target repeated rules are needed later, this can evolve into
a target hit cache.

### Beam

Beam is a persistent entity. While the player keeps sending fire input and the
weapon can fire, the beam is refreshed. It disappears when the refresh condition
fails and its lifetime expires.

Ammo consumption follows the existing cooldown model: when `can_fire` passes,
the weapon consumes one ammo and sets `next_fire_tick = current_tick +
cooldown_ticks`. Beam refresh then updates the persistent beam instance.

The beam collider is an oriented box and performs query-volume damage every tick
while active.

### Explosion

Explosion should be represented as an AOE projectile/entity spawned by impact,
not as a special scalar field on the original projectile. Its collider template
owns the AOE shape and radius. Damage interval controls repeated damage:

- one-shot explosion: `damage_interval_ticks == exist_ticks`
- persistent AOE: `damage_interval_ticks < exist_ticks`

Damage falloff belongs to the AOE projectile damage behavior.

## Query API Policy

`Kernel_QueryColliderShapes` should return active runtime collider instances
with the existing query spirit: entity filter, purpose filter, and shape data for
debug visualization. Do not add `tick` or `source_template_id` yet. If Unity
debug grouping later needs them, add fields in a separate ABI/API revision.

The query must report the same size, transform, purpose, and layer that gameplay
collision uses for that tick.

## Collision Performance Policy

Collision query should move toward a lightweight broad phase plus narrow phase:

- Store active collider instances in a runtime collider registry.
- Maintain a conservative world AABB per instance.
- Use layer, purpose, owner, and ignored-entity filters before expensive tests.
- Broad phase returns candidate colliders by AABB overlap or segment bounds.
- Narrow phase performs shape-specific tests, such as segment vs AABB, sphere vs
  AABB, oriented box queries, and AOE volume overlaps.
- Update world bounds only for dirty or moving instances where practical.

This is intended to reduce repeated full-world scans as projectile, segment,
beam, and AOE counts increase.

## Open Items For Implementation Planning

- Exact YAML schema for the new `projectile_templates` directory.
- Exact `segment_collider_template` schema for length, `scatter_degrees`, and
  lifetime override.
- Whether actor collider runtime instances are materialized directly from actor
  templates or via collider templates referenced by actor templates.
- Whether non-spherical homing projectiles should use oriented box or capsule
  first when gameplay-critical orientation becomes required.
- ABI migration shape for `KernelColliderShapeView` if new shape fields exceed
  the existing struct.
