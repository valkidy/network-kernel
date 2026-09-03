0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 86


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 85


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 84


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 84


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 82


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 80


0.7.0 release notes:

- aligns Unity plugin API with kernel ABI 79


0.7.0 release notes:

- aligns the managed API with kernel ABI 79, catching up three kernel revisions
  at once. Reading this as one change is the point: 77, 78 and 79 all concern a
  legged rig's per-bone colliders, and a mirror that takes only some of them is
  worse than one that takes none.

- ABI 79 appends `collision_mask` to `KernelWeaponMechanicsDefinition`. This is
  the one that breaks silently if it is missed: the struct is nested inside the
  gameplay catalog, so an out-of-date mirror does not report a size error, it
  shifts every field after it. Zero means the engine default, which is what
  every weapon meant before the field existed.

- ABI 78 changes what `KernelSkeletonColliderDefinition.hit_zone` means. Same
  width, same offset, new meaning: a damage multiplier in hundredths, where
  `KernelConstants.HitZoneUnscaled` (100) leaves damage alone and 0 makes a hit
  on that volume harmless. It was previously an unused body-part id that was
  always zero -- so a catalog built against 77 would describe every volume as
  harmless.

- ABI 77 adds `KernelConstants.CollisionLayerLimb` (0x08), the gameplay-side bit
  that lets a projectile, beam or prop trigger name a rig's bones as a target.
  Distinct from `MovementLayerLimb` (0x10), which is a different bit space; the
  two are never interchangeable.

- The managed ABI smoke now compares the sizes the kernel reports against the
  managed structs for weapon mechanics, projectile mechanics, combat state and
  entity templates, and pins both limb layers and the neutral hit_zone. Nothing
  compared those before, which is how an appended field could go unmirrored
  without a single check going red.


0.6.11 release notes:

- fixes MinGW large-object package builds


0.6.11 release notes:

- shrinks the beam snapshot record from 34 bytes to 6 (snapshot schema 18 -> 19)
- no managed API change: kernel ABI stays at 76, every public struct keeps its
  layout, and RenderEntityState.beam_end still means the same thing. It is now
  derived from the beam's origin, orientation and reach while render states are
  built, rather than decoded straight off the wire, so presentation code that
  reads it needs no change.
- breaks wire compatibility. A client on 0.6.10 or earlier is rejected at the
  handshake by a server built from this kernel, with a snapshot schema
  mismatch. Clients and servers must be upgraded together.


0.6.10 release notes:

- aligns Unity plugin API with kernel ABI 76
- adds MovementLayerLimb and MovementMaskSupported; a template may now ask to be
  blocked by a legged rig's per-bone colliders. MovementMaskDefault is unchanged,
  so any actor that authors no mask keeps exactly the blockers it had.


0.6.9 release notes:

- refreshes Unity package artifacts


0.6.9 release notes:

- refreshes Unity package artifacts


0.6.9 release notes:

- aligns Unity plugin API with kernel ABI 75


0.6.9 release notes:

- refreshes Unity package artifacts


0.6.9 release notes:

- adds tripod sphere presentation proxy
- aligns Unity plugin API with kernel ABI 74


0.6.9 release notes:

- adds managed status-effect query bindings


0.6.9 release notes:

- aligns Unity plugin API with kernel ABI 73


0.6.9 release notes:

- updates gameplay catalog bundle


0.6.9 release notes:

- updates native plugins and gameplay catalog bundle


0.6.9 release notes:

- adds Unity meta files for skeleton manifest sources


0.6.9 release notes:

- aligns Unity managed bindings with kernel ABI 67


0.6.9 release notes:

- updates Unity package artifacts for the latest kernel merge


0.6.9 release notes:

- syncs native plugins and gameplay bundle after latest main merge


0.6.9 release notes:

- refreshes Unity package artifacts after main merge


0.6.9 release notes:

- revalidates Unity package artifacts against kernel ABI 66


0.6.9 release notes:

- updates native plugins and gameplay catalog bundle


0.6.9 release notes:

- updates native plugins


0.6.9 release notes:

- aligns Unity skeleton locomotion bindings with kernel ABI 66


0.6.9 release notes:

- aligns Unity skeleton poses with native bind-relative transforms and kernel ABI v66
- updates quadruped v5 bindings and movement-driven locomotion catalog assets


0.6.9 release notes:

- updates native plugins and gameplay catalog bundle


0.6.9 release notes:

- updates native plugins and gameplay catalog bundle


0.6.9 release notes:

- updates native plugins


0.6.9 release notes:

- aligns Unity package artifacts with kernel ABI 61


0.6.9 release notes:

- aligns Unity editor and managed ABI smoke with kernel ABI 60


0.6.9 release notes:

- aligns Unity plugin API with kernel ABI 60
- updates native plugins and gameplay catalog bundle


0.6.9 release notes:

- fixes macOS owned inventory container API export


0.6.9 release notes:

- rebuilds macOS and Windows native plugins for kernel ABI 57


0.6.9 release notes:

- aligns Unity plugin API with kernel ABI 57
- adds managed item, prop, inventory, and semantic gameplay-request bindings
- updates samples and ABI smoke coverage for renamed player input and item/prop layouts


0.6.9 release notes:

- keeps local predicted actor presentation independent from simulation ticks and external render query time
- smooths prediction, reconciliation, ground correction, and velocity changes without reversing active movement
- preserves C and managed ABI 45 with no public API changes


0.6.8 release notes:

- refreshes macOS and Windows native plugins for kernel ABI 45


0.6.8 release notes:

- makes macOS local predicted actor rendering frame-rate independent
- makes predicted projectile correction decay frame-rate independent
- preserves C and managed ABI 45 with no public API changes

0.6.7 release notes:

- preserves client sessions across out-of-range actor tombstones
- marks fatal client prediction failures as failed and stops managed input


0.6.6 release notes:

- aligns Unity plugin API with kernel ABI 45


0.6.6 release notes:

- aligns Unity plugin API with kernel ABI 43


0.6.6 release notes:

- aligns Unity benchmark and movement bindings with kernel ABI 42
- initializes movement layouts for managed gameplay catalogs


0.6.6 release notes:

- aligns Unity plugin API with kernel ABI 42


0.6.6 release notes:

- aligns Unity plugin API with kernel ABI 35 and game server ABI 5


0.6.6 release notes:

- aligns Unity plugin API with kernel ABI 34


0.6.6 release notes:

- aligns Unity API with kernel ABI 34
- adds control-plane RPC and entity health bindings


0.6.6 release notes:

- adds Unity gameplay catalog synchronization


0.6.6 release notes:

- aligns Unity bindings with kernel ABI v27


0.6.6 release notes:

- updates native plugins and gameplay catalog bundle


0.6.6 release notes:

- aligns Unity bindings with kernel ABI v26
- updates Unity ABI smoke coverage for actor templates


0.6.6 release notes:

- fixes projectile render state duplication


0.6.6 release notes:

- updates managed bindings for kernel ABI v18


0.6.5 release notes:

- updates native plugins


0.6.5 release notes:

- updates managed bindings for kernel ABI v17
- updates managed bindings for GameServer ABI v4


0.6.5 release notes:

- updates native plugins


0.6.5 release notes:

- adds managed gameplay config bundle loading APIs
- wires client and host samples to optional TextAsset bundle loading


0.6.5 release notes:

- updates native plugins


0.6.5 release notes:

- updates native plugins


0.6.4 release notes:

- updates native combat tuning


0.6.4 release notes:

- updates macOS native plugin for ABI 8 health render states
