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
`1_player.yaml`'s `weapon_slots`), or nothing will ever hold it. A loadout holds
at most 4 weapons.

Ids must be unique within each directory. Weapon ids are 0-255.

Name the files `$id_projectile_$name.yaml` and `$id_weapon_$key.yaml`, as in
`3_projectile_rocket.yaml` and `3_weapon_rocket.yaml`. A weapon's YAML `name`
is its display string and does not have to match the file. Action template
files keep their plain names. The full rule is in
[Template File Naming](DATA_DRIVEN_TEMPLATE_DESIGN.md#template-file-naming).

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
| `targeted_strike` | `max_range` |

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

### weapon_type: targeted_strike

Lands its projectile template on a point instead of firing it from the muzzle.
The server resolves the point from the aim, so the client sends nothing new:

1. A ray along the aim, up to `max_range`, stops at the first actor, terrain or
   static obstacle. An actor is tested where the shooter saw it (lag
   compensated), and a wall still hides whoever is behind it.
2. From there, a ray straight down onto terrain or a static obstacle. It passes
   through actors, so aiming at someone lands at their feet. Aiming at a wall
   lands at its foot on your side.

If either ray finds nothing, the shot is **refused**. This covers aiming at the
sky, at ground beyond `max_range`, or at something with no ground under it. A
refused shot costs no ammunition and starts no cooldown. The client should show
the reticle as invalid in the same cases, so it runs the same two rays from the
same point: the actor's position plus 1 m up.

```yaml
id: 13
name: Meteor Staff
weapon_type: targeted_strike
magazine_size: 3
max_range: 35.0
fire_action_template: meteor_staff_cast
reload_action_template: meteor_staff_reload
projectile_template: meteor_marker
```

The projectile template must be `server_snapshot_only` and must not be a beam.
The strike is never predicted: it appears where only the server decides.

Usually what lands is a **marker** (below) that expires into the real thing, so
the delay doubles as a telegraph every client sees. A `launch: descent`
template may also be landed directly.

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

**Known gap**: a projectile that names *no* side (`spammer`, which is
`terrain | static_obstacle`) has an empty gameplay-category mask, so it passes
through every side-layered collider — actors and deployable cover alike. See
`collider_templates/13_collider_ice_block_hitbox.yaml`, which hit the same wall from the
target's end.

### A marker: speed 0

A standard projectile with `speed: 0` is a marker. It holds its place for
`lifetime_ticks` and then fires `on_expired` there, facing the direction it was
spawned with. It is accepted only when it can do nothing else:
`collision_mask: none`, linear motion, and no gravity. A template that simply
forgot its speed still fails to load.

```yaml
id: 18
name: meteor_marker
type: standard
collider_template: projectile_sphere
damage: 0
damage_shape: none
speed: 0.0
collision_mask: none
sync_mode: server_snapshot_only
lifetime_ticks: 20          # the delay
triggers:
  on_expired:
    action_graph: action_spawn_projectile_at_expired
    parameters:
      template: meteor_body
      position: event.position
      direction: event.direction
```

This is the delay mechanism. An area effect cannot do the same job: it expires
without firing `on_expired`, so binding `on_expired` on one is a load error.

### launch: descent

The spawn point becomes a landing target. The projectile starts `height` above
it, from a direction and at an elevation (within `elevation_degrees`) that are
both picked by a seed, and lands on it in a straight line after `fall_ticks`.
The target is first dropped onto the terrain or static obstacle under it.

```yaml
id: 19
name: meteor_body
type: standard
collider_template: projectile_sphere
damage: 0
damage_shape: none
collision_mask: terrain | static_obstacle
lifetime_ticks: 18          # must exceed fall_ticks
launch:
  type: descent
  elevation_degrees: [75, 85]   # or one number; 0 < min <= max <= 90
  height: 40.0
  fall_ticks: 15
triggers:
  on_projectile_impact:
    action_graph: action_spawn_projectile_at_impact
    parameters: { template: meteor_blast, position: event.position, direction: event.direction }
```

The speed is derived, so do not author `speed`. The motion is always linear
with no gravity, and `sync_mode` defaults to (and only accepts)
`server_snapshot_only`. `lifetime_ticks` must exceed `fall_ticks`: the tick
after arrival is the one whose sweep meets the ground.

The direction and the elevation are seeded from the caster, the cast, the
template and the spawn's launch salt, and from nothing else: not the direction
the projectile was spawned facing. One cast therefore always falls the same
way, and anyone who knows the target and those four values can derive the
fall. That is what lets a client draw it without it being sent. A launch template cannot be fired from
the muzzle of an ordinary weapon, because it would fall onto its shooter. Land
it with `targeted_strike` or an action graph.

Put the damage on the impact effect, not on the falling body. Also keep actors
out of the body's `collision_mask`: then its path depends only on the static
world, which is what lets clients derive it later.

### replication: derived

Everything below a targeted strike's marker can be left off the wire.
`replication: derived` on a template means the server never sends it: no
spawn, no snapshot record and no despawn for it. The server still simulates it
and deals its damage. Each client re-runs the chain from the marker it was
sent, using the same code, the same catalog and the same static world, and
draws the result.

```yaml
replication: derived        # default: replicated
```

A derived template is accepted only where a client can reproduce it:

- It must descend from a stationary marker that a `targeted_strike` weapon
  lands. That marker is the chain's root and stays replicated.
- A derived template may spawn only derived templates. An area effect's
  `on_projectile_impact` may not spawn one, because it fires once per target
  and only the server knows the targets.
- A standard projectile may collide only with `terrain | static_obstacle`. It
  must be `server_snapshot_only`, and it cannot be a beam or homing.
- No weapon fires one directly, and no entity or item trigger spawns one.

The server keeps the root (hidden) after it expires, until the whole chain
has run out, so a player who arrives mid-storm is still sent the root and can
catch up. If the root's chain fails to start on the server, the root is
removed at once and clients draw nothing under it.

A stationary marker is also never written into snapshots; the client draws it
from its spawn record. The Meteor Storm Staff costs clients one spawn and one
despawn per cast, instead of about 46 of each plus snapshot records every
interval.

### type: area_effect

Adds a damage-over-time block. `speed` is optional here and defaults to `0`,
which is a field that sits where it was spawned — every blast wants that.

`damage_interval_ticks` is the field's own cadence, not a per-target one: the
effect looks for targets once every interval and hits everything it finds. A
target that walks in between two of those evaluations is not noticed until the
next one, so the interval is also the worst-case delay before someone entering
the field is first hit. Keep it short for a field people walk into; a blast
whose interval is at least its `lifetime_ticks` is evaluated exactly once, at
the moment it appears.

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

`motion_collision_mask` is what stops it. It is a separate mask from
`collision_mask` on purpose: `collision_mask` says *who the field affects*,
this one says *what stops the field*, and only static-world bits (`terrain`,
`static_obstacle`) are accepted. Absent, the default, means nothing stops it —
the field crosses walls, and **no swept query is run for it at all**, so a
template that does not need this does not pay for it.

```yaml
type: area_effect
speed: 6.0
motion_collision_mask: terrain | static_obstacle
```

On contact the field **stops at the contact point and keeps working from
there** for the rest of its lifetime; it is not destroyed. Its expiry still
belongs to `lifetime_ticks`. Authoring `motion_collision_mask` on a template
with no `speed`, on a non-area-effect type, or with actor/prop bits in it is
rejected at load.

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

1. `projectile_templates/<id>_projectile_<name>_shot.yaml` — damage and
   collision_mask
2. **Name the side in `collision_mask`** — `hostile_side` for a player's weapon,
   `player_side` for an enemy's. Leaving it out means it hits both.
3. `action_templates/<name>_fire.yaml` — rate of fire (shareable with another
   weapon)
4. `action_templates/<name>_reload.yaml` — reload time (shareable)
5. `weapon_templates/<id>_weapon_<name>.yaml` — magazine, range, and the three
   references; YAML `name` is the display string
6. Add the weapon id to `weapon_slots` in an `entity_templates/` loadout
7. Rebuild the catalog bundle and ship the same bundle to client and server

For a targeted strike, steps 1 and 3 become a marker or descent template and
a cast action. See `meteor_staff`, `meteor_storm_staff` and `sky_laser`.

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
| `standard projectile speed must be positive; speed 0 is only accepted for a marker...` | A marker needs `collision_mask: none`, linear motion, no gravity |
| `on_expired is not supported on area_effect projectiles` | Put the delay on a marker |
| `launch needs lifetime_ticks greater than fall_ticks` | Give the fall one more tick to meet the ground |
| `a projectile with a launch rule cannot be fired from the muzzle` | Use `targeted_strike` or an action graph |
| `targeted_strike weapon requires max_range` | It is the farthest aimable point |
| `targeted_strike projectile_template needs sync_mode server_snapshot_only` | Strikes are never predicted |

Two failures that load cleanly and only show up in play:

| Symptom | Cause |
|---|---|
| The weapon damages its own side | `collision_mask` names no side, or the wrong one. See "Name a side" above |
| A shot passes through an actor that should be hittable | The actor's hit collider `layer:` is not a side the mask names — or the mask names no side at all |

The catalog hash changes whenever any of this changes, so the client and the
server must load the **same** bundle or the handshake fails.
