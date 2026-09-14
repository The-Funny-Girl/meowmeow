# Linux build

The Linux port is intentionally separate from the original Windows build.

## Requirements

- Linux x86-64
- CMake 3.20 or newer
- GCC 11+ or Clang 14+ with C++20 support

## Build

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
```

The resulting executable is:

```text
build-linux/kirkware
```

## Scope

This target does not use the Windows PowerShell build, Windows SDK, Direct3D 9, Win32 resources, PE/DLL bootstrap assets, or Windows BCrypt API. The existing Windows build remains unchanged on `main`.

The Linux target currently provides the portable application shell and filesystem initialization. Windows-specific process/bootstrap/hooking components must be replaced with Linux-native implementations before they can be part of the Linux executable.
