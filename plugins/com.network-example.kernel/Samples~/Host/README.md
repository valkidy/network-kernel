# Host Sample

`NetworkKernelHostBehaviour` starts a listen host through the runtime SDK,
ticks the native game server bridge, submits local input with client-local
action timestamps, and reads render states as data through
`GetRenderStatesAtTime`.

The sample intentionally does not instantiate prefabs or apply render states to
GameObjects. A Unity demo project owns presentation-specific mapping such as
prefabs, scene object registries, animation, camera, pooling, and UI.

The sample loads the package default
`Runtime/Resources/gameplay_catalog_bundle/bundle.bytes` when the gameplay
catalog bundle field is empty. It starts `NetworkHost` with
`EnableClientSync = true`, so remote clients can receive the same gameplay
catalog bundle from the host.

Assign a `gameplay_catalog_bundle.bytes` TextAsset only when intentionally
overriding the package default bundle. The default entry path is
`gameplay_catalog.yaml`.
