# Linux build

The Linux port is a native C++20 target and is intentionally isolated from the original Windows build/runtime.

## Requirements

- Linux x86-64
- CMake 3.20 or newer
- GCC 11+ or Clang 14+ with C++20 support

Ninja is optional but recommended.

## Build

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
```

The executable is:

```text
build-linux/kirkware
```

Run the tests with:

```sh
ctest --test-dir build-linux --output-on-failure
```

## Useful commands

```sh
./build-linux/kirkware --help
./build-linux/kirkware --paths
./build-linux/kirkware --check
./build-linux/kirkware --version
```

The Linux runtime follows the XDG Base Directory layout when the corresponding environment variables are set. Otherwise it falls back to conventional locations below `$HOME`.

- configuration: `$XDG_CONFIG_HOME/kirkware` or `~/.config/kirkware`
- data: `$XDG_DATA_HOME/kirkware` or `~/.local/share/kirkware`
- cache: `$XDG_CACHE_HOME/kirkware` or `~/.cache/kirkware`
- state/logs: `$XDG_STATE_HOME/kirkware` or `~/.local/state/kirkware`
- temporary workspaces: `$XDG_RUNTIME_DIR/kirkware/workspaces` or a per-user directory below the system temporary directory

The created directories are restricted to the current user where the host filesystem supports POSIX permissions.

## Install

```sh
cmake --install build-linux --prefix "$HOME/.local"
```

This installs `kirkware` below `$HOME/.local/bin` by default.

## Scope of the current port

The Linux target now provides native application startup, XDG path discovery, private directory creation, bounded file I/O helpers, atomic text-file replacement, logging, temporary workspace lifecycle management, stale workspace cleanup, runtime health checks, a command-line interface, unit/integration tests, and CI builds.

It deliberately does not compile the Windows PowerShell build, Windows SDK code, Direct3D 9/Win32 UI backend, PE resource loader, Windows DLL/bootstrap assets, BCrypt dependency, or Windows process/hook/bootstrap chain. Those pieces are not portable by changing compiler flags and require separate Linux-native designs.

See `docs/LINUX_PORT_STATUS.md` for the current migration map.
