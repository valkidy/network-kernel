# Gameplay Data Sync Policy

## Goal

Dedicated server gameplay tests should support fast tuning loops without
turning every stable data asset into runtime sync payload. The default rule is:

```text
If a change should be testable by restarting only the dedicated server,
it belongs in the server-synced tuning bundle.

If a change can reasonably require a native/plugin/client update,
it belongs in long-lived static data.
```

## Server-Synced Tuning Data

Short-lived tuning data changes often during gameplay iteration. It should be
loaded by the dedicated server from `bundle.zip`, announced through the
gameplay catalog manifest, downloaded by clients when stale, and loaded before
client handshake.

Examples:

- action trigger, commit cadence, ammo cost, recovery, timeout, and cancel policy
- weapon damage, cooldown, reload time, magazine size, and reserve magazines
- projectile speed, lifetime, damage, gravity, sync mode, and hit response
- collider dimensions, centers, purpose, layer, and transient lifetime
- actor and entity HP, move speed, weapon slots, and active weapon
- AI alert/forget timing, patrol timing, sentry weapon choice, and spawn tuning
- director spawn count, radius, seed, position, and target templates

These values participate in the gameplay catalog hash. Clients must not send a
normal gameplay handshake until they have loaded the server manifest's catalog.

## Long-Lived Static Data

Long-lived static data is structural, ABI-shaped, or presentation specific. It
should not be pulled into the fast dedicated-server sync path until there is a
concrete need.

Examples:

- legacy entity-type collider bindings
- entity component layout
- new enum values, projectile motion models, damage shapes, or protocol schema
- kernel ABI and Game Server bridge ABI structs
- Unity prefab mapping, VFX, animation clips, UI, cameras, and presentation
  registries

These changes may require a native build, Unity plugin update, or client build.
That cost is acceptable because they should change much less frequently than
weapon and AI tuning.

## Reference Rule

Server-synced tuning data may reference templates contained in the same bundle.
Weapon, projectile, and entity templates bind collider templates by authored
name; the loader resolves those references to stable ids before runtime use.

New enum values, component layouts, ABI fields, and presentation mappings still
require the corresponding native or client build update.

## Traffic Assessment Rule

Whenever an implementation governed by this data-driven policy adds or
changes a server data-sync packet, completion must include an updated server
data-sync traffic assessment report.

The report must include before/after encoded application packet sizes,
direction, frequency or trigger conditions, typical and upper-bound per-client
traffic, the boundary between application and transport overhead, whether
fragmentation or application-level packet splitting is required, and the
commands or tests used to obtain the measurements. C/C++ `sizeof` values alone
are not packet-size measurements.

## Current Collider Binding Position

Collider templates are part of the fast sync surface and are loaded one file
per template from `collider_template_dir`. They participate in the catalog hash
and are downloaded before client handshake when the cache is stale.

Legacy entity-type collider bindings remain outside the sync surface. The
current gameplay config validates that collider catalogs contain templates and
no bindings, and the kernel rejects non-zero collider binding counts.

If gameplay later needs multiple authoritative server-side colliders per entity,
add a focused design for minimal authoritative bindings instead of making the
entire collider/presentation model dynamically synced by default.

## Current Bundle Limitation

The compressed `bundle.zip` has a hard sync limit of 1 MiB on both server and
client. Clients may choose a smaller limit but cannot raise the version-level
cap. Sync remains a whole-bundle, pre-handshake operation with local caching;
hot reload, delta sync, and Unity presentation assets are not supported.
