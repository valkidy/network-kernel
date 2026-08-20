# Locomotion gait tuning

Why the procedural legged gait is tuned the way it is, kept here rather than in
one entity template so that every rig can point at it.

This was originally written inside `monster_sim_actor.yaml`, against the
`simplified_monster_sim_v4` rig. That rig and that template have been removed as
deprecated data; the reasoning below survives them because none of it is a
property of that particular skeleton. Where a measurement *is* specific to that
rig it is labelled as such and kept only as the evidence behind a rule — do not
re-derive today's numbers from it.

## Gait timing is set by move_speed, not by leg length

The Unity prototype (LocomotionTest) scales its gait off leg length alone
(`stepThreshold = legLength * 0.30`, `stepHeight * 0.22`) because it also scales
`moveSpeed` the same way (`LeggedLocomotion.cs:108,109,163`: `moveSpeed *
legLength`). Gameplay here does not: speed is combat data, fixed at 2.5 m/s,
while a rig may measure 18-24 m per leg.

Copying the rig-derived threshold into a fixed-speed game is therefore wrong.
Measured on the 18.24 m-legged monster rig, a rig-derived 5.47 m threshold gave a
2.2 s stance against a 0.2 s swing — an 8% swing duty factor where the prototype
runs 42%, with the foot travelling 11x the body's speed instead of 1.4x. That is
the "stand still, then flick a leg 5.5 m" look, and it is a gait-tuning artefact,
not a netcode one.

The invariant worth porting is the prototype's *timing*, which is leg-length
independent by construction:

    stance = step_threshold / move_speed = 0.30L / 1.2L = 0.25 s

At 2.5 m/s that is 0.625 m, with `step_height` keeping the prototype's
height/stride ratio (0.22 / 0.30 = 0.73).

## Threshold and duration must always move together

Timings sized purely by the clock read as mincing on a long-legged rig, so they
are scaled up together — 3x on the monster rig. Raising the threshold *alone*
makes the foot cover three times the ground in the same swing, and the leg-flick
comes straight back:

| threshold | duration | stride  | duty | peak foot speed / body speed |
|-----------|----------|---------|------|------------------------------|
| 0.65      | 6        | 1.17 m  | 41%  | 3.7x                         |
| 1.95      | 6        | 2.50 m  | 21%  | 10.0x                        |
| 2.60      | 6        | 3.17 m  | 15%  | 13.0x                        |
| 1.95      | 18       | 3.50 m  | 46%  | 3.8x  ← authored             |
| 2.60      | 24       | 4.67 m  | 46%  | 3.8x                         |

Stride runs about 1.8x the threshold: a step triggers once drift exceeds it, and
the landing target is then aimed ahead by the body's travel over the swing.

`1.95 / 18` is carried unchanged onto the current rigs. It is sized to the
2.5 m/s gameplay speed they all share, **not** to their leg lengths, so
rescaling it to a longer or shorter limb would stretch the stance and bring back
the low-duty flick this tuning removed.

## Reach is spent by terrain relief, not by stride

A straight-strut bind pose spends nearly all of its own reach standing still. On
the monster rig, hips 13.76 m up and 12.5 m out over the feet on 17.9-18.7 m of
bone left a foothold only 0.10-0.47 m below the body plane before the IK clamped:

| leg        | bones | rest% | max dip | max drift |
|------------|-------|-------|---------|-----------|
| FrontLeft  | 18.66 | 99.5% | 0.10 m  | 0.11 m    |
| FrontRight | 18.47 | 98.8% | 0.27 m  | 0.31 m    |
| RearLeft   | 17.92 | 98.0% | 0.47 m  | 0.52 m    |
| RearRight  | 17.92 | 97.9% | 0.47 m  | 0.52 m    |

`undulating.obj` varies by 1.47 m (median) to 2.61 m across a 16.1 m stance —
3x to 26x that dip budget. Sampling 2500 random positions and headings, the share
of poses with at least one clamped leg (and the mean number clamped, of 4):

| body seated | threshold 1.95 | threshold 0 (standing still) |
|-------------|----------------|------------------------------|
| bind        | 84%  1.89      | 83%  1.87                    |
| -2 m        | 48%  0.85      | 44%  0.76                    |
| -4 m        | 20%  0.30      | 18%  0.28                    |
| -5 m        | 10%  0.15      | 10%  0.14                    |

