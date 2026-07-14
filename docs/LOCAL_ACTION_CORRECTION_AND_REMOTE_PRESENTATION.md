# Local Action Correction and Remote Presentation Specification

## Scope and status

The C++ kernel owns the generalized action synchronization path. `PlayerInput`
is the native input envelope, and `ActionIntent` / `ActionInput` are its current
generalized-action fields. AI producers may submit the same generalized action
contract internally; `PlayerInput` is not an AI output format requirement.

ABI 41 completes the C++ owner-correction and remote-presentation transport
closure described below. Unity consumers, Animator/VFX integration,
Skill/Casting production producers, presentation profiles, and Climb gameplay
remain deferred and are not claimed complete by this specification.

## Owner correction

- Accepted results confirm only the unconfirmed commit range and never rebuild
  presentation already predicted by the owner.
- Finite actions remove bookkeeping after their last commit is confirmed. Hold
  actions retain it until a snapshot shows that the action ended and there is no
  unconfirmed commit.
- Corrected and Rejected results stop the predicted action, remove unbound
  projectiles/effects created by that action instance, and restore the captured
  ammo/effect checkpoint.
- A result at or before the latest snapshot updates dedup/accounting only; it
  does not replace snapshot authority. A newer terminal result remains pending
  until a snapshot reaches its authoritative tick, at which point snapshot ammo,
  phase/recovery, and effect ownership are authoritative.
- A result timeout stops persistent prediction, removes unbound objects,
  restores the checkpoint, and increments timeout stats. It does not synthesize
  a `KernelLocalActionResult`.
- Disconnect/reconnect, controlled-actor replacement, gameplay-catalog
  replacement, and runtime reset share the same client action-sync clear path.

## Remote presentation

- The client rejects duplicate/reordered batches and stale records on receipt.
  Poll/release rechecks the 250 ms default expiry against render time, so a
  render-clock jump cannot release expired presentation.
- The server excludes the owner and filters relevance before applying traffic
  policy. Selection priority is Death, Hit, Reload/Casting, then Fire.
- Per-peer and server token buckets use simulation time. Defaults are 8 KiB/s
  per client and 256 KiB/s server aggregate.
- The 1,200 B action packet limit derives maxima of 97 owner-result records
  (`36 + 12N`) or 58 remote records (`36 + 20N`). Owner results split across
  reliable packets and bypass traffic drops. Remote overflow drops the lowest
  priority records immediately and is never deferred for retransmission.

## Network statistics

`KernelNetworkStatsMode_Default` resolves to Basic. Off disables counters and
timing. Basic records channel bytes, packets, action outcome, timeout,
drop/dedup, relevance, and batching counters. Detailed adds packet
serialization/deserialization cost and owner-result latency samples. Stats mode
does not change encoded wire bytes.

## Verification boundary

Protocol, ABI, client correction, remote expiry/relevance/priority/budget,
listen-server, and deterministic traffic tests cover the C++ behavior. The
traffic matrix and its application-message-only accounting are recorded in
`docs/LOCAL_ACTION_NETWORK_TRAFFIC_REPORT.md`.
