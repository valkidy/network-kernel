# Host Sample

`NetworkKernelHostBehaviour` starts a listen host through the runtime SDK,
ticks the native game server bridge, submits local input with client-local
action timestamps, and reads render states as data through
`GetRenderStatesAtTime`.

The sample intentionally does not instantiate prefabs or apply render states to
GameObjects. A Unity demo project owns presentation-specific mapping such as
prefabs, scene object registries, animation, camera, pooling, and UI.

To exercise config bundle loading, build
`//game_server/gameplay_catalog_bundle:bundle.zip`, copy the output into the
Unity project as `gameplay_catalog_bundle.bytes`, and assign that `TextAsset` to
the gameplay catalog bundle field. The default entry path is
`gameplay_catalog.yaml`.
