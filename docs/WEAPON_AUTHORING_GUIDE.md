# Weapon Authoring Guide

For designers adding or tuning a weapon. Everything here is YAML under
`game_server/`; no code changes are needed for a new weapon.

## One weapon is four files

Create them in this order. Each one is referenced by the next.

| # | File | Directory | Owns |
|---|---|---|---|
| 1 | shot / projectile template | `projectile_templates/` | **damage**, **collision_mask**, travel, lifetime |
| 2 | fire action template | `action_templates/` | rate of fire, trigger mode, ammo per shot |
| 3 | reload action template | `action_templates/` | reload time and reload shape |
| 4 | weapon template | `weapon_templates/` | magazine, range, spread, and the three references above |

Then add the weapon's `id` to a loadout in `entity_templates/` (for example
`player.yaml`'s `weapon_slots`), or nothing will ever hold it. A loadout holds
at most 4 weapons.

Ids must be unique within each directory. Weapon ids are 0-255.

Files 2 and 3 can be shared with an existing weapon; file 1 cannot. If the same
gun is wanted on both sides, it is two weapons with two projectile templates —
see "Name a side" below for why.

## Where each number lives

This is the part that used to be ambiguous. There is now exactly one place to
author each value.

| You want to change | Edit | Field |
|---|---|---|
| Damage | projectile template | `damage` |
| What the shot can hit | projectile template | `collision_mask` |
| Rate of fire | fire action template | `commit_interval_ticks` |
| Reload time | reload action template | `commit_offset_ticks` |
| Magazine size, spare mags | weapon template | `magazine_size`, `reserve_magazines` |
| Range | weapon template | `max_range` |
| Pellet count and spread | weapon template | `pellet_count`, `pellet_spread` |

The weapon template **cannot** author `damage` or `collision_mask`. Writing
either is a load error, not a value that quietly loses to another one.

All timing is in **ticks at 30 Hz**: 30 ticks = 1 second.

## Weapon template

```yaml
id: 12
name: SMG
weapon_type: hitscan
magazine_size: 40
fire_action_template: smg_fire
reload_action_template: smg_reload
max_range: 60.0
segment_collider: rifle_segment
projectile_template: smg_shot
```

**Required for every weapon type**

`id`, `name`, `weapon_type`, `magazine_size`, `fire_action_template`,
`projectile_template`

**Required by type**

| `weapon_type` | Also required |
|---|---|
| `hitscan` | `max_range`, `segment_collider` |
| `shotgun` | `max_range`, `segment_collider`, `pellet_count`, `pellet_spread` |
| `projectile` | — |
| `area_effect` | — |
| `beam` | — |

**Optional**

| Field | Default |
|---|---|
| `reserve_magazines` | `6` |
| `reload_action_template` | the catalog's `shared_reload`, 30 ticks |
| `burst_count` | `1` (projectile weapons: shots per trigger pull) |
| `burst_spread_degrees` | `0` |

`hitscan` and `shotgun` resolve instantly by raycast and spawn nothing. They
still name a projectile template, because that is where their damage and target
mask are authored — and it is also what the client uses to find the tracer art.

## Projectile template

The weapon's shot. `damage` is always the top-level key, whatever the type.

```yaml
id: 12
name: smg_shot
type: standard
collider_template: rifle_segment
damage: 18
collision_mask: actor | terrain | static_obstacle
damage_shape: direct_hit
speed: 200.0
lifetime_ticks: 3
```

**Required**: `id`, `name`, `collider_template`, `damage`, `speed`,
`lifetime_ticks`

**Optional**

| Field | Default | Notes |
|---|---|---|
| `type` | `standard` | `standard`, `area_effect`, `beam` |
| `movement_model` | `linear` | `linear`, `parabolic`, `homing` |
| `sync_mode` | `hybrid_deterministic_then_snapshot` | `area_effect` defaults to `server_snapshot_only` |
| `hit_response` | `destroy` | |
| `damage_shape` | `direct_hit` | `none` requires `damage: 0` |
| `collision_mask` | `actor \| terrain \| static_obstacle` | |
| `max_hit_count` | `1` | how many targets one shot may hit |
| `gravity` | `{0, 0, 0}` | |

### collision_mask tokens

`damageable` (= `player_side | hostile_side | neutral`), `player_side`,
`hostile_side`, `neutral`, `actor`, `limb`, `terrain`, `static_obstacle`, `prop`,
`projectile`, `none`. Combine with `|`.

