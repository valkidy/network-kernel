# Network Example Kernel Unity Package

This package is the Unity package for the network-example native kernel
ABI v14 plus the Game Server native bridge ABI v2.
It is intentionally a pure networking/runtime SDK: native networking and game
server behavior stay in C++, while Unity projects own input sampling,
prefab/entity mapping, animation, scene objects, cameras, UI, and render-state
presentation.

## Install

Add this package to a Unity project with a local package reference:

```json
{
  "dependencies": {
    "com.network-example.kernel": "file:/absolute/path/to/network-example/plugins/com.network-example.kernel"
  }
}
```

For remote Git installation, point Unity Package Manager at the package
directory that contains this `package.json`:

```json
{
  "dependencies": {
    "com.network-example.kernel": "https://github.com/<owner>/<repo>.git?path=/plugins/com.network-example.kernel#dev-latest"
  }
}
```

Replace `dev-latest` with an immutable `v*` release tag for release builds.

## Native Plugin

Build, stage, verify, and pack the macOS and Windows x86_64 package from the
repo root through the package builder:

```text
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh --unity off
```

The builder is the supported packaging entry point. It builds
`//engine/src/kernel:network_kernel_shared`, stages the Bazel-signed macOS
dylib and Windows x86_64 DLL resources into the Unity package, removes
`.DS_Store` files, verifies ABI/version/export alignment, and writes the UPM
tarball under:

```text
plugins/output
```

The produced native plugins export both `Kernel_*` and `GameServer_*` symbols.
The package keeps the produced native plugins at:

```text
plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib
plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/network_kernel.dll
```

Unity resolves the C# import name `network_kernel` to
`libnetwork_kernel.dylib` on macOS and `network_kernel.dll` on Windows.

## Smoke Checks

Required compile/ABI smoke:

- Run the package builder verification without Unity:

  ```text
  .agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh --mode verify --unity off
  ```

- Compile the package runtime/editor C# assemblies with Unity.
- In Unity Editor, run `Network Example/Kernel Hello World` to confirm the
  Editor can load `libnetwork_kernel.dylib` and call `Kernel_GetAbiInfo`.
- Compile and run `Tests~/AbiSmoke/NetworkKernelManagedAbiSmoke.cs` with the
  package runtime sources in a directory containing `libnetwork_kernel.dylib`;
  it validates kernel and game-server ABI sizes, exercises
  create/start/update/input/render/event calls, player/enemy render-state
  health, projectile sync metadata,
  server entity create/query/update/destroy calls, and a pure runtime
  `NetworkHost` smoke without opening Unity Editor.
- Run `NetworkExample.Kernel.Editor.NetworkKernelAbiSmokeRunner.Run` in an
  environment where Unity batchmode licensing/headless execution works. In
  package-builder auto mode, missing or blocked Unity is reported as a skip
  instead of a package failure.
- The runner calls `Kernel_GetAbiInfo` and `GameServer_GetAbiInfo`, compares
  native struct sizes with `Marshal.SizeOf<T>()`, starts listen-server mode,
  checks local-player info, updates, submits one projectile input, creates an
  enemy, queries and mutates server entity state, polls events, reads render
  states including projectile sync metadata, verifies render states at a
  client-local render timestamp, destroys the enemy, runs a `NetworkHost`
  smoke through the native
  `GameServer_*` bridge, and destroys the handles.

To confirm the packaged dylib has the ABI v14 kernel server exports and Game
Server v2 bridge exports:

```text
nm -gU plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib | grep Kernel_Server
nm -gU plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib | grep GameServer_
```

## Runtime Layout

The runtime package is split by SDK responsibility:

- `Runtime/Core`: native ABI mirrors, P/Invoke declarations, ABI validation,
  and native handle ownership.
- `Runtime/Client`: client connection/session flow, local-player readiness,
  input submission, event polling, and render states as data.
- `Runtime/Host`: listen-host lifecycle, native Game Server bridge ownership,
  local-client input bridging, and host tick ordering.

Runtime code does not depend on `UnityEngine`. Samples may use Unity APIs to
demonstrate input collection and logging.

## Command-Line Server + Unity Client

Build the app and native plugin from the repo root:

```text
bazel build //app:signed_dedicated_server //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
```

Start the dedicated server:

```text
bazel-bin/app/signed/network_kernel_dedicated_server --mode=dedicated_server
```

In Unity, import the `Client` sample and add `NetworkKernelClientBehaviour` to
an empty GameObject. The behaviour connects to `127.0.0.1:7777` by default,
waits for `PlayerJoined`, reads the assigned local player through
`NetworkClient`, and stops submitting input after `Disconnected`.

## Unity Host Sample

Import the `Host` sample and add `NetworkKernelHostBehaviour` to an empty
GameObject. The behaviour starts listen-server mode, creates the native Game
Server bridge through `NetworkHost`, submits local input, ticks the native game
server, and logs render-state data and enemy count.

The sample intentionally does not instantiate prefabs or apply render states to
GameObjects. A Unity demo project owns presentation-specific mapping such as
prefabs, scene object registries, animation, camera, pooling, and UI.

`Kernel.GetRenderStates()` returns renderable world entities, not connected
client count. A single connected client may see its player, enemies,
projectiles, and other replicated entities. Use `Kernel.LocalPlayerNetId` or
`TryGetLocalPlayerInfo` to identify the local player state.

When another player leaves, consume `KernelEventType.PlayerLeft` through
`Kernel.PollEvents`. Snapshots remain the source of render-state truth; the
event is for gameplay/UI notification.

Local note: Unity 6000.4.3f1 batchmode starts on the current development
machine, but headless licensing initialization times out with
`LicenseClient-kasaki doesn't exist` and `com.unity.editor.headless was not
found`. Treat Editor batchmode smoke as environment-dependent until that Unity
installation is fixed.
