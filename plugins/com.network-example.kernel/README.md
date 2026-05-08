# Network Example Kernel Unity Package

This package is the M6.4 Unity prototype for the network-example native kernel.
It is intentionally thin: gameplay and networking behavior stay in the native
kernel, while Unity owns plugin loading, input sampling, and render-state
consumption.

## Install

Add this package to a Unity project with a local package reference:

```json
{
  "dependencies": {
    "com.network-example.kernel": "file:/absolute/path/to/network-example/plugins/com.network-example.kernel"
  }
}
```

## Native Plugin

Build the macOS dylib from the repo root:

```text
bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
```

The package keeps the produced native plugin at:

```text
plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib
```

Unity resolves the C# import name `network_kernel` to
`libnetwork_kernel.dylib` on macOS.

## Smoke Checks

Required compile/ABI smoke:

- Compile the package runtime/editor C# assemblies with Unity.
- In Unity Editor, run `Network Example/Kernel Hello World` to confirm the
  Editor can load `libnetwork_kernel.dylib` and call `Kernel_GetAbiInfo`.
- Compile and run `Tests~/AbiSmoke/NetworkKernelManagedAbiSmoke.cs` with the
  package runtime sources in a directory containing `libnetwork_kernel.dylib`;
  it validates ABI sizes and exercises create/start/update/input/render/event
  calls without opening Unity Editor.
- Run `NetworkExample.Kernel.Editor.NetworkKernelAbiSmokeRunner.Run` in an
  environment where Unity batchmode licensing/headless execution works.
- The runner calls `Kernel_GetAbiInfo`, compares native struct sizes with
  `Marshal.SizeOf<T>()`, starts listen-server mode, checks local-player info,
  updates, submits one input, polls events, reads render states, and destroys
  the handle.

## Command-Line Server + Unity Client

Build the app and native plugin from the repo root:

```text
bazel build //app:app //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
```

Start the dedicated server:

```text
bazel run //app:app --config=macos -- --mode=dedicated_server
```

In Unity, import the `Client` sample and add `NetworkKernelClientBehaviour` to
an empty GameObject. The behaviour connects to `127.0.0.1:7777` by default,
waits for `PlayerJoined`, reads the assigned local player through
`TryGetLocalPlayerInfo`, and stops submitting input after `Disconnected`.

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
