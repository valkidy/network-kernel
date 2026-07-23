# Projectile Prediction ABI v5–v7 — Historical Record

Status: **Implemented and superseded**

This file records the ABI v5–v7 projectile prediction milestone. It is not a
current implementation plan and must not be used to determine present ABI,
packet, snapshot, or Unity package versions.

Current references:

- `docs/NETCODE_SYNC_POLICY.md` — active synchronization and authority policy.
- `docs/NETWORK_KERNEL_ABI.md` — current ABI and compatibility history.
- `docs/AUTHORITATIVE_WEAPON_CURRENT_IMPLEMENTATION.md` — current weapon and
  projectile runtime.

## Delivered In ABI v5–v7

The milestone established:

- `client_action_time_us` for client-local action timing;
- non-zero action correlation IDs for predicted projectile binding;
- client-local stable `RenderEntityState::entity_id`;
- authoritative `net_id` assignment after prediction binding;
- owner prediction matched by owner identity and action correlation ID;
- PingPong-based clock conversion and a bounded compensation window;
- `Kernel_GetRenderStatesAtTime` for render-timeline interpolation;
- local predicted projectile correction separated from remote interpolation;
- server-authoritative pending projectile damage with Dodge/Parry timing.

These changes are now part of the broader netcode and generalized action
systems. Later revisions added deterministic projectile collision, action
intent/result streams, data-driven projectile and collider templates, Jolt
static collision, and time-based presentation smoothing.

## Historical Runtime Contract

The original owner flow was:

```text
local fire prediction
  -> submit action time and correlation id
  -> server validates and spawns the authoritative projectile
  -> snapshot binds owner + correlation id to authoritative net_id
  -> owner fast-forwards authoritative past state
  -> correction converges without replacing the stable local entity_id
```

Remote entities remained on the delayed interpolation timeline. Gameplay
damage and hit decisions remained server authoritative.

## Current Version Boundary

As of 2026-07-23:

```text
KERNEL_ABI_VERSION = 45
protocol version = 1
packet schema version = 17
snapshot schema version = 15
```

Historical test expectations and follow-up items from ABI v5–v7 should be read
from Git history, not treated as current TODOs.
