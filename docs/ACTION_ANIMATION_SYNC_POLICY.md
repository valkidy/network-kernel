# Local Action Correction And Remote Presentation Specification

Status: Proposed
Scope: actor action state, local prediction correction, remote action presentation,
Unity-facing render consumption, and runtime traffic policy

## 1. Objective

This specification separates action synchronization into two contracts:

1. The owner client must eventually converge to the server-authoritative result
   for every locally predicted action.
2. A client viewing a remote actor receives best-effort action presentation.
   Missing a remote one-shot animation is acceptable and must not affect gameplay
   correctness, entity lifecycle, or future snapshot reconciliation.

The intended player experience is:

```text
Local player:
    immediate prediction + reliable authoritative correction

Remote player:
    delayed authoritative state + best-effort one-shot presentation
```

## 2. Non-Goals

This specification does not:

- bind native actions to Unity Animator state ids or AnimationClip ids;
- make presentation events a gameplay authority source;
- guarantee delivery of remote muzzle flashes, cast gestures, or other one-shot
  cosmetic effects;
- require gameplay actions to remain active for a snapshot interval;
- change gameplay cooldown, recovery, ammo, damage, or cancellation policy to
  satisfy presentation timing;
- replace snapshots for continuous action state or reliable lifecycle messages.

## 3. Terminology

| Term | Meaning |
|---|---|
| Owner | The client controlling the actor that originated an action input. |
| Observer | A client rendering an actor it does not control. |
| Action instance | One logical action attempt identified by a non-zero `action_instance_id`. |
| Commit | A server-authoritative gameplay effect produced by an action instance. |
| Owner correction | Accepted, rejected, or corrected action result sent only to the owner. |
| Remote presentation event | Best-effort cosmetic hint sent to relevant observers. |
| Continuous state | State that remains true over time, such as Reloading, Climbing, or Dead. |
| One-shot presentation | An effect that should normally play once, such as FireCommit or HitReaction. |

Normative terms `MUST`, `MUST NOT`, `SHOULD`, and `MAY` describe implementation
requirements.

## 4. Authority Boundary

The server remains authoritative for:

- whether an action instance was accepted;
- authoritative commit count and commit tick;
- ammo, cooldown, damage, projectile spawn, death, and other gameplay effects;
- continuous action phase and entity lifecycle.

The owner client MAY predict action phase, commits, projectiles, and presentation
immediately. Prediction does not make the client authoritative.

Action templates contain gameplay policy only: trigger mode, cancellation, ammo
cost, commit cadence, recovery, and input timeout. The native kernel MUST NOT
contain Animator state ids, AnimationClip ids, VFX asset ids, or Unity-specific
dependencies. If server-selectable presentation is needed, an abstract
`presentation_profile_id` MAY be added to catalog data and mapped to local assets
by each client.

## 5. Required Synchronization Channels

### 5.1 Authoritative snapshots

Snapshots remain the source for current continuous state. An actor action block
contains the existing 20-byte timeline when an action is present:

- action template id;
- action instance id;
- action start tick;
- commit count;
- action phase plus reserved bytes.

Snapshots are used for:

- local reconciliation baseline;
- remote delayed interpolation;
- Reloading, Climbing, Dead, channeling, and other continuous presentation;
- late join and recovery after missing remote presentation events.

`next_commit_tick` and `last_commit_tick` remain runtime scheduling fields. A
client derives cadence from the action template, start tick, and commit count.

### 5.2 Owner action result stream

Every locally predicted action instance that the server processes MUST produce
an owner-only authoritative result. The result stream MUST be reliable and
ordered or provide equivalent eventual-delivery and deduplication guarantees.

The logical result record is:

```cpp
struct LocalActionResult {
    uint32_t action_instance_id;
    uint16_t confirmed_commit_count;
    uint8_t result;
    uint8_t reason;
    uint32_t authoritative_tick;
};
```

The logical encoded size is 12 bytes before batch and packet framing.

`result` supports at least:

| Result | Meaning |
|---|---|
| Accepted | Predicted instance and authoritative result agree. |
| Corrected | The instance exists, but phase, commit count, tick, or gameplay effect differs. |
| Rejected | The server did not authorize the predicted action. |

`reason` is diagnostic and policy-specific. It MUST NOT be interpreted as a
presentation asset id.

The server sends this record only to the actor owner. Observers MUST NOT receive
owner correction records.

### 5.3 Remote presentation stream

