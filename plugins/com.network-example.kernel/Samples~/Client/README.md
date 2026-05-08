# Network Kernel Client Sample

Add `NetworkKernelClientBehaviour` to an empty GameObject in a Unity scene.

Before entering Play Mode on macOS:

1. Build and package the native plugin:

   ```text
   bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
   ```

2. Start the command-line dedicated server from the repo root:

   ```text
   bazel run //app:app --config=macos -- --mode=dedicated_server
   ```

3. Enter Play Mode. The sample connects to `127.0.0.1:7777` by default.

The sample waits for `PlayerJoined`, reads the local player id through
`TryGetLocalPlayerInfo`, submits input only while connected, filters render
states by `LocalPlayerNetId`, and logs `PlayerLeft` and `Disconnected` events.
`GetRenderStates` returns renderable entities, not the connected client count.
