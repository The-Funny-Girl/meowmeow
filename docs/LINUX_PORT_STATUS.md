# Linux port status

This document tracks what is usable on Linux and what remains Windows-specific.

## Implemented on Linux

The `linux-port` branch has a native CMake/C++20 runtime with:

- XDG-compliant config, data, cache, state and runtime directories, with validation that XDG overrides are absolute.
- Per-user application directory and file permissions.
- Persistent validated `linux.conf` settings.
- Native Linux logging.
- Bounded text file reads and atomic text file replacement.
- Temporary per-process workspaces with automatic cleanup.
- File locks protecting active workspaces from stale-workspace cleanup.
- Configurable stale-workspace retention.
- CLI diagnostics (`--paths`, `--settings`, `--check`, `--version`).
- Automated integration and CLI tests for paths, settings, file I/O, workspace locking/cleanup and application startup.
- CMake Debug, Release and ASan/UBSan presets.
- Install rules plus portable `.tar.gz` and Debian/Ubuntu/Linux Mint `.deb` packages.
- GitHub Actions builds/tests with GCC and Clang, downloadable executable/package artifacts, install smoke tests, and a dedicated Clang sanitizer job.

## Intentionally not part of the Linux target

The original Windows runtime assumes Win32, PE images, embedded Windows resources, DLLs, Direct3D 9 and Windows-specific process/runtime behavior. The Linux build does not attempt to reinterpret those artifacts as ELF objects or shared libraries.

The following areas therefore remain outside the native Linux target:

- `BUILD.ps1` and its PE/resource validation pipeline.
- `source/app.manifest`.
- Direct3D 9 and Win32 ImGui backends in `source/ui` and the bundled ImGui backend directory.
- PE/DLL assets such as `bootstrap-b1.dll` and `bootstrap-b2.dll`.
- Windows-only APIs used by `_native_main.cpp` and related runtime code.
- Windows-specific process/bootstrap/hook code.
- UI states that are directly coupled to the Windows native-chain/process implementation.

## Migration progress

### Platform/runtime foundation — implemented

Filesystem layout, validated settings, logging, workspace ownership/lifetime, diagnostics, testing, installation, distro-friendly packaging and CI are now native Linux components with no Windows binary dependency.

### Configuration — partially implemented

The Linux runtime now has its own ordinary text settings file and safe file primitives. The original Windows config sanitizer still depends on an embedded PE resource and Win32 file APIs, so it is not linked into Linux. Any reusable schema/data parsing should be separated from that Windows resource validation before reuse.

### UI — not yet ported

The existing UI renderer is coupled both to D3D9/Win32 and to `NativeChainBridge`. A future Linux GUI should separate presentation/state from platform hosting first, then use a supported Linux backend such as SDL2/OpenGL or GLFW/OpenGL. It should not translate the Windows native process chain mechanically.

### Native integrations — require Linux-specific design

Functionality that genuinely needs operating-system or game integration must be designed against documented Linux facilities and normal ELF/shared-library conventions. Windows PE/bootstrap assumptions are not portable interfaces.

## Design rules

- The Linux executable must remain independently buildable and testable without Windows binaries.
- New portable services belong behind platform-neutral interfaces or in `source/platform` when Linux-specific.
- Linux filesystem state should use XDG locations and normal POSIX ownership/locking semantics.
- Avoid adding a Windows compatibility layer merely to preserve PE/resource assumptions.
- Prefer documented OS/application interfaces to implementation-specific process machinery.
- Every new Linux subsystem should have a non-interactive test or health check where practical.

See `docs/LINUX_ARCHITECTURE.md` for the current component boundaries.
