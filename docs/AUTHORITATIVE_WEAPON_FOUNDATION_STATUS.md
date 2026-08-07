# Authoritative Weapon Foundation Status — Superseded

Status: **Superseded on 2026-07-23**

This file described the weapon foundation at native ABI 17. It is retained as
a compatibility pointer because older tasks and commits reference its path; it
must not be used as the current weapon or Unity package status.

Use these sources instead:

- `docs/AUTHORITATIVE_WEAPON_CURRENT_IMPLEMENTATION.md` for implemented weapon,
  projectile, damage, and interaction behavior.
- `docs/NETWORK_KERNEL_ABI.md` for the current public ABI and compatibility
  history.
- `docs/NETCODE_SYNC_POLICY.md` for prediction, interpolation, and server
  authority rules.
- `plugins/output/com.network-example.kernel-0.6.9.tgz` for the current packed
  Unity artifact.

## Historical Context

At the time of the original document, the implementation used a dense weapon
array and the Unity package lagged behind native ABI 17. Those constraints no
longer describe the repository:

- The native kernel is ABI 45.
- Weapon catalog IDs are sparse `uint8_t` values.
- Actor inventories contain at most
  `KERNEL_MAX_WEAPON_SLOTS == 4u` runtime slots.
- Each runtime slot maps to a catalog ID through `weapon_ids`.
- The current Unity 0.6.9 package mirrors ABI 45 and includes macOS and Windows
  x86_64 native plugins.

Historical foundation details remain available in Git history before this
superseding update.
