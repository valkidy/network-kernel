# Data Driven Template Design Reference

## Purpose

This document is the compact policy reference for data-driven gameplay templates
in `game_server`. It merges the first completed template pass with the broader
data-driven rules for template loading, runtime binding, serialization, and ABI
safety.

The current file layout is:

```text
game_server/gameplay_catalog.yaml
  -> weapon_template_dir: game_server/weapon_templates/*.yaml
  -> projectile_template_dir: game_server/projectile_templates/*.yaml
  -> entity_template_dir: game_server/entity_templates/*.yaml
  -> collider_template_file: game_server/collider_templates/default.yaml
  -> player.entity_template
  -> director entity template spawn policy
```

This policy describes how to extend that layout without turning authoring YAML
into runtime state, unstable string lookup, or an accidental ABI migration.

## Core Rules

- YAML files define reusable templates and composition policy, not live runtime
  objects.
- Runtime instances are created by explicit binding from loaded templates.
- Runtime hot paths must use stable integer IDs, enum values, or resolved
  handles, not string comparison.
- Human-readable names are authoring/debug labels. Numeric IDs are canonical for
  runtime storage, deterministic tests, hashes, serialization, and references
  after import.
- Unknown fields should be rejected by default. Optional fields need documented,
  deterministic defaults.
- Required references must be validated during load unless lazy resolution is
  explicitly designed.
- Gameplay-affecting data must be included in `compute_gameplay_catalog_hash`.
- ABI-facing C/C#/Unity structs must stay plain, fixed-width, and versioned.

## Template And Runtime Separation

A template is static data loaded from YAML, such as a weapon, projectile, actor,
collider, AI behavior, or status-effect definition. A runtime instance is a live
object created from template data, such as a player's weapon, a spawned
projectile, or an active enemy.

Every field added to the data model must be classified as one of:

- template data
- runtime instance data
- derived/cache data
- debug/editor-only data

Template YAML must not contain runtime-only values such as instance IDs, current
HP, cooldown remaining, projectile position, server tick, owner entity, or
current ammo unless the file is explicitly a save-game, replay, or runtime
snapshot format.

Runtime instances must keep their source template ID, initialize all mutable
runtime fields during binding, and never mutate or own template memory. They may
copy selected template values only for performance or determinism.

## IDs, Names, And Schema

Existing YAML uses stable numeric `id` plus human-readable `name`. In policy
terms, `id` is the template ID and `name` is a debug/authoring label. This
document does not require renaming current files to `template_id` or
`debug_name`.

Rules:

- Template IDs must be deterministic, stable, and unique within their domain.
- Agents must not invent random IDs unless explicitly requested.
- File paths are useful for import/debug errors but must not be the only
  identity.
- Duplicate IDs and duplicate names in the same domain must be rejected.
- String-to-ID and string-to-enum conversion is allowed during import,
  validation, editor tooling, debug tooling, and error reporting.
- Persistent IDs must not use `std::hash` or unstable platform-dependent hash
  behavior. If hashing is used, the algorithm, seed, and collision handling must
  be fixed and documented.

Current catalog-level versioning uses `catalog_version`. Whether individual
template files should also gain `schema_version` is an open discussion item
below.

## Mapping, Validation, And Serialization

Each YAML schema must map explicitly to known C++ fields. The loader should:

1. Read YAML.
2. Validate catalog/template version.
3. Reject unknown fields unless a forward-compatible mode is designed.
4. Convert authoring names and enum strings to canonical integer IDs.
5. Validate required fields, defaults, numeric ranges, enum values, duplicate
   IDs, and cross-template references.
6. Create C++ template structs.
7. Register templates into read-only template registries.

Errors should be structured enough for both tooling and humans: error code,
file path, template ID/name if known, field name or field ID, line/column when
available, and a clear message.

Serialization and bundling must be deterministic:

- stable field order
- sorted unordered template collections by stable ID
- no runtime-only fields in template YAML
- no derived/cache fields unless explicitly part of the format
- equivalent filesystem and bundle-memory loads
- clean, repeatable git diffs and catalog hashes

