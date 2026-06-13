# Authoritative Weapon Foundation Status

This document separates remaining foundation work from feature expansion. Use it
as the baseline for future weapon milestones.

## Foundation Complete

- Server-authoritative damage path is unified through `DamagePipeline`.
- Weapon mechanics are configurable through native structs and `game_server`
  YAML templates for seven slots.
- Runtime support exists for hitscan, shotgun, projectile explosion/direct hit,
  piercing segment by `max_hit_count`, area effect, beam, and homing v1.
- Collision query helpers provide deterministic hit/overlap collection with
  collision layers, entity types, and best-effort normals.
- Projectile hit processing has a dispatcher-style foundation for future hit
  responses.
- ProjectileInteractionSystem v1 exists as internal C++ rules stored on
  `World`.

## Foundation Still Needed

- Resync Unity package source and staged package with native ABI v17. Until
  then, Unity verify/Editor smoke results do not represent current native
  behavior.
- Update Unity C# mirror structs, constants, capability flags, and smoke tests
  for ABI v17 before treating Unity validation as required.
- Keep ABI/status docs aligned with the current native header whenever
  `KERNEL_ABI_VERSION`, public structs, or exported query functions change.
- Document ProjectileInteractionSystem v1 as internal-only behavior. Its current
  event surface is existing spawn/despawn events, not a dedicated
  `ProjectileReactionEvent`.
- Decide whether a dedicated gameplay event layer is required before exposing
  projectile reactions beyond internal tests.

## Pending Feature Expansions

These are intentionally not foundation blockers:

- YAML reaction catalog/loading.
- GameServer/Kernel public reaction metadata query.
- Bounce runtime using collision normals.
- Attach runtime for target/surface sticking.
- Material penetration budget and target thickness beyond current
  `max_hit_count` piercing.
- Advanced projectile reactions: spawn projectile/beam, convert projectile,
  multi-result reactions.
- Homing retarget, explicit lock-on input, and predictive guidance.
- Beam oriented box/rectangle collision.
- Friendly-fire and team policy expansion.
- Unity gameplay-facing wrappers for weapon metadata beyond ABI smoke.

## Validation Baseline

For native weapon foundation changes, run:

```text
bazel test //engine/src/tests/simulation_tests:all
bazel test //engine/src/tests/world_tests:all
bazel test //engine/src/tests/sync_tests:all
bazel test //engine/src/tests/kernel_tests:all
bazel test //engine/src/tests/protocol_tests:all
bazel test //game_server:all
```

For documentation-only changes, a narrower verification is acceptable:

```text
git diff --check
rg -n "KERNEL_ABI_VERSION == [0-9]|AbiVersion = [0-9]|v[0-9]" docs plugins/output
bazel test //engine/src/tests/kernel_tests:kernel_api_test //engine/src/tests/protocol_tests:all
```

Unity package builder verification is currently a known validation gap, not a
required passing check, until the Unity package source is restored and aligned
with ABI v17.