`limb` is opt-in per weapon: without it, a shot passes through a creature's legs
and only its body can be hit.

`actor` and `damageable` are the **same mask** — both sides and neutral. So the
default `actor | terrain | static_obstacle` means *hits everyone*, which is
where friendly fire comes from.

### Name a side, or the shot hits your own team

**A side in `collision_mask` is an absolute category, not a category relative to
whoever fired.** There is no "enemies of the shooter" token. A projectile that
names `hostile_side` hits hostile-side actors no matter who pulled the trigger,
so the rule is:

| The weapon is held by | `collision_mask` names | Example |
|---|---|---|
| A player | `hostile_side` | `beam_rifle_beam` |
| An enemy agent | `player_side` | `beam_sentry_beam` |
| An agent fighting *for* the player | `hostile_side` | `allied_sentry_beam` |

Omitting the side is not a neutral choice — it selects both.

This is why the same weapon needs a per-side twin, and the twin is mandatory
rather than stylistic: `apply_weapon_template_references` stamps `weapon_id`
onto the projectile template it resolves, so **a projectile template belongs to
exactly one weapon** and cannot be shared even when the mask would suit both.

Fire and reload action templates, and collider templates, have no such
back-stamping — they are resolved by reference and only their id is copied, so
two weapons may share them freely. `allied_sentry_rifle` reuses
`beam_sentry_fire`, `beam_sentry_reload` and `beam_sentry_beam_box`, and
authors only its own projectile.

### The other half of a side lives on the target

A mask is matched against the target's **hit collider `layer:`**, which is where
an actor's side actually lives. Not `camp` — camp only decides what an AI looks
for.

So a weapon's side authoring only works if the actors it is aimed at are
authored on the layer it names:

| Collider template | `layer:` | Worn by |
|---|---|---|
| `player_hit_aabb` | `player_side` | `player` |
| `sentry_grunt_hit_aabb` | `hostile_side` | every enemy agent |
| `allied_sentry_hit_aabb` | `player_side` | `allied_beam_sentry` |

An agent authored `camp: player_side` while still wearing a `hostile_side` hit
collider ends up hostile to everyone: the player's `hostile_side` weapons shoot
it, and the enemy's `player_side` weapons pass straight through it. Adding a
friendly unit therefore means authoring a hit collider on the player's layer,
not only setting its camp.

**Known gap**: a projectile that names *no* side (`spammer_projectile`, which is
`terrain | static_obstacle`) has an empty gameplay-category mask, so it passes
through every side-layered collider — actors and deployable cover alike. See
`collider_templates/ice_block_hitbox.yaml`, which hit the same wall from the
target's end.

### type: area_effect

Adds a damage-over-time block. `speed` is optional here and defaults to `0`,
which is a field that sits where it was spawned — every blast wants that.

```yaml
type: area_effect
collider_template: area_effect_sphere
damage: 12               # per interval
lifetime_ticks: 6
damage_behavior:
  type: area_interval
  damage_interval_ticks: 2
  falloff: none          # or linear
```

`movement_model`, `hit_response`, and `damage_shape` are **rejected** here: an
area effect always ends on its lifetime and takes its damage from
`damage_behavior`, and its motion model is fixed to linear — homing stays a
standard-projectile model — so none of the three can mean anything.

### A travelling area effect

Author a non-zero `speed` and the field travels instead of sitting still: a
front that sweeps across the ground rather than a blast. It keeps applying its
`damage_behavior` every `damage_interval_ticks` to whatever is inside it as it
goes, and its direction is whatever direction it was spawned facing.

```yaml
type: area_effect
speed: 6.0               # metres per second; 0 (default) stays put
lifetime_ticks: 90
damage_behavior:
  type: area_interval
  damage_interval_ticks: 2
  falloff: none
```

**It passes through terrain.** A travelling area effect is advanced but not
swept against the world, so it will cross walls and floors. Author one only
where that reads as intended until a swept query is added.

`hit_instigator` (default `false`) is accepted here and rejected everywhere
else. An area effect normally filters the actor that fired it out of its overlap
query, so a weapon's own blast can neither hurt nor push its shooter, and that
filter follows a spawn chain — a rocket's explosion is filtered against the
actor who fired the rocket. Authoring `hit_instigator: true` turns the filter
off, which is what a self-knockback (rocket jump) needs. One query feeds both
the damage and the impact trigger, so it buys self-damage along with the push.