Descriptor-based mapping, stable field IDs, and generated schema tables are
preferred for larger generic systems, but the current v1 layout does not require
an immediate rewrite away from existing loaders.

## Runtime Binding And Registries

Loaded templates live in template registries keyed by stable integer ID. Runtime
objects live in separate runtime registries keyed by runtime instance ID.

Binding functions create runtime instances from templates. They must:

- fail explicitly when the template does not exist
- initialize all runtime fields
- resolve required references
- avoid string lookup in hot paths
- keep template registries read-only after load unless hot reload is explicitly
  supported

If hot reload is added later, cached template pointers or handles need a clear
invalidation policy.

## Current Data Surface

`gameplay_catalog.yaml` is the composition root:

```yaml
catalog_version: 1
weapon_template_dir: weapon_templates
projectile_template_dir: projectile_templates
entity_template_dir: entity_templates
collider_template_file: collider_templates/default.yaml
player:
  entity_template: player
```

Ownership boundary:

| Data kind | Owner | Policy |
| --- | --- | --- |
| Catalog source paths | `gameplay_catalog.yaml` | Points to template files and directories. |
| Catalog version | `gameplay_catalog.yaml` | Compatibility and hash input. |
| Weapon mechanics | `weapon_templates/*.yaml` | Reusable weapon behavior by stable weapon ID. |
| Projectile mechanics | `projectile_templates/*.yaml` | Movement, sync, lifetime, damage, collider reference. |
| Area/beam target rules | Weapon template mode blocks | Use `collision_mask`; do not add `collision_flags`. |
| Entity composition | `entity_templates/*.yaml` | Reusable actor/director definition by stable entity ID/name. |
| Player actor choice | `gameplay_catalog.yaml` | References an entity template. |
| Agent spawn policy | director entity template | World-rule Director AI owns initial and replacement spawn policy. |
| Collider geometry | `collider_templates/default.yaml` | Kernel-packed collider shapes; actor/projectile templates reference these shapes. |

## Gameplay Catalog Policy

`gameplay_catalog.yaml` should answer which template sources are loaded and
which default actor templates/spawn policy are used. It should not own weapon
mechanics, actor stats, collider geometry, friendly-fire rules, or ABI layout
changes.

`weapon_template_dir`, `projectile_template_dir`, `entity_template_dir`, and
`collider_template_file` are required for new catalog data. `actor_template_dir`
and `actor_template` references remain compatibility paths, but new authoring
should use `entity_template_dir` and `entity_template` references.

## Weapon Template Policy

Weapon templates own reusable weapon mechanics:

```yaml
id: 0
name: Rifle
weapon_type: hitscan
magazine_size: 30
reserve_magazines: 6
damage: 25
cooldown_ticks: 3
reload_ticks: 30
max_range: 100.0
```

Type-specific fields stay with their weapon mode:

- `hitscan`: range and direct-hit mechanics.
- `shotgun`: range, pellet count, and pellet spread.
- `projectile`: projectile movement, lifetime, sync mode, hit response, damage
  shape, explosion radius, gravity, and projectile collision mask.
- `area_effect`: area lifetime, radius, and area collision mask.
- `beam`: beam lifetime and beam collision mask.

Weapon templates must not own actor loadouts, spawn policy, actor HP/movement,
enemy counts, or player/enemy selection.

`reserve_magazines` counts spare full magazines for reloads. If omitted, the
loader defaults it to `6`; templates may author larger practical maximums such
as `65535` for weapons intended not to exhaust reserves during normal match
lengths.

### Collision Mask Policy

Use `collision_mask` as the single target-layer rule:

```text
collision_mask = target layers this weapon effect can hit
```

Do not introduce `collision_flags`. Friendly fire is represented by the mask:

```yaml
collision_mask: hostile_side | player_side
collision_mask: player_side
collision_mask: none
```

Supported tokens are `damageable`, `hostile_side`, `player_side`, `neutral`,
`projectile`, `area_effect`, `none`, and numeric `0`. `damageable` expands to
`player_side | hostile_side | neutral`. The parser supports `|` expressions.
Unknown or empty tokens must be rejected.

`collision_mask == 0` means the effect may still fire, exist, spawn, and sync,
but damage/hit queries must produce no damage targets.

