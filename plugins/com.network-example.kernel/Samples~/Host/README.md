# Host Sample

`NetworkKernelHostBehaviour` starts a listen host through the runtime SDK,
ticks the native game server bridge, submits local input with client-local
action timestamps, and reads render states as data through
`GetRenderStatesAtTime`.

The sample intentionally does not instantiate prefabs or apply render states to
GameObjects. A Unity demo project owns presentation-specific mapping such as
prefabs, scene object registries, animation, camera, pooling, and UI.
