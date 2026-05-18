# Managed ABI Smoke

This runner validates the C# mirror struct sizes against the native ABI without
opening Unity Editor. Compile it with the package Runtime sources and run it in
a directory where `libnetwork_kernel.dylib` is available.

The smoke calls `Kernel_GetAbiInfo()` through P/Invoke, compares the native
sizes with `Marshal.SizeOf<T>()`, validates local-player info, starts
listen-server mode, submits one projectile input, updates once, creates and
mutates a server-owned enemy, reads render states including projectile sync
metadata, reads render states at a client-local render timestamp, polls
events, destroys the enemy, and disposes the native handle.
