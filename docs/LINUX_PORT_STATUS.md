# Linux port status

This document tracks what is usable on Linux and what remains Windows-specific.

## Implemented on Linux

The native CMake/C++20 runtime now provides:

- XDG-compliant config, data, cache, state and runtime directories, with validation that XDG overrides are absolute.
- Per-user application directory and file permissions.
- Persistent validated `linux.conf` settings.
- Native Linux logging.
- Bounded text file reads and atomic text file replacement.
- Temporary per-process workspaces with automatic cleanup.
- File locks protecting active workspaces from stale-workspace cleanup.
- Configurable stale-workspace retention.
- Native Steam library discovery for Garry's Mod app ID 4000, including secondary library folders.
- Normal Linux game launching through `steam -applaunch 4000` or the desktop Steam URI handler.
- A native SDL2/OpenGL graphical UI that reproduces the Windows UI's dimensions, theme, custom widgets, animated transitions, login/home presentation, and Garry's Mod tile.
- A native in-process Linux component with a stable C ABI and host-side `ComponentSession` lifecycle.
- CLI diagnostics (`--paths`, `--settings`, `--game-info`, `--check`, `--version`).
- Automated integration and CLI tests plus a graphical smoke test under Xvfb/llvmpipe.
- CMake Debug, Release and ASan/UBSan presets.
- Install rules plus portable `.tar.gz` and Debian/Ubuntu/Linux Mint `.deb` packages.
- GitHub Actions builds/tests with GCC and Clang, downloadable CLI/UI/component/package artifacts, SHA-256 checksums, install smoke tests, and a dedicated Clang sanitizer job.

## Intentionally not part of the Linux target

The original Windows runtime assumes Win32, PE images, embedded Windows resources, DLLs, Direct3D 9 and Windows-specific process/runtime behavior. The Linux build does not attempt to reinterpret those artifacts as ELF objects or shared libraries.

The following areas therefore remain outside the native Linux target:

- `BUILD.ps1` and its PE/resource validation pipeline.
- `source/app.manifest`.
- The preserved Direct3D 9 and Win32 UI host in `source/ui`.
- PE/DLL assets such as `bootstrap-b1.dll` and `bootstrap-b2.dll`.
- Windows-only APIs used by `_native_main.cpp` and related runtime code.
- Windows-specific process/bootstrap/hook code.
- Windows trace-cleaning behavior and native-chain states that depend directly on the preserved Windows process implementation.

## Migration progress

### Platform/runtime foundation — implemented

Filesystem layout, validated settings, logging, workspace ownership/lifetime, diagnostics, testing, installation, distro-friendly packaging and CI are native Linux components with no Windows binary dependency.

### Safe game integration — implemented

The Linux target can locate Garry's Mod through normal Steam metadata and launch it through supported user-level Steam mechanisms. This restores the original application's benign "run Garry's Mod" behavior without process injection or PE/bootstrap assumptions.

### Linux in-process component — implemented

`libkirkware_component.so` and `ComponentSession` provide a validated load → initialize → poll → shutdown lifecycle inside the current process. This gives the Linux UI/runtime a native component boundary without translating the Windows remote-process/manual-map implementation.

### Configuration — partially implemented

The Linux runtime has its own ordinary text settings file and safe file primitives. The original Windows config sanitizer still depends on an embedded PE resource and Win32 file APIs, so it is not linked into Linux. Any reusable schema/data parsing should be separated from that Windows resource validation before reuse.

### UI — implemented as a native Linux host

`kirkware-ui` uses SDL2, OpenGL and the same vendored Dear ImGui presentation layer. The Linux implementation reproduces the Windows layout/style constants, borderless-window presentation, custom buttons/check boxes/group boxes/spinners, animated resizing, connecting/login/logging/home flow, and Garry's Mod tile while keeping the original `source/ui` files unchanged.

The Linux `load` button uses the supported in-process `ComponentSession`. The optional game launch uses normal Steam integration. The Windows-only `clean traces` behavior is deliberately not translated; Linux exposes `clean workspace`, which only removes inactive Kirkware-owned workspaces while respecting active workspace locks.

### Windows-specific native integrations — preserved

The existing Windows injection/manual-mapping/bootstrap/hook path remains the preserved Windows implementation. Linux portability work does not rewrite or substitute that code. Functionality that genuinely needs Linux operating-system or game integration should continue to be designed against documented Linux facilities and normal ELF/shared-library conventions.

## Design rules

- The Linux executables must remain independently buildable and testable without Windows binaries.
- New portable services belong behind platform-neutral interfaces or in `source/platform` when Linux-specific.
- Linux filesystem state should use XDG locations and normal POSIX ownership/locking semantics.
- Avoid adding a Windows compatibility layer merely to preserve PE/resource assumptions.
- Prefer documented OS/application interfaces to implementation-specific process machinery.
- Every new Linux subsystem should have a non-interactive test or health check where practical.
- The Windows runtime and UI remain protected by CI from incidental Linux-port edits.

See `docs/LINUX_ARCHITECTURE.md` for the current component boundaries and `docs/LINUX_COMPONENT_RUNTIME.md` for the Linux component ABI.
