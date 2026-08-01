# Monster Observer Sample

Add `NetworkKernelMonsterObserverBehaviour` to an empty GameObject and enter
Play Mode. The sample starts a listen host with
`monster_observer_gameplay_catalog.yaml`, keeps the existing local player idle,
and follows the single template ID 20 monster with an observer camera.

The default fallback presentation is generated from the v4 GLB node hierarchy
and mesh bounds. It binds skeleton asset ID `1`, content hash
`0x1c171165d9bb479b`, and all 41 Native bone indices, so root movement, root yaw,
and procedural bone poses remain visible without adding a GLB importer to the
runtime package.

For final art, import `simplified_monster_sim_v4.glb` into the Unity project with
the project's chosen glTF pipeline, add `KernelSkeletonBinding` and
`KernelSkeletonPoseApplicator` to its prefab, and map template ID `20` to that
prefab in the `KernelEntityPresentationWorld` catalog. `Bones[index]` must match
the Native 41-bone order and the binding ID/hash must remain unchanged.

The package gameplay bundle must contain the test entry before running this
sample. Rebuild/stage the package after the implementation change to refresh
`Runtime/Resources/gameplay_catalog_bundle/bundle.bytes`.