One-shot remote presentation uses an unreliable sequenced batch sent only to
relevant observers. Delivery is best-effort. Events are not retransmitted.

The logical event record is:

```cpp
struct RemoteActionPresentationEvent {
    uint32_t actor_net_id;
    uint32_t action_template_id;
    uint32_t action_instance_id;
    uint16_t first_commit_index;
    uint16_t commit_count;
    uint8_t event_type;
    uint8_t flags;
    uint16_t server_tick_delta;
};
```

The logical encoded size is 20 bytes before batch and packet framing.

The minimum event types are:

- FireCommit;
- AbilityCommit;
- ReloadCommit;
- HitReaction;
- DeathTrigger.

Additional event types MAY be added without making their delivery authoritative.
Continuous state such as Reloading, Climbing, or Dead MUST still be recoverable
from snapshot or lifecycle state.

## 6. Local Owner Contract

### 6.1 Input identity

Every predicted action attempt MUST use a non-zero `client_action_id`. The owner
MUST generate ids monotonically within a session, allowing unsigned wrap only
after outstanding instances can no longer collide. The server uses this value as
the authoritative `action_instance_id` correlation token.

Preparing client input MUST NOT silently replace a non-zero id. A zero id is not
valid for a predicted action that requires correction.

### 6.2 Prediction flow

```text
1. Owner allocates action_instance_id.
2. Owner applies local prediction immediately.
3. Owner plays the predicted one-shot presentation immediately.
4. Owner sends input containing the same action_instance_id.
5. Server processes the instance idempotently.
6. Server sends LocalActionResult only to the owner.
7. Owner reconciles gameplay state and marks the prediction confirmed,
   corrected, or rejected.
```

Repeated delivery or repeated input for the same action instance MUST NOT
produce duplicate authoritative commits.

### 6.3 Accepted result

On `Accepted`, the owner:

- MUST mark the matching predicted commit range confirmed;
- MUST NOT replay an already predicted one-shot animation;
- MUST discard acknowledged prediction bookkeeping;
- MAY correct small timing differences without restarting the clip.

### 6.4 Corrected result

On `Corrected`, the owner:

- MUST replace predicted gameplay state with the authoritative result;
- MUST reconcile ammo, cooldown, projectile/effect ownership, and commit count;
- SHOULD preserve an already-played one-shot clip rather than visibly rewind it;
- MUST stop or retime continuing loops when the authoritative state no longer
  permits them.

### 6.5 Rejected result

On `Rejected`, the owner:

- MUST roll back predicted gameplay effects;
- MUST remove or reject-bind predicted projectiles and persistent effects;
- MUST restore authoritative ammo and cooldown state;
- MUST stop continuing action loops;
- SHOULD NOT attempt to reverse a muzzle flash or other completed one-shot
  cosmetic effect.

### 6.6 Processed input is not action acceptance

`last_processed_input_seq` only proves that the server processed an input. It
does not prove that ammo was available, a commit occurred, or the action was
accepted. The owner MUST use `LocalActionResult` or equivalent explicit result
data for action confirmation.

### 6.7 Timeout and disconnect

An owner prediction that receives neither an authoritative result nor a newer
authoritative baseline within the configured timeout MUST stop producing new
persistent gameplay effects. The client MAY keep already-completed cosmetic
one-shots visible.

Disconnect, reconnect, catalog replacement, and controlled-actor replacement
MUST clear outstanding action instances and result deduplication state.

## 7. Remote Observer Contract

Remote observers render actors on the delayed snapshot timeline. They do not
predict remote gameplay actions.

For remote presentation events, an observer:

- MUST deduplicate by `(actor_net_id, action_instance_id, commit_index,
  event_type)`;
- MUST reject older batch sequences;
- MUST discard events older than the presentation expiry window;
- MAY skip missing sequence ranges without requesting retransmission;
- MUST NOT change ammo, HP, projectile ownership, lifecycle, or any gameplay
  state from a presentation event;
- MUST NOT treat a missing event as cancellation, rejection, death, or despawn;
- SHOULD use the next snapshot to recover continuous action phase.

Remote one-shot loss is explicitly acceptable. A missed remote muzzle flash or
gesture MUST NOT cause state divergence. Death state and entity destruction
remain authoritative through snapshot and lifecycle paths even if DeathTrigger
presentation is lost.

## 8. Presentation Derivation