The two columns are the same to within noise: **the gait contributes almost
nothing and only the seat height moves the number.** The model is pessimistic in
absolute terms — it ignores the 5-candidate foothold fan and the reachable-landing
fallback — so read the columns against each other, not as absolute rates.

So the fix buys reach rather than shortening the stride, through
`body.stance_crouch_meters`, which bends the knees. Measured on a "+X" capture,
300 samples, before and after seating the body on its own feet at a 4 m crouch:

|                                | before  | after   |
|--------------------------------|---------|---------|
| worst leg clamped              | 40.7%   | 2.3%    |
| leg extension, p10-p90         | 83-100% | 78-91%  |
| foot above the ground, median  | +0.22 m | +0.00 m |
| ...p90                         | +2.08 m | +0.35 m |
| leg-samples over 1 m off       | 34%     | 3%      |
| travel in 10 s                 | 23.95 m | 23.96 m |

What remains is a single leg on the steepest ground. Closing it needs a deeper
crouch, a stance-width parameter (0.8x stance modelled at 8%, but no such
parameter exists), or a rig authored with the knees already bent — which is what
`simplified_quadruped` and its siblings do, and why their crouch is 0.40 m rather
than 4.0 m.

Reproduce the per-leg table for any rig with:

```bash
bazel run //engine/src/tests/kernel_tests:locomotion_reach_report -- \
  $PWD/bazel-bin/game_server/skeleton_assets/generated/<rig>.ozz
```

## Turning dominates stance drift

At `max_yaw_degrees_per_second: 45` across a 16.08 m stance radius the stance
sweeps at 12.6 m/s — five times the walk — which no threshold in this range keeps
up with. Worst stance error over a 20 s run on flat ground:

| threshold | walk duty | walk err | patrol err | sustained-spin err |
|-----------|-----------|----------|------------|--------------------|
| 5.50      | 7%        | 5.92 m   | 8.26 m     | 7.94 m             |
| 2.00      | 20%       | 2.42 m   | 10.29 m    | 32.26 m            |
| 1.20      | 32%       | 1.67 m   | 10.65 m    | 32.26 m            |
| 0.65      | 41%       | 1.08 m   | 10.19 m    | 32.26 m            |

"patrol" slews the heading 30° once a second the way the sentry AI does; its
error is set by the 45°/s slew, not by the threshold, so shrinking the stride
costs ~2 m of turn lag to gain 6x the walk duty. "spin" holds max yaw rate
indefinitely and saturates at twice the stance radius — feet diametrically
opposite their home, fully decoupled. If sustained spinning ever matters the knob
is `max_yaw_degrees_per_second`: the legs keep up to about 8-10°/s at this
threshold, far slower than gameplay currently wants.

## Body follow and slope alignment

`body.follow_speed > 0` hands body height to the legs: the body rides the average
of its own footholds instead of the terrain the movement capsule happens to be
standing on. 10 is prototype parity (`LeggedLocomotion.cs` authors
`bodyFollowSpeed = 10` under the same `k = 1 - exp(-speed * dt)` smoothing).

This is not polish. A movement capsule wide enough to bridge a rig's stance
resolves a different height from the one the feet imply — 0.22 m (median) to
0.76 m (p90) on the monster rig, plus a further 0.49 m of standing bias — and all
of it lands in the knees, pumping leg extension between 75% and 95% every second
on a rig whose rest is 85%. Riding the feet removes the mismatch by construction.
Locomotion smooths from the height it itself last applied rather than from the
transform, so the character controller's answer is not readmitted every tick; the
controller still owns x/z, and owns y whenever no foot is planted.

`slope_alignment` stays 0 on every rig. Tilt changes `applied_root_rotation`,
which is where the feet are placed, and a follower holds an identity tilt because
it never samples ground normals — so enabling it on the authority alone
desynchronises replicated legs. Height has no such problem: it arrives inside the
replicated transform.

## Foothold ray coverage

Ray coverage is scaled to the rig, unlike gait timing. The start height must
clear terrain rising ahead of a foot's home stance and the distance must reach
any drop below it; the prototype uses `rayUp = legLength * 3` and `rayLength =
legLength * 30`. Too small a start height makes a foot's home ray begin below
rising terrain and miss, stalling that leg's steps. The authored 30 / 90 pair was
sized against an 18.24 m-legged rig on the undulating terrain, and the live rigs
scale it by their own leg length from there.
