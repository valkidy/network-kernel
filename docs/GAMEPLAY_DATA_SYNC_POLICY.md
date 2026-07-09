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

- weapon damage, cooldown, reload time, magazine size, and reserve magazines
- projectile speed, lifetime, damage, gravity, sync mode, and hit response
- actor and entity HP, move speed, weapon slots, and active weapon
- AI alert/forget timing, patrol timing, sentry weapon choice, and spawn tuning
- director spawn count, radius, seed, position, and target templates

These values participate in the gameplay catalog hash. Clients must not send a
normal gameplay handshake until they have loaded the server manifest's catalog.

## Long-Lived Static Data

Long-lived static data is structural, geometric, ABI-shaped, or presentation
specific. It should not be pulled into the fast dedicated-server sync path until
there is a concrete need.

Examples:

- collider templates and collider bindings
- entity component layout
- new enum values, projectile motion models, damage shapes, or protocol schema
- kernel ABI and Game Server bridge ABI structs
- Unity prefab mapping, VFX, animation clips, UI, cameras, and presentation
  registries

These changes may require a native build, Unity plugin update, or client build.
That cost is acceptable because they should change much less frequently than
weapon and AI tuning.

## Reference Rule

Server-synced tuning data may reference long-lived static ids only when those
ids are already present in the client/native build. For example, a tuning
bundle may change a weapon to use an existing projectile or collider template,
but it must not reference a collider id that the receiving client build does
not know how to validate or present.

If a gameplay experiment needs new static ids, update the static data and
client/native build first. After that, follow-up tuning changes can stay in the
smaller server-synced bundle.

## Current Collider Binding Position

Collider bindings are intentionally not part of the fast sync surface today.
The current gameplay config validates that collider catalogs contain templates
and no bindings, and the kernel rejects non-zero collider binding counts.

If gameplay later needs multiple authoritative server-side colliders per entity,
add a focused design for minimal authoritative bindings instead of making the
entire collider/presentation model dynamically synced by default.