Visual flags remain composable presentation hints and never gameplay authority.
`animation_state` remains ABI-compatible legacy data and MUST NOT drive Idle,
Moving, or Firing.

| Presentation state | Kernel condition |
|---|---|
| Moving | `visual_flags & KERNEL_VISUAL_FLAG_MOVING` |
| Reloading | `visual_flags & KERNEL_VISUAL_FLAG_RELOADING` or corresponding continuous action |
| Climbing | Corresponding continuous action or future climbing visual flag |
| Dead | `visual_flags & KERNEL_VISUAL_FLAG_DEAD` or authoritative lifecycle state |
| Aiming | `visual_flags & KERNEL_VISUAL_FLAG_AIMING` |
| Firing state | `action.phase == KernelActionPhase_Active` |
| Fire one-shot | Predicted local commit or remote FireCommit presentation event |
| Windup | `action.phase == KernelActionPhase_Windup` |
| Recovery | `action.phase == KernelActionPhase_Recovery` |
| Idle | No higher-priority continuous state and action phase is None |

The client presentation registry maps action template, phase, event type, actor
presentation profile, and visual flags to local animation assets. Animation
layer priority remains client presentation policy. A recommended priority is:

```text
Death/full-body override
Climb/full-body locomotion
Reload/equipment
Fire/upper-body
Aim and locomotion base layers
```

## 9. Delivery And Batching Policy

| Stream | Recipients | Delivery | Retransmit | Purpose |
|---|---|---|---|---|
| Snapshot | Relevant clients | Existing snapshot policy | No | Current authoritative state |
| LocalActionResultBatch | Owner only | Reliable ordered | Yes | Prediction correctness |
| RemoteActionPresentationBatch | Relevant observers only | Unreliable sequenced | No | Cosmetic one-shot playback |
| Lifecycle events | Relevant clients | Existing reliable policy | Yes | Spawn/despawn/destroy truth |

Events MUST be batched. A one-event-per-packet implementation is not acceptable
for sustained automatic fire. A remote batch contains a batch server tick,
record count, and compact records. Multiple consecutive commits from the same
action MAY be represented as a commit range using `first_commit_index` and
`commit_count`.

The packet header sequence MAY sequence batches. Per-event sequence fields are
not required when the actor/action/commit tuple is sufficient for deduplication.

## 10. Traffic Budget

Current encoded sizes used by this policy are:

| Item | Encoded bytes |
|---|---:|
| Packet header | 28 B |
| Snapshot base | 60 B total including packet header |
| Conditional actor action timeline | 20 B per active actor |
| Proposed local result record | 12 B before batch framing |
| Proposed remote presentation record | 20 B before batch framing |

At 10 commits per second, an owner receives approximately 120 B/s of result
records before amortized batch framing. Because correction is owner-only, server
correction egress grows with action rate and player count, not with the square of
player count.

For `P` players, action rate `A`, and result size `R`:

```text
owner correction server payload per second ~= P * A * R
```

Remote presentation still fans out to relevant observers:

```text
remote presentation payload per second
    ~= visible_actor_observer_pairs * A * remote_record_size
```

Therefore remote events MUST use relevance filtering, batching, commit-range
aggregation, and unreliable delivery. Traffic limits MAY drop remote
presentation records, starting with the oldest or lowest-priority cosmetic
records. Traffic limits MUST NOT drop owner correction results.

## 11. Snapshot Lifetime Policy

Gameplay actions are not required to satisfy:

```text
action visible ticks >= snapshot interval ticks
```

Zero-recovery one-shot actions remain valid. Their owner correctness comes from
`LocalActionResult`, and remote one-shot presentation comes from the best-effort
presentation stream.

Snapshots continue to carry an action timeline only while authoritative
continuous action state exists. Implementations MUST NOT increase
`recovery_ticks`, cooldown, or action-slot occupancy solely to expose a one-shot
commit to a snapshot.

## 12. ABI And Protocol Surface

The preferred additive public surface is:

```text
Kernel_PollLocalActionResults(...)
Kernel_PollRemoteActionPresentationEvents(...)
```

The result and presentation queues MUST be separate so callers cannot confuse
authoritative owner correction with remote cosmetic hints.

The implementation SHOULD add separate capability flags for local action
results and remote presentation events. New public structs/functions require an
ABI version update according to the existing ABI compatibility policy. New
message types or changed packet layouts require packet schema updates.

