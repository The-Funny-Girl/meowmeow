# Linux port status

This document tracks what is usable on Linux and what remains Windows-specific.

## Implemented on Linux

The `linux-port` branch has a native CMake/C++20 runtime with:

- XDG-compliant config, data, cache, state and runtime directories.
- Per-user directory permissions.
- Native Linux logging.
- Bounded text file reads and atomic text file replacement.
- Temporary per-process workspaces with automatic cleanup.
- Cleanup of stale application-owned workspaces.
- CLI diagnostics (`--paths`, `--check`, `--version`).
- Automated tests for path discovery, file I/O, workspace cleanup and application startup.
- GitHub Actions builds/tests with both GCC and Clang, including downloadable executable artifacts.

## Intentionally not part of the Linux target

The original Windows runtime contains code that assumes Win32, PE images, embedded Windows resources, DLLs, Direct3D 9 and Windows-specific process/runtime behavior. The Linux build does not attempt to reinterpret those artifacts as ELF objects or shared libraries.

In particular, the following areas remain outside the native Linux target:

- `BUILD.ps1` and its PE/resource validation pipeline.
- `source/app.manifest`.
- Direct3D 9 and Win32 ImGui backends in `source/ui` and bundled ImGui backends.
- PE/DLL assets such as `bootstrap-b1.dll` and `bootstrap-b2.dll`.
- Windows-only APIs used by `_native_main.cpp` and related runtime code.
- Windows-specific process/bootstrap/hook code.

## Recommended next migration stages

1. **Portable configuration model** — separate data/schema parsing from embedded Win32 resource validation and use ordinary Linux files.
2. **Portable UI model** — split UI state/rendering from the D3D9/Win32 host, then add a supported Linux window/render backend such as SDL2/OpenGL or GLFW/OpenGL.
3. **Application services** — move safe filesystem, logging and settings behavior behind platform-neutral interfaces and reuse it from both platforms where practical.
4. **Linux-native integrations** — design Linux functionality against documented Linux/game interfaces rather than translating Windows PE/process machinery mechanically.

## Design rule

New Linux code should be independently buildable and testable without requiring Windows binaries. Windows code remains available for reference, but the Linux target should prefer documented OS facilities and normal ELF/shared-library conventions.
