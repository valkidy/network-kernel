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
  `Marshal.SizeOf<T>()`, starts listen-server mode, updates, submits one input,
  polls events, reads render states, and destroys the handle.

Local note: Unity 6000.4.3f1 batchmode starts on the current development
machine, but headless licensing initialization times out with
`LicenseClient-kasaki doesn't exist` and `com.unity.editor.headless was not
found`. Treat Editor batchmode smoke as environment-dependent until that Unity
installation is fixed.
