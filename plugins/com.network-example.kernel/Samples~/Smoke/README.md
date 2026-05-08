# Network Kernel Smoke Sample

Add `NetworkKernelSmokeBehaviour` to an empty GameObject in a Unity scene.

Before entering Play Mode on macOS:

1. Build the native plugin:

   ```text
   bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
   ```

2. Confirm the package contains the native plugin:

   ```text
   plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib
   ```

The sample starts listen-server mode, submits local input every frame, advances
the native kernel at 30 Hz, polls events, and reads render states.
Use the `Client` sample when validating Unity against a command-line dedicated
server.
