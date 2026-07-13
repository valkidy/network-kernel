# Action And Animation Sync Policy

## Authority Boundary

Action templates contain gameplay policy only: trigger mode, cancellation,
ammo cost, commit cadence, recovery, and input timeout. They are loaded from
the server gameplay catalog before the normal gameplay handshake and are part
of the catalog hash.

The native kernel does not bind actions to Animator state ids, AnimationClip
ids, VFX assets, Unity-specific fields, or Unity Engine dependencies. If
server-selectable presentation is needed later, the preferred extension is an
abstract `presentation_profile_id` that each client maps to local assets. No
such field exists in the current ABI.

## Synchronized Runtime Facts

Actor snapshots always carry normalized aim. When an action is present, the
20-byte action block carries:

- action template id
- action instance id
- action start tick
- commit count
- action phase plus reserved bytes

`next_commit_tick` and `last_commit_tick` are server runtime scheduling state,
not wire fields. Clients derive the next commit from template cadence, start
tick, and commit count. Action phase, ids, ticks, commit count, and aim are
discrete values from the newer snapshot; they are not blended during transform
interpolation.

## Presentation Derivation

Visual flags are composable presentation hints, never gameplay authority.
`KERNEL_VISUAL_FLAG_FIRING` is not an independent snapshot fact: render and
server queries derive it from `action.phase == KernelActionPhase_Active`.
`animation_state` remains ABI-compatible legacy data and must not drive Idle,
Moving, or Firing.

| Presentation state | Kernel API condition |
|---|---|
| Moving | `visual_flags & KERNEL_VISUAL_FLAG_MOVING` |
| Reloading | `visual_flags & KERNEL_VISUAL_FLAG_RELOADING` |
| Dead | `visual_flags & KERNEL_VISUAL_FLAG_DEAD` |
| Aiming | `visual_flags & KERNEL_VISUAL_FLAG_AIMING` |
| Firing | `action.phase == KernelActionPhase_Active` |
| Windup | `action.phase == KernelActionPhase_Windup` |
| Recovery | `action.phase == KernelActionPhase_Recovery` |
| Idle | No Moving, Reloading, Dead, or Firing state and phase is None |

The client presentation registry maps `(action_template_id, phase,
visual_flags)` to local animation assets and owns animation-layer priority.

## Prediction And Reconciliation

The owner predicts aim, phase, and commits immediately, then reconciles from
the authoritative action state and replays unacknowledged inputs. Remote
players and AI use the newer snapshot's discrete action state and delayed
transform interpolation. A matching instance id permits replay from the
authoritative baseline; a mismatching id replaces the local action.

Late join reconstructs continuous presentation from the current template id,
phase, start tick, and commit count. A missing or stale gameplay catalog blocks
the normal gameplay handshake rather than guessing action policy. Phase None
or an absent action block clears remote action presentation.
