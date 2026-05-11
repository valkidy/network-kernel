# Projectile Sync Kernel / C# Package ABI Plan

## Summary

- Implement native projectile prediction reconciliation metadata in the
  `network-example` kernel, protocol, snapshot, and public C ABI.
- Native C++ ABI work is completed on `main` first, then merged back into
  `feat-unity-plugin` before updating the Unity C# package mirrors.
- The C# package uses `clientProjectileId` to match a locally predicted
  projectile to the server-authoritative projectile returned through render
  state metadata.

## Branch Flow

- Start from clean `feat-unity-plugin`.
- Switch to `main` for native C++ ABI v4 work.
- Commit native C++ and documentation changes on `main`.
- Switch back to `feat-unity-plugin`, merge `main`, and resolve conflicts
  without rewriting history.
- Update Unity package C# ABI mirrors and managed smoke tests on
  `feat-unity-plugin`.

## Native ABI v4 Changes

- `KERNEL_ABI_VERSION` is `4`.
- `PlayerInput` adds `client_projectile_id` after `client_tick`; `client_tick`
  remains the client fire tick.
- `EntitySnapshot` and `RenderEntityState` include projectile reconciliation
  metadata: `owner_peer`, `velocity`, `spawn_tick`, and
  `client_projectile_id`.
- Projectile entities store `spawn_tick` and `client_projectile_id` only after
  server fire validation succeeds.
- Input and snapshot packet encode/decode paths include the new fields, and the
  packet schema stays aligned with the handwritten protocol implementation.

## Unity Package Follow-Up

- Update `KernelConstants.AbiVersion` to `4`.
- Mirror native `PlayerInput` and `RenderEntityState` field order exactly.
- Update managed smoke input setup and ABI validation to cover the new fields.
- Rebuild and stage the macOS native plugin dylib after the merged native v4
  build is verified.

## Test Plan

- Native Bazel tests cover input roundtrip, snapshot metadata roundtrip,
  projectile spawn metadata retention, render state metadata, and dynamic ABI
  smoke.
- Unity package validation verifies C# struct sizes against native ABI v4 and
  confirms managed smoke can submit input and read render states.

## Assumptions

- Server authority remains unchanged for transform, ammo, cooldown, hit,
  explosion, and despawn behavior.
- `client_projectile_id == 0` means no predicted projectile id.