`Kernel_GetRenderStates` and `Kernel_GetRenderStatesAtTime` remain the continuous
render-state APIs. They do not need to return every completed one-shot commit.

## 13. Failure Handling

| Condition | Required behavior |
|---|---|
| Duplicate owner result | Apply once; do not replay presentation. |
| Owner result for unknown instance | Preserve authoritative bookkeeping; do not start a new animation. |
| Owner result older than current authoritative baseline | Do not rewind presentation; retain only required gameplay accounting. |
| Remote duplicate event | Drop. |
| Remote sequence gap | Accept the gap; do not request retransmission. |
| Remote stale event | Drop or fast-forward only if still inside expiry policy. |
| Missing catalog/template | Block normal gameplay handshake; do not guess presentation policy. |
| Snapshot disagrees with presentation event | Snapshot/lifecycle state wins. |
| Traffic budget exhausted | Preserve owner results; drop low-priority remote presentation first. |

## 14. Metrics

The implementation SHOULD expose at least:

- owner action results sent, accepted, corrected, rejected, duplicated, and
  timed out;
- owner result delivery latency;
- remote presentation records generated, sent, relevance-filtered, budget-
  dropped, stale-dropped, and duplicate-dropped;
- average and maximum result/presentation batch size;
- bytes per second split by owner correction and remote presentation;
- action-instance id collisions or zero-id prediction attempts.

## 15. Verification Requirements

### 15.1 Owner correctness tests

The test suite MUST verify:

1. Accepted local fire confirms without replaying its predicted one-shot.
2. Rejected fire rolls back ammo, cooldown, projectile/effect state, and loops.
3. Corrected commit count converges to the server result.
4. Duplicate input and duplicate result do not create duplicate commits or
   animations.
5. Delayed or reordered result delivery still converges.
6. A zero-offset, zero-recovery one-shot receives an authoritative owner result.
7. `last_processed_input_seq` alone does not confirm an action.
8. Disconnect/reconnect clears outstanding instances.

### 15.2 Remote best-effort tests

The test suite MUST verify:

1. A received remote FireCommit plays once.
2. A duplicate remote event does not replay.
3. A dropped remote event does not change gameplay or lifecycle state.
4. A sequence gap does not trigger retransmission or client failure.
5. A stale remote event is discarded.
6. Continuous Reloading, Climbing, and Dead state recovers from snapshots.
7. Snapshot/lifecycle truth overrides conflicting cosmetic presentation.

### 15.3 Traffic tests

The test suite MUST verify:

1. Owner correction is sent only to the owning peer.
2. Remote presentation is sent only to relevant observers.
3. Multiple commits are batched rather than sent as one packet each.
4. Automatic-fire commits can be encoded as commit ranges.
5. Remote presentation may be budget-dropped while owner correction remains
   deliverable.

## 16. Implementation Phases

### Phase 1: owner correctness

- Require non-zero action instance ids for predicted actions.
- Add idempotent server action-result generation.
- Add reliable owner-only result transport and native poll API.
- Reconcile accepted, corrected, and rejected results in the client kernel.
- Add owner correctness tests before remote presentation work.

### Phase 2: remote best-effort presentation

- Add compact unreliable sequenced presentation batches.
- Add relevance filtering, expiry, deduplication, and traffic priority.
- Add Unity/client consumption without treating events as authority.

### Phase 3: generalized actions

- Move Fire, Reload, Death, and Climb commit effects behind generic action
  executors.
- Add presentation profiles and action-layer policy without Unity asset ids in
  native data.
- Preserve snapshots as the source of continuous action state.

### Phase 4: measurement and tuning

- Measure packet rate, bytes per second, result latency, remote event drop rate,
  and batch occupancy.
- Tune batch cadence, relevance radius, expiry, and remote event priority from
  measured data.

## 17. Acceptance Criteria

The correction design is complete when all of the following are true:

- every processed local predicted action eventually receives an explicit owner
  result;
- accepted confirmation never double-plays a local one-shot;
- rejected/corrected local gameplay state converges to server authority;
- duplicate or reordered owner messages are idempotent;
- remote presentation loss is tolerated without gameplay divergence;
- remote continuous state remains recoverable from snapshots/lifecycle;
- zero-duration one-shot actions do not require artificial recovery ticks;
- owner correction is never relevance-filtered or traffic-budget dropped;
- remote events are relevance-filtered, batched, sequenced, expirable, and
  droppable;
- measured traffic remains within the configured per-client and server egress
  budgets.
