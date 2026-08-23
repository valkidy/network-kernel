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
collision_mask: actor | terrain | obstacle
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
| `collision_mask` | `actor \| terrain \| obstacle` | |
| `max_hit_count` | `1` | how many targets one shot may hit |
| `gravity` | `{0, 0, 0}` | |

### collision_mask tokens

`damageable` (= `player_side | hostile_side | neutral`), `player_side`,
`hostile_side`, `neutral`, `actor`, `limb`, `terrain`, `obstacle`, `prop`,
`projectile`, `none`. Combine with `|`.

`limb` is opt-in per weapon: without it, a shot passes through a creature's legs
and only its body can be hit.

### type: area_effect

Replaces `speed` with a damage-over-time block.

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
area effect spawns with zero velocity, always ends on its lifetime, and takes
its damage from `damage_behavior`, so none of the three can mean anything.

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
2. `action_templates/<name>_fire.yaml` — rate of fire
3. `action_templates/<name>_reload.yaml` — reload time
4. `weapon_templates/<name>.yaml` — magazine, range, and the three references
5. Add the weapon id to `weapon_slots` in an `entity_templates/` loadout
6. Rebuild the catalog bundle and ship the same bundle to client and server

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

The catalog hash changes whenever any of this changes, so the client and the
server must load the **same** bundle or the handshake fails.
