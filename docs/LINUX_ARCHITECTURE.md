# Linux architecture

The Linux port is intentionally structured as a native application rather than a compatibility build of the original Win32 executable.

## Current dependency direction

```text
linux_main.cpp
    |
    v
Application
    |-- LinuxSettings / config_store
    |-- Logger
    |-- AppPaths / linux_paths
    |-- TemporaryWorkspace
    `-- file_utils
```

`kirkware_platform` is a static library containing the reusable Linux runtime services. The `kirkware` executable owns command-line parsing and presentation of diagnostics.

## Components

### `linux_paths`

Resolves XDG locations and maps application-owned configuration, data, cache, state and runtime directories. It rejects relative XDG overrides so later filesystem code always receives deterministic absolute roots.

### `file_utils`

Provides the small filesystem primitives used by higher layers: private directory creation, bounded reads, atomic text replacement and writable-directory probes. This keeps low-level error handling and permissions out of application logic.

### `config_store`

Owns the native Linux `linux.conf` format. Parsing is strict and bounded. Unknown keys and malformed values are errors, which keeps configuration behavior deterministic as the port grows.

### `workspace`

Owns temporary application workspaces. Each active workspace holds an advisory `flock` on an application-owned lock file. Stale cleanup only removes old `session-*` directories that are not actively locked.

### `logger`

Writes an owner-readable append log in the XDG state directory. Logging does not depend on the Windows event APIs or resources.

### `Application`

Coordinates discovery, directory creation, settings loading, logging, stale-workspace cleanup and optional workspace creation. It also exposes non-interactive health checks used by both the CLI and automated tests.

## Boundary with the original Windows source

The Linux target does not link the original `_native_main.cpp`, Win32 UI host, D3D9 backend, PE resource pipeline or Windows bootstrap/process chain. Those files remain reference material for behavior and data-model decisions, not libraries for the Linux executable.

Portable logic may be extracted from the original source only when it can be given a platform-neutral contract and tested without Windows binaries. A good extraction has ordinary C++ inputs/outputs and no dependency on PE resources, Win32 handles, process-memory behavior or Direct3D objects.

## Future GUI boundary

A Linux GUI should depend on an application-facing model rather than `NativeChainBridge`. A preferred shape is:

```text
Linux window/render backend
          |
          v
portable UI state + view model
          |
          v
Application services
```

That makes it possible to choose SDL2/OpenGL, GLFW/OpenGL, or another normal Linux backend without changing application services. It also lets the CLI remain useful for diagnostics and headless testing.

## Build and verification

CMake is the source of truth for the Linux build. GCC and Clang Release builds, CLI/integration tests, install smoke tests and package creation run in GitHub Actions. A separate Clang ASan/UBSan job compiles with warnings as errors.

The goal for new Linux code is simple: no Windows binary dependency, a documented interface, predictable filesystem ownership, and automated verification.
