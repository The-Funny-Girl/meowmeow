# Linux build

The Linux port is a native C++20 target and is intentionally isolated from the original Windows build/runtime.

## Requirements

- Linux x86-64
- CMake 3.20 or newer
- GCC 11+ or Clang 14+ with C++20 support
- Ninja is optional but recommended

## Build and test

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```

The executable is `build-linux/kirkware`.

The supplied presets are a shorter alternative when Ninja is installed:

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

For an ASan/UBSan development build with warnings treated as errors:

```sh
CC=clang CXX=clang++ cmake --preset linux-sanitize
cmake --build --preset linux-sanitize
ctest --preset linux-sanitize
```

## Runtime commands

```sh
./build-linux/kirkware --help
./build-linux/kirkware --paths
./build-linux/kirkware --settings
./build-linux/kirkware --game-info
./build-linux/kirkware --check
./build-linux/kirkware --version
```

`--game-info` searches common native Steam roots and `libraryfolders.vdf` entries for Garry's Mod app ID 4000 and reports the resolved library/install paths. `--launch-game` explicitly asks the normal Steam launcher (`steam -applaunch 4000`) to start the game, falling back to the desktop `steam://rungameid/4000` handler through `xdg-open`. This is ordinary launcher integration and does not use the Windows process/bootstrap chain.

`--no-workspace` skips temporary workspace creation for diagnostics. `--keep-workspace` overrides the persisted setting for the current invocation.

## Files and XDG locations

The Linux runtime follows the XDG Base Directory layout when the corresponding variables are set. Otherwise it uses conventional locations below `$HOME`.

- configuration: `$XDG_CONFIG_HOME/kirkware` or `~/.config/kirkware`
- data: `$XDG_DATA_HOME/kirkware` or `~/.local/share/kirkware`
- cache: `$XDG_CACHE_HOME/kirkware` or `~/.cache/kirkware`
- state/logs: `$XDG_STATE_HOME/kirkware` or `~/.local/state/kirkware`
- temporary workspaces: `$XDG_RUNTIME_DIR/kirkware/workspaces` or a per-user directory below the system temporary directory

XDG paths must be absolute. Application-owned directories are restricted to the current user where the filesystem supports POSIX permissions.

On first start the application writes `linux.conf` in the configuration root. Supported settings are:

```text
keep_workspace=false
cleanup_stale_workspaces=true
workspace_retention_hours=24
```

`workspace_retention_hours` accepts values from 1 through 720. Unknown or malformed settings are rejected instead of silently ignored. Active workspaces hold a file lock so stale cleanup cannot remove a running session.

## Install and package

Install into a local prefix with:

```sh
cmake --install build-linux --prefix "$HOME/.local"
```

This places `kirkware` below `$HOME/.local/bin` by default.

Create a portable `.tar.gz` package with:

```sh
cpack --config build-linux/CPackConfig.cmake -G TGZ -B packages
```

On Debian/Ubuntu/Linux Mint systems, create an installable `.deb` with:

```sh
cpack --config build-linux/CPackConfig.cmake -G DEB -B packages
sudo apt install ./packages/kirkware-linux_1.2.0_amd64.deb
```

The Debian package uses CPack's shared-library dependency scan to record the runtime libraries required by the compiled binary.

GitHub Actions builds and tests with GCC and Clang, performs an install smoke test, creates both package formats, validates Debian metadata, publishes SHA-256 checksums, and uploads the executable plus `.tar.gz` and `.deb` files as workflow artifacts. A separate sanitizer job runs Clang with ASan/UBSan.

## Scope of the current port

The Linux target provides native application startup, XDG path discovery, persistent validated settings, private directory creation, bounded file I/O, atomic text-file replacement, logging, locked temporary workspace lifecycle management, stale workspace cleanup, Steam/Garry's Mod discovery and normal launcher integration, runtime health checks, a command-line interface, automated tests, packaging, and CI builds.

It deliberately does not compile the Windows PowerShell build, Windows SDK code, Direct3D 9/Win32 UI backend, PE resource loader, Windows DLL/bootstrap assets, BCrypt dependency, or Windows process/hook/bootstrap chain. Those pieces are not portable by changing compiler flags and require separate Linux-native designs.

See `docs/LINUX_PORT_STATUS.md` and `docs/LINUX_ARCHITECTURE.md` for the current migration map and architecture boundary.
