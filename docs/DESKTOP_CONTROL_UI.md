# Kirkware Linux desktop control panel

`kirkware-control-ui.py` is a pre-load/runtime configuration surface for the existing Kirkware Linux feature modules.

It does **not** replace or rewrite the feature implementations. Instead it edits the same module IDs and client convars already used by the supported Garry's Mod addon and exports them as a small line-based profile.

## Run

```bash
cd ~/Desktop/meowmeow-main
python3 kirkware-control-ui.py
```

If Tkinter is not installed on Linux Mint/Ubuntu:

```bash
sudo apt install python3-tk
```

## Tabs

- **Loader** — cooperative target PID, control root, module path, list/status/load/unload/stop, and a controlled test-target launcher.
- **Aim** — legit/rage module toggles, target policy, FOV/smoothing/distance/delay values, and target sorting.
- **Visuals** — player ESP, entity visuals, outlines/chams, crosshair, tracers and related display modules.
- **Misc** — third person, FOV/zoom, movement helpers, freecam, watermark and HUD options.
- **Config** — profile import/export, live export, local save/load and default reset.

## Shared profile

The control panel writes settings like:

```text
module.legit_enable=false
module.esp_player_enable=true
convar.kirkware_legit_fov=6
convar.kirkware_legit_smoothing=8
```

The default export target is the Garry's Mod data file:

```text
garrysmod/data/kirkware_linux/desktop_profile.conf
```

The export path is editable. A separate simulator can therefore watch the same profile from another directory or repository.

## Addon bridge

`kirkware_linux_zzzzzzz_desktop_profile.lua` loads after the existing module files and watches `data/kirkware_linux/desktop_profile.conf`.

When the profile changes it:

1. Applies known `module.<id>` values through `KIRKWARE_LINUX.SetModuleEnabled`.
2. Applies a fixed allowlist of existing `kirkware_*` client convars.
3. Leaves the actual combat, visual, movement, viewmodel and other feature implementations untouched.

The profile is checked every 0.75 seconds. It can also be applied manually in the client console:

```text
kirkware_desktop_profile_apply
```

## Cooperative loader boundary

The Loader tab uses `build-load-harness/kirkware-load-client`. A selected PID must still implement the Kirkware cooperative target protocol under the selected control root. The UI does not attach to unrelated processes or implement remote process injection.

## Self-test

The profile parser/serializer can be tested without opening the GUI:

```bash
python3 kirkware-control-ui.py --self-test
```