## Actor Template Policy

Use the term `actor template`. It covers players, enemies, NPCs, bosses,
turrets, and other controllable or damageable gameplay actors without
confusing the concept with ECS internals.

The first actor-template implementation is game-server-local. It configures
runtime player and enemy combat state, but is not packed into
`KernelGameplayCatalogDefinition` and does not change the kernel or Unity
managed ABI.

Actor entity templates own reusable actor facts. Hostility is determined by
camp/relation and AI policy; `enemy` is not an entity type:

```yaml
id: 2
name: sentry_grunt
entity_type: actor
actor_type: agent
camp: enemy_side
health:
  hp: 240
  max_hp: 240
movement:
  move_speed_meters_per_second: 2.5
hitbox:
  center: {x: 0.0, y: 0.8, z: 0.0}
  half_extents: {x: 0.4, y: 0.8, z: 0.4}
weapon_slots:
  - 2
active_weapon_slot: 0
animations:
  idle: 0
  chasing: 1
ai:
  controller: sentry
  profile: default
```

Director entity templates own world-rule spawn policy:

```yaml
id: 100
name: earth_mother
entity_type: director
server_only: true
transform:
  position: {x: 6.0, y: 0.0, z: 0.0}
ai:
  controller: director
  profile: earth_mother
  tick_interval: 10
director:
  kind: world_rule
  spawn:
    target_count: 10
    entity_template: sentry_grunt
    radius: 5.0
```

Validation:

- `id` must be nonzero and unique.
- `name` must be non-empty and unique.
- `entity_type` is `actor` or `director`; `entity_type: enemy` is rejected.
- actor templates declare `actor_type` and `camp`.
- health values must be positive, with `hp <= max_hp`.
- movement speed must be positive.
- hitbox center and half extents are required; half extents must be positive.
- `weapon_slots` must contain 1 to 4 loaded weapon template references.
- `active_weapon_slot` defaults to `0` and must be in range.
- enemy AI derived from actor data must reference usable weapon timing, reload,
  and magazine values.

## Collider Template Policy

Collider templates are already data-driven and kernel-packed:

- `templates`: reusable collider geometry and layer/purpose metadata.

Current policy forbids a parallel `entity_type -> collider_template` binding
table. Actor templates and projectile templates each own their collider template
reference, so every runtime entity family has one documented resolution path.
This avoids the failure mode where gameplay materialization uses a
template-specific collider while client/debug tooling falls back to a generic
binding and observes the wrong shape.

## Runtime Flow

Loading order:

1. Load the gameplay catalog root.
2. Load weapon templates from `weapon_template_dir`.
3. Load collider templates from `collider_template_file`.
4. Load projectile templates from `projectile_template_dir` and resolve weapon
   projectile references.
5. Load entity templates from `entity_template_dir` and validate actor loadouts
   against loaded weapon templates.
6. Resolve `player.entity_template` and director spawn entity-template refs.
7. Compute the gameplay catalog hash from all gameplay-affecting data.
8. Validate the assembled `GameServerGameplayConfig`.

Player runtime:

1. Kernel emits `PlayerJoined`.
2. `GameServer::configure_player` resolves the configured player actor
   template.
3. `make_player_combat_state` builds combat state from actor stats/loadout.
4. Game server applies only the actor's configured weapon slots.

Agent/director runtime:

1. `GameServer` bootstraps configured server-only director entities.
2. `DirectorRuntime` enqueues entity-template create work to maintain target
   agent counts.
3. `EntityLifecycleSystem::create_entity` materializes template-declared
   components.
4. `AgentRuntimeManager` observes live agent actors from kernel state and
   applies game-server weapon mechanics for sentry controllers.

## ABI Boundary

The kernel gameplay catalog ABI now carries the compact runtime-facing template
surface needed by server materialization:

- weapons are packed into kernel weapon mechanics definitions
- projectile templates are loaded independently and packed for kernel use
- collider templates are packed for kernel collision/debug queries
- actor templates are packed into `KernelActorTemplateDefinition`
- entity templates are packed into `KernelEntityTemplateDefinition` for
  component-driven actor and server-only director creation

