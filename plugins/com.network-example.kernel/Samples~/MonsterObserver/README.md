# Monster Observer Sample

Add `NetworkKernelMonsterObserverBehaviour` to an empty GameObject and enter
Play Mode. The sample starts a listen host with
`monster_observer_gameplay_catalog.yaml`, keeps the existing local player idle,
and follows the single template ID 20 monster with an observer camera.

## Presentation

The fallback presentation is generated from the skeleton manifests inside the
gameplay catalog bundle, by `KernelSkeletonProxyFactory`. Bone names, parents
and the bind pose all come from the manifest, so a rig can be changed or added
in the catalog and appear here without touching this sample or rebuilding the
package. `skeletonAssetIdByTemplateId` in the behaviour only says which skeleton
each entity template uses; anything not listed there falls back to a capsule.

Geometry is whatever the rig carries in its bone scales. Rigs authored as scaled
box primitives -- `simplified_biped` and `simplified_quadruped`, plus the
limbs of `simplified_tripod` -- put each `GEO_` bone's dimensions in its rest
scale, so the proxy reproduces their shape exactly. The tripod's `GEO_Body` is
the rig's authored sphere and is rendered as a sphere proxy. `simplified_monster_sim_v4`
bakes its dimensions into mesh vertices instead, and its `GEO_` bones have unit
scale, so it renders as joint markers rather than as its silhouette. Root
movement, root yaw and procedural bone poses are visible either way, which is
what the observer is for.

## Using the real art

Import the GLB with the project's chosen glTF pipeline, add
`KernelSkeletonBinding` and `KernelSkeletonPoseApplicator` to its prefab, and map
the template ID to that prefab in the `KernelEntityPresentationWorld` catalog.
`KernelSkeletonBinding` maps bones by name against the manifest, so `Bones` no
longer has to be filled in by hand -- but the imported hierarchy must survive
intact, which means disabling Optimize Game Objects.

One caveat that is easy to lose a day to: nothing in this pipeline converts
coordinates. The exporter writes Unity's own local TRS, `gltf2ozz` copies it into
ozz verbatim, and the kernel hands bone transforms back the same way. A glTF
importer that converts handedness -- which the common ones do by default --
breaks that correspondence on every bone with a non-trivial rotation, and
`PreservePrefabBindPose` will not rescue it: that corrects a constant bind
offset within one handedness, not a mirror. The manifest-built proxy has no such
problem because its transforms come from the same numbers the kernel solves
against.

## Bundle

The package gameplay bundle must contain the test entry before running this
sample. Rebuild/stage the package after an implementation change to refresh
`Runtime/Resources/gameplay_catalog_bundle/bundle.bytes`; the manifests the
proxy reads come from that same file, so a stale bundle shows a stale rig.
