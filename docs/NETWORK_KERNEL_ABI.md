# Network Kernel ABI

M6.1 through M6.3 define the native plugin boundary for the network kernel.
M6.4 adds a thin Unity C# package over the same C ABI.

## Public ABI

The public ABI is the `extern "C"` `Kernel_*` surface declared in
`engine/src/kernel/public/kernel_api.h`. `KernelHandle` is opaque; callers must
create it with `Kernel_Create` and release it with `Kernel_Destroy`.

`Kernel_GetAbiInfo` returns the ABI version, public struct sizes, and capability
flags. Consumers should call it before creating a kernel and reject an ABI
version they do not support. The current native ABI version is
`KERNEL_ABI_VERSION == 4`.

## Ownership

Callers own every input and output buffer passed to the ABI. The kernel copies
input data during the call and copies output data into caller-owned buffers. It
does not retain caller buffer pointers after an ABI call returns.

All handles are single-thread owned unless a later ABI revision explicitly
changes that rule. No STL, EnTT, glm, FlatBuffers, or C++ ownership types cross
the public boundary. C++ exceptions are contained inside the ABI implementation;
failures return `NULL`, `false`, or `0`.

## Compatibility Rules

Additive changes must prefer new `Kernel_*` functions or new capability flags.
Breaking changes to public struct layout, enum semantics, buffer ownership, or
function signatures require a `KERNEL_ABI_VERSION` bump.

ABI version 3 adds server-only gameplay scaffolding for external dedicated
server logic. The kernel exposes generic entity create/destroy, transform,
velocity, persistent state write, and entity query functions. These functions
are intentionally not enemy-specific, and they fail when used from client mode.

ABI version 4 adds projectile prediction reconciliation metadata. `PlayerInput`
includes `client_projectile_id`; `client_tick` remains the client fire tick.
Projectile snapshots and render states expose `owner_peer`, `velocity`,
`spawn_tick`, and `client_projectile_id` so managed clients can match a
server-authoritative projectile to a locally predicted projectile after the
server validates ammo, cooldown, and weapon state.

Consumers pass a `struct_size`-style byte size to `Kernel_GetAbiInfo`. The call
returns `false` if the output pointer is null or the provided size is smaller
than the current `KernelAbiInfo` layout.

## Exported Symbols

`libnetwork_kernel.dylib` exports only the public `Kernel_*` symbols on macOS.
Internal C++ symbols and third-party dependency symbols are hidden by linker
export flags. `dynamic_abi_smoke_test` verifies this with `nm -gU`.

## macOS Dependencies

Build the optimized macOS dylib with:

```text
bazel build //engine/src/kernel:network_kernel_shared --config=macos --copt=-Wunused-function -c opt
```

It produces:

```text
libnetwork_kernel.dylib
```

The macOS build links against Homebrew OpenSSL:

```text
/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib
/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
```

External consumers must either run on a machine with those libraries available
at those install names or package equivalent runtime dependencies with adjusted
install names in a later packaging step.

## Unity Prototype

The Unity package lives at:

```text
plugins/com.network-example.kernel
```

It provides handwritten C# P/Invoke declarations for the public `Kernel_*`
surface, C# mirror structs for public ABI data, an `IDisposable` wrapper for
`KernelHandle`, local player info helpers, an Editor ABI smoke runner, and
minimal listen-server/client smoke samples.

On macOS, the Unity package includes the built dylib at:

```text
plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib
```

Unity resolves the C# import name `network_kernel` to `libnetwork_kernel.dylib`
on macOS. The Editor smoke runner calls `Kernel_GetAbiInfo`, validates
`Marshal.SizeOf<T>()` against native struct sizes, starts listen-server mode,
checks `Kernel_GetLocalPlayerInfo`, updates the kernel, submits one input,
polls events, reads render states, and destroys the handle.