Authoring-only YAML names, file paths, and editor metadata still remain in
`game_server`. ABI-facing templates use stable numeric ids and fixed-width
plain C structs only.

ABI-facing structs must avoid `std::string`, `std::vector`, virtual methods,
non-trivial constructors/destructors, implicit array ownership, and unstable
layout. Use fixed-width integers, explicit counts, and struct versioning.

## Hash And Bundle Policy

The gameplay catalog hash must change when gameplay-affecting template data
changes. It should cover:

- catalog version
- weapon IDs, names, mechanics, mode-specific data, collision masks, projectile
  sync modes, and derived projectile data
- player and enemy actor template references
- enemy spawn position, count, radius, and seed
- actor IDs, names, entity types, health, movement, hitboxes, weapon slots,
  active slots, animation states, AI tunables, collider template references,
  and vision collider template references
- collider template definitions

Hashing must sort unordered template collections by stable IDs so filesystem
enumeration order does not affect compatibility checks.

Gameplay catalog bundles must include the catalog root and every referenced
template source needed to reconstruct the same config from memory:

- `gameplay_catalog.yaml`
- `weapon_templates/*.yaml`
- `projectile_templates/*.yaml`
- `entity_templates/*.yaml`
- `collider_templates/default.yaml`

Filesystem loading and bundle-memory loading should produce equivalent gameplay
configuration and catalog hash values.

## Extension Rules

Before adding a new data-driven field, answer:

1. Is the field reusable across actors, weapons, projectiles, or colliders?
   Put it in the matching template.
2. Is the field scenario-specific or encounter-specific?
   Put it in `gameplay_catalog.yaml`.
3. Is it runtime state?
   Put it in runtime instance data, not template YAML.
4. Does the kernel need it during simulation?
   Pack it only through an existing kernel catalog structure unless a new ABI
   design is approved.
5. Does it affect compatibility or gameplay outcome?
   Include it in the catalog hash.
6. Does it reference another template?
   Allow authoring-friendly names when useful, then validate and store
   canonical IDs.
7. Does it introduce enums, defaults, or serialization?
   Document stable enum values, deterministic defaults, and field order.

## Open Discussion Items

These facts are not decided by the current design:

- Whether individual weapon/projectile/actor/collider template files should add
  `schema_version`, or whether `catalog_version` remains the only v1 version
  gate.
- Whether to introduce stable field IDs and descriptor-based generic
  serialization now, or defer until more schemas need shared tooling.
- Whether authoring YAML should continue accepting names and IDs for references
  long term, or move to a stricter numeric-ID mode with names as labels only.
- The official numeric ID ranges for weapons, projectiles, actors, colliders,
  AI profiles, and future status-effect templates.
- The null-reference convention for optional template references, such as `0`
  versus `-1`.
- Whether hot reload is required; if yes, cached template pointer invalidation
  must be designed before use.
- Whether future actor archetypes need additional collider metadata beyond the
  existing actor-template `collider_template` reference.
- The save-game/replay/runtime snapshot format, which must remain separate from
  template YAML if introduced.

## Review Checklist

- `gameplay_catalog.yaml` remains a composition root.
- Weapon mechanics stay in weapon templates.
- Projectile mechanics stay in projectile templates.
- Actor stats and loadouts stay in actor entity templates.
- Agent spawn policy stays in director entity templates.
- Runtime state does not enter template YAML.
- Entity templates remain read-only authoring/catalog data; runtime ownership
  stays with ECS/kernel state.
- Collision targeting uses `collision_mask`, not `collision_flags`.
- `collision_mask: none` and `collision_mask: 0` still allow projectile
  spawn/sync while preventing damage targets.
- Template IDs and names are validated for uniqueness.
- Template references are validated at load time.
- Runtime lookup uses canonical IDs, enums, or resolved handles.
- Unknown fields, invalid types, invalid enums, and malformed values fail load.
- Optional fields have deterministic documented defaults.
- Gameplay-affecting fields are included in catalog hash computation.
- Serialization, hashing, and bundle loading are deterministic.
- Filesystem and bundle-memory catalog loading stay equivalent.
- ABI-facing structs remain plain and stable.