```yaml
type: area_effect
hit_instigator: true     # the shooter is hit by their own blast
```

`sync_mode` is accepted, and defaults to `server_snapshot_only` rather than the
`hybrid_deterministic_then_snapshot` every other projectile type defaults to.
Authoring `local_predicted_deterministic` is what lets the client predict an
impact impulse on the local player from an area effect it fired itself.

### type: beam

```yaml
type: beam
collider_template: beam_oriented_box
damage: 1                # per tick while the beam is up
speed: 0.0
lifetime_ticks: 0
beam:
  length: 8.0
  radius: 0.25
  lifetime_ticks: 2      # optional, default 2
```

The beam block carries no damage or mask of its own — both come from the
top-level keys.

## Action templates

Fire and reload are both action templates; they differ only in their values.
Every field is required. `flags` may be an empty list.

```yaml
# smg_fire.yaml -- 900 RPM full auto
id: 4120
name: smg_fire
trigger_mode: hold
flags: [cancel_on_release, cancel_on_death, cancel_on_weapon_change, cancel_before_first_commit]
ammo_cost_per_commit: 1
commit_offset_ticks: 0
commit_interval_ticks: 2
max_commit_count: 0
recovery_ticks: 4
hold_input_timeout_ticks: 6
```

```yaml
# smg_reload.yaml -- 1.2 s
id: 4121
name: smg_reload
trigger_mode: press
flags: [cancel_on_death, cancel_on_weapon_change, cancel_before_first_commit]
ammo_cost_per_commit: 0
commit_offset_ticks: 36
commit_interval_ticks: 0
max_commit_count: 1
recovery_ticks: 0
hold_input_timeout_ticks: 0
```

### Rate of fire

`commit_interval_ticks` is the gap between shots. RPM = `1800 / interval`, so
only these values exist at 30 Hz:

| ticks | 1 | 2 | 3 | 4 | 5 | 6 | 8 | 20 |
|---|---|---|---|---|---|---|---|---|
| RPM | 1800 | 900 | 600 | 450 | 360 | 300 | 225 | 90 |

`trigger_mode: hold` with `max_commit_count: 0` is full auto.
`trigger_mode: press` with `max_commit_count: 1` is semi-auto.
A fire action must have `commit_interval_ticks` greater than 0.

### Reload

`commit_offset_ticks` is the reload time — the magazine refills on the action's
single commit, so delaying that commit is what makes the reload take time.

Because a reload is a full action template and not just a duration, it can also
change shape. A shell-at-a-time reload the player can interrupt would be
`max_commit_count: 0` with a `commit_interval_ticks` per shell — note that the
refill amount itself is not yet data-driven, so that shape needs an engineer.

## Checklist for a new weapon

1. `projectile_templates/<name>_shot.yaml` — damage and collision_mask
2. **Name the side in `collision_mask`** — `hostile_side` for a player's weapon,
   `player_side` for an enemy's. Leaving it out means it hits both.
3. `action_templates/<name>_fire.yaml` — rate of fire (shareable with another
   weapon)
4. `action_templates/<name>_reload.yaml` — reload time (shareable)
5. `weapon_templates/<name>.yaml` — magazine, range, and the three references
6. Add the weapon id to `weapon_slots` in an `entity_templates/` loadout
7. Rebuild the catalog bundle and ship the same bundle to client and server

## Common load errors

| Message | Cause |
|---|---|
| `unknown field: damage` (in a weapon template) | Damage belongs in the projectile template |
| `unknown field: collision_mask` (in a weapon template) | Same — it moved with damage |
| `unknown field: reload_ticks` | Reload time is `commit_offset_ticks` in a reload action template |
| `instant weapon requires projectile_template` | A hitscan or shotgun weapon needs one too |
| `instant weapon requires segment_collider` | hitscan and shotgun only |
| `weapon fire_action_template requires commit_interval_ticks greater than 0` | A fire action needs a real cadence |
| `unknown projectile_template reference: X` | Name mismatch, or the file is not in `projectile_templates/` |
| `duplicate projectile template id` | Pick an unused id |

Two failures that load cleanly and only show up in play:

| Symptom | Cause |
|---|---|
| The weapon damages its own side | `collision_mask` names no side, or the wrong one. See "Name a side" above |
| A shot passes through an actor that should be hittable | The actor's hit collider `layer:` is not a side the mask names — or the mask names no side at all |

The catalog hash changes whenever any of this changes, so the client and the
server must load the **same** bundle or the handshake fails.
