# Native Projectile Prediction ABI v5

## Summary

- Native kernel ABI v5 exposes stable render identity and generic client action
  correlation for projectile prediction.
- This document covers native C++ kernel/protocol/snapshot behavior only. Unity
  C# package mirrors are intentionally deferred to a follow-up task.
- Presentation consumers should use `RenderEntityState::entity_id` as the stable
  object key and treat `net_id` as the optional server-authoritative identity.

## Native ABI v5 Changes

- `KERNEL_ABI_VERSION` is `5`.
- `PlayerInput` carries `input_seq`, `client_action_time_us`, and
  `client_action_id`; `client_tick` is no longer part of the public ABI.
- `RenderEntityState` includes `uint64_t entity_id` before `net_id`.
- `EntitySnapshot` and projectile state use `client_action_id` for
  predicted-action reconciliation metadata.
- Input packets encode `client_action_time_us` and `client_action_id`.
- Snapshot packets encode projectile `client_action_id` but never encode
  `entity_id`, which is client-local.
- `InputButton_Dodge` and `InputButton_Parry` are additive ABI v5 button bits
  for rollback-eligible defensive input.

## Runtime Behavior

- `client_action_time_us == 0` means the kernel fills an estimated server
  timeline action time before sending input.
- Hitscan rewind converts `client_action_time_us` to a history tick and clamps
  compensation to 100ms.
- Local client projectile prediction creates a provisional projectile render
  state immediately for grenade fire when `client_action_id != 0`.
- Authoritative projectile snapshots bind back to predicted projectiles by
  `owner_peer + client_action_id`; the render-facing `entity_id` stays stable
  while `net_id` becomes the server id.
- If the server acknowledges an input sequence without producing a matching
  authoritative projectile, the provisional projectile render state is removed.
- Server-originated projectile/explosion damage against players enters an
  internal pending damage queue instead of applying immediately.
- Pending damage uses a 100ms grace window; Dodge overlapping the hit time
  cancels damage, and Parry overlapping the hit time reduces final damage by
  50%. Existing `DamageApplied` events are emitted only when damage confirms.

## Test Coverage

- Native ABI smoke tests verify ABI version 5 and public struct sizes/layout.
- Protocol tests verify input packet roundtrip for `client_action_time_us` and
  `client_action_id`, plus projectile snapshot `client_action_id`.
- Kernel listen-server tests verify immediate predicted projectile render
  states, predicted-to-authoritative `entity_id` stability, duplicate
  suppression, and rejected prediction cleanup.
- Damage pipeline tests cover grace-window confirmation, Dodge cancel, Parry
  reduction, non-eligible input rejection, and server-owned projectile pending
  damage.

## Assumptions

- `entity_id` is client-local and stable only within one kernel instance.
- `net_id` remains the server-authoritative entity id.
- `client_action_id == 0` means no predicted/correlatable action.
- Full time-sync sampling, projectile historical replay, sticky projectile
  authority, client damage presentation timing, public defensive feedback
  events, and Unity C# mirror updates remain separate follow-up work.
