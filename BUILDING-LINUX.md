# Linux build

The Linux port is a native C++20 target and is intentionally isolated from the original Windows build/runtime. It now includes both the command-line runtime and a native graphical UI that mirrors the existing Windows presentation while using Linux-safe runtime integrations.

## Requirements

- Linux x86-64
- CMake 3.20 or newer
- GCC 11+ or Clang 14+ with C++20 support
- SDL2 development files
- SDL2_image development files
- OpenGL development files
- FreeType development files
- Ninja is optional but recommended

On Debian, Ubuntu, or Linux Mint the UI dependencies can be installed with:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config \
  libsdl2-dev libsdl2-image-dev libgl1-mesa-dev libfreetype6-dev
```

## Build and test

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```

The build produces:

- `build-linux/kirkware` — command-line Linux runtime
- `build-linux/kirkware-ui` — native graphical Linux UI
- `build-linux/libkirkware_component.so` — in-process Linux component

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

The UI can be disabled for a headless/runtime-only build with `-DKIRKWARE_BUILD_UI=OFF`.

## Graphical UI

Start the Linux UI with:

```sh
./build-linux/kirkware-ui
```

The Linux host uses SDL2 and OpenGL while retaining the dimensions, dark theme, color table, custom controls, animated resizing, login/home transitions, Garry's Mod tile, and borderless-window presentation of the Windows UI.

The UI looks for `libkirkware_component.so` next to the executable and in normal installed library locations. A development component can be selected explicitly with:

```sh
./build-linux/kirkware-ui --component ./build-linux/libkirkware_component.so
```

`KIRKWARE_COMPONENT_PATH` can also select the component. `KIRKWARE_ASSET_DIR` can override the asset directory. `KIRKWARE_UI_FONT` and `KIRKWARE_UI_FONT_BOLD` can override the regular and bold fonts. The host tries Tahoma when installed, then common Liberation Sans and DejaVu Sans fallbacks.

The Linux `load` control uses the supported in-process `ComponentSession` lifecycle. It does not invoke the preserved Windows process-injection/manual-mapping path. The `run garry's mod` option uses the normal Steam launcher integration. The Windows-only trace-cleaning option is not reproduced on Linux; the corresponding Linux control is `clean workspace`, which only removes inactive Kirkware-owned workspace directories while respecting active workspace locks.

For CI and display-server diagnostics, `kirkware-ui --smoke-test` creates and renders a hidden UI for a small number of frames and exits.

## Runtime commands

```sh
./build-linux/kirkware --help
./build-linux/kirkware --paths
./build-linux/kirkware --settings
./build-linux/kirkware --game-info
./build-linux/kirkware --check
./build-linux/kirkware --version
./build-linux/kirkware --component-self-test ./build-linux/libkirkware_component.so
```

`--component-self-test` loads the Linux shared component into the current process through `dlopen`, validates ABI version 1, polls its heartbeat/integrity status, and shuts it down cleanly. CTest runs the same load/initialize/poll/shutdown path automatically.

`--game-info` searches common native Steam roots and `libraryfolders.vdf` entries for Garry's Mod app ID 4000 and reports the resolved library/install paths. `--launch-game` explicitly asks the normal Steam launcher (`steam -applaunch 4000`) to start the game, falling back to the desktop `steam://rungameid/4000` handler through `xdg-open`.

`--no-workspace` skips temporary workspace creation for diagnostics. `--keep-workspace` overrides the persisted setting for the current invocation.

## Linux in-process component

`libkirkware_component.so` is a native Linux shared component with a stable C ABI. Its current status snapshot reports an in-process heartbeat, Linux `TracerPid`, file-backed mapping count, ABI version, and status detail.

The host-facing `ComponentSession` layer owns the lifecycle and validates the ABI before accepting a component. A protected application/game integration should load the shared component through a supported cooperating module/plugin/startup path and then use the same ABI lifecycle. The Linux runtime does not remotely attach to another process, write process memory, manually map an ELF object, or install remote hooks.

See `docs/LINUX_COMPONENT_RUNTIME.md` for the ABI and lifecycle contract.

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

This installs `kirkware` and `kirkware-ui` below `$HOME/.local/bin`, the Linux component below `$HOME/.local/lib/kirkware`, and the Garry's Mod UI image below `$HOME/.local/share/kirkware` by default.

Create a portable `.tar.gz` package with:

```sh
cpack --config build-linux/CPackConfig.cmake -G TGZ -B packages
```

On Debian/Ubuntu/Linux Mint systems, create an installable `.deb` with:

```sh
cpack --config build-linux/CPackConfig.cmake -G DEB -B packages
sudo apt install ./packages/kirkware-linux_1.5.0_amd64.deb
```

The Debian package uses CPack's shared-library dependency scan to record the runtime libraries required by the compiled binaries and component.

GitHub Actions builds and tests with GCC and Clang, performs an install smoke test, renders the UI under Xvfb/llvmpipe, creates both package formats, validates Debian metadata, publishes SHA-256 checksums, and uploads the CLI, UI, component, `.tar.gz`, and `.deb` files as workflow artifacts. A separate sanitizer job runs Clang with ASan/UBSan.

## Scope of the current port

The Linux target provides native application startup, the Windows-style graphical UI, XDG path discovery, persistent validated settings, private directory creation, bounded file I/O, atomic text-file replacement, logging, locked temporary workspace lifecycle management, stale workspace cleanup, Steam/Garry's Mod discovery and normal launcher integration, a loadable in-process Linux component with a stable ABI, runtime health checks, a command-line interface, automated tests, packaging, and CI builds.

The original Windows process/bootstrap implementation and Windows UI remain preserved and isolated. The Linux UI reproduces presentation and benign interaction flow through native Linux interfaces while the component uses a cooperating in-process load path rather than translating remote process injection/manual mapping behavior.

See `docs/LINUX_PORT_STATUS.md`, `docs/LINUX_ARCHITECTURE.md`, and `docs/LINUX_COMPONENT_RUNTIME.md` for the current migration map and architecture boundary.
