# Unified Python manager

`kirkware.py` is the primary Linux entry point for Kirkware. The older shell
scripts remain available as compatibility fallbacks, but the Python manager can
perform the build, cooperative loader, Garry's Mod launch, addon installation,
native-module build/install, live verification, and feature-control workflows in
one place.

## Interactive use

```bash
python3 kirkware.py
```

The top-level menu contains:

- main Release build/test;
- cooperative load harness;
- supported Garry's Mod native module;
- folder-addon installation;
- isolated private `gm_construct` launch;
- main-menu launch;
- validated GMod install/process information;
- Aim/Visuals/Misc desktop control UI;
- live initialization verification.

## GMod-only validation

The manager does not expose a generic executable picker for its GMod workflow.
It resolves a Steam installation containing `appmanifest_4000.acf`, verifies that
the manifest identifies AppID `4000`, and verifies that the selected install is
that manifest's `steamapps/common/<installdir>` directory.

When showing running GMod processes, it only reports processes whose
`/proc/<pid>/exe` resolves inside that validated installation. Current Linux
executable candidates include `bin/linux64/gmod`, `hl2_linux`, and `hl2.sh`.

This validation is intentionally separate from the cooperative loader. A typed
PID still has to implement the Kirkware cooperative control protocol before the
loader client will send it a load request.

## Native Linux GMod module

Garry's Mod expects Linux x86-64 client binary modules under
`garrysmod/lua/bin/` using the filename form:

```text
gmcl_<name>_linux64.dll
```

Despite the `.dll` suffix, Linux modules are ELF shared objects. Kirkware builds:

```text
gmcl_kirkware_native_linux64.dll
```

and the client addon loads it through:

```lua
require("kirkware_native")
```

The initial native module intentionally exposes only metadata (`version`,
`platform`, `abi`, and `loaded`). It establishes and tests the supported native
module path without adding remote process attachment or memory modification.

### Build and install

```bash
python3 kirkware.py native deps
python3 kirkware.py native build --clean
python3 kirkware.py native install
```

The first command places `garrysmod_common` under `.deps/garrysmod_common` when
it is not already present. A different checkout can be supplied with
`--gmcommon PATH`.

## Main build

```bash
python3 kirkware.py build
python3 kirkware.py build --clean
python3 kirkware.py build --sanitize
```

## Cooperative loader

```bash
python3 kirkware.py harness-build --clean
python3 kirkware.py loader --start-target --control-root ~/Downloads
python3 kirkware.py loader --list --control-root ~/Downloads
python3 kirkware.py loader --pid 12345 --status --control-root ~/Downloads
python3 kirkware.py loader --pid 12345 --load build-load-harness/libkirkware_load_test.so --control-root ~/Downloads
```

Manual PID selection is preserved. The selected process must still have its
same-user control directory under the selected control root.

## Real private GMod session

```bash
python3 kirkware.py gmod info
python3 kirkware.py native build
python3 kirkware.py gmod launch
```

The launch command installs/updates the folder addon, installs the native module
when it has been built, disables Workshop content by default for isolation, and
starts a private Sandbox session on `gm_construct` with local client Lua allowed.

To stop at the main menu instead:

```bash
python3 kirkware.py gmod launch --menu
```

To retain Workshop content:

```bash
python3 kirkware.py gmod launch --keep-workshop
```

## Verification

```bash
python3 kirkware.py gmod verify
python3 kirkware.py gmod verify --require-native
```

Verification reads the actual GMod `console.txt`. It proves that expected
modules initialized; it does not by itself prove gameplay behavior.
