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

3. Keep the behaviour's catalog startup mode at `DedicatedServerSync` for
   dedicated-server gameplay testing. The client requests the server manifest,
   downloads or reuses the cached `bundle.zip`, loads that catalog, and then
   completes the normal connection handshake.

4. Enter Play Mode. The sample connects to `127.0.0.1:7777` by default.

Use `LocalBundleOverride` only for explicit compatibility or offline debugging.
That mode requires assigning a `gameplay_catalog_bundle.bytes` TextAsset and
skips server catalog sync.

The sample waits for `PlayerJoined`, reads the local player id through
`TryGetLocalPlayerInfo`, submits input only while connected, passes a
client-local action timestamp with each input, reads render states through
`GetRenderStatesAtTime`, filters render states by `LocalPlayerNetId`, and logs
`PlayerLeft` and `Disconnected` events. Render states are renderable entities,
not the connected client count.
