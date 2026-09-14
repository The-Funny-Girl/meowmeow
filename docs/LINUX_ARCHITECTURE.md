# Linux architecture

The Linux port is intentionally structured as a native application rather than a compatibility build of the original Win32 executable.

## Current dependency direction

```text
linux_main.cpp
    |-- Application
    |     |-- LinuxSettings / config_store
    |     |-- Logger
    |     |-- AppPaths / linux_paths
    |     |-- TemporaryWorkspace
    |     `-- file_utils
    `-- game_integration
          `-- file_utils
```

`kirkware_platform` is a static library containing the reusable Linux runtime services. The `kirkware` executable owns command-line parsing and presentation of diagnostics/actions.

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

Writes an owner-readable append log in the XDG state directory. Logging does not depend on Windows event APIs or resources.

### `game_integration`

Implements benign Linux game integration through documented user-level Steam behavior. It discovers Garry's Mod app ID 4000 from common native/Flatpak Steam roots and `libraryfolders.vdf`, validates the manifest/install directory, and can send an explicit launch request through the `steam` command or desktop Steam URI handler. It never attaches to, maps code into, or manipulates the game process.

### `Application`

Coordinates path discovery, directory creation, settings loading, logging, stale-workspace cleanup and optional workspace creation. It also exposes non-interactive health checks used by both the CLI and automated tests.

## Boundary with the original Windows source

The Linux target does not link the original `_native_main.cpp`, Win32 UI host, D3D9 backend, PE resource pipeline or Windows bootstrap/process chain. Those files remain separate from the Linux target.

The existing Windows runtime is treated as a preserved implementation. Linux portability work must not rewrite, substitute, simplify, or otherwise alter that Windows execution path as a side effect of the Linux port. This is important both for behavioral compatibility and for keeping Windows-side security assumptions stable while the Linux architecture evolves independently.

GitHub Actions enforces this boundary for Linux work by checking the preserved Windows paths, including `source/core`, `source/ui`, the Windows build files, manifest, and bootstrap/runtime assets. A Linux change that modifies those protected paths fails the preservation check before the Linux build jobs run.

Portable logic may be extracted into new platform-neutral files only when the original Windows implementation remains intact. The Linux side should depend on its own documented interfaces rather than replacing Windows internals in place.

The Steam integration demonstrates the preferred migration pattern for Linux-only functionality: identify the user-visible behavior (find/run Garry's Mod) and implement that Linux behavior using supported Linux/Steam interfaces while leaving the existing Windows runtime untouched.

## Future GUI boundary

A Linux GUI should depend on an application-facing model rather than `NativeChainBridge`. A preferred shape is:

```text
Linux window/render backend
          |
          v
portable UI state + view model
          |
          v
Application + game services
```

That makes it possible to choose SDL2/OpenGL, GLFW/OpenGL, or another normal Linux backend without changing application services. It also lets the CLI remain useful for diagnostics and headless testing.

## Build and verification

CMake is the source of truth for the Linux build. GCC and Clang Release builds, CLI/integration tests, install smoke tests and package creation run in GitHub Actions. A separate Clang ASan/UBSan job compiles with warnings as errors. Steam discovery tests use a synthetic directory tree and never start Steam or a game in CI.

The goal for new Linux code is simple: preserve the existing Windows runtime, keep Linux code independently testable, use documented interfaces, maintain predictable filesystem ownership, and verify changes automatically.
