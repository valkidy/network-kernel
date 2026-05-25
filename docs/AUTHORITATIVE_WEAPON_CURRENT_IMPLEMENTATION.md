# Authoritative Weapon Current Implementation

This document records the current authoritative weapon implementation as of
native kernel ABI v12. It describes what exists today, not the full future
weapon design.

## Server Authority Model

Weapon gameplay follows `docs/NETCODE_SYNC_POLICY.md`:

- Clients submit input, aim direction, selected weapon, and `client_action_id`.
- Hit detection, projectile interactions, damage, death, and gameplay entity
  spawn/despawn decisions are server-owned.
- Clients may predict local projectile visuals, but authoritative projectile
  state is bound back by `owner_peer + client_action_id`.
- Area effects, beams, homing guidance, projectile reactions, and damage are
  resolved in fixed server simulation.

The current native public ABI is:

```text
KERNEL_ABI_VERSION == 12u
KERNEL_MAX_WEAPONS == 7u
```

The projectile interaction foundation did not change snapshot packet layout,
protocol packet layout, or Kernel C ABI.

## Implemented Runtime

The current weapon foundation includes these systems:

- `DamageRequest -> DamagePipeline -> DamageSystem` is the unified damage path
  for hitscan, shotgun, projectile, area effect, beam, and homing damage.
- YAML weapon templates are owned by `game_server`, while the engine/kernel own
  mechanics validation and simulation.
- Area-effect weapons spawn server-authoritative `EntityType::kAreaEffect`
  entities that damage through the damage pipeline.
- Beam weapons use dedicated `EntityType::kBeam` entities and a server-side DPS
  accumulator.
- Homing projectile v1 is fire-and-forget only. Boost prediction is
  deterministic; guided/lost phases are server authoritative.
- Collision query helpers support ray/AABB, segment/swept-sphere hit
  collection, sphere/box overlap, deterministic sorting, collision masks, and
  default projectile/area-effect exclusion.
- Projectile hit processing now has internal `ProjectileHitRecord` and
  `ProjectileHitOutcome` helpers so movement, hit collection, damage emission,
  and destroy decisions have a single extension point.
- ProjectileInteractionSystem v1 is an internal C++ foundation. Rules live on
  `World`; matching projectiles can be destroyed and may spawn an
  authoritative area effect.

## Projectile Interaction v1

Projectile interactions currently support only internal rules:

- match by `lhs_weapon_id` and `rhs_weapon_id`
- optional symmetric matching
- destroy lhs and/or rhs projectile
- optional resulting `AreaEffect` spawn

Interactions run after projectile movement and before normal damageable hit
processing. They never submit `DamageRequest` directly; damage must come from a
spawned gameplay entity such as an area effect. Processing order is
deterministic by projectile net-id pair.

Not implemented in v1:

- YAML reaction catalog/loading
- public Kernel/GameServer reaction metadata query
- dedicated `ProjectileReactionEvent`
- bounce or attach runtime
- projectile-vs-projectile direct damage

## Unity Validation Caveat

Native Bazel tests are independent from Unity package validation. The weapon
foundation is currently implemented in native C++ runtime and internal engine
APIs.

Unity package validation is stale relative to native ABI v12:

- `plugins/com.network-example.kernel` is not currently an expanded package
  source tree with `package.json`, runtime C# files, editor tests, and staged
  dylib.
- The existing `plugins/output/com.network-example.kernel-0.6.4.tgz` contains
  C# bindings whose managed ABI constant remains at 8, while native
  `kernel_types.h` reports
  `KERNEL_ABI_VERSION == 12u`.
- Therefore Unity package builder verify/Editor smoke cannot be used as a
  passing signal for the current native weapon foundation until the Unity
  package is resynced.
