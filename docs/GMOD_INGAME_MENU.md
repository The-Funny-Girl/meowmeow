# Garry's Mod in-game menu (Linux)

The Linux port includes a client-side Garry's Mod addon that uses the game's supported Lua/addon interfaces. It does not use the preserved Windows injection, manual-mapping, remote-hooking, anti-cheat-bypass, or network-exploit path.

## Install or update

From the repository root:

```sh
./build-linux.sh --install-addon
```

The helper first builds/tests the Linux port and then discovers the Garry's Mod install through the Linux runtime. If automatic discovery is not possible, provide the install directory explicitly:

```sh
./build-linux.sh --install-addon --game-dir "/path/to/steamapps/common/GarrysMod"
```

The standalone installer can also be used without rebuilding:

```sh
./install-gmod-addon.sh
```

It only creates/replaces `garrysmod/addons/kirkware_linux`. No other addon directory is modified.

To remove it:

```sh
./install-gmod-addon.sh --uninstall
```

Restart Garry's Mod after installing or updating the addon.

## Menu key

Press **Insert** in-game to open or close the menu. The key uses edge-triggered input so holding Insert does not repeatedly toggle the window.

The console command below opens/closes the same menu:

```text
kirkware_menu
```

## Module registry

Module state is stored client-side in:

```text
garrysmod/data/kirkware_linux/modules.json
```

The current Linux module set maps to existing Kirkware configuration names where practical.

### Visuals

- `esp_player_enable` — master player-visual switch
- `esp_player_name` — player names
- `esp_player_box` — simple 2D player boxes
- `esp_player_hpbar` — health bars
- `esp_player_arbar` — armor bars
- `esp_player_distance` — player distance
- `esp_player_weapon` — active weapon class
- `esp_player_skeleton` — model-bone skeleton
- `esp_player_velocity` — movement speed
- `esp_other_crosshair` — center crosshair
- `chams_enable` — through-world player outline using the supported halo renderer
- `entities_enable` — master nearby-entity switch
- `entities_name` — nearby entity class names
- `entities_distance` — nearby entity distance

### Misc / movement / camera

- `misc_thirdperson` — collision-aware third-person camera
- `misc_fov_changer` — configurable normal camera FOV
- `misc_zoom` — configurable zoom FOV
- `misc_bunnyhop` — held-jump retrigger helper
- `misc_auto_strafe` — mouse-direction air strafe input
- `misc_auto_pistol` — alternating held primary attack input
- `menu_spectators` — current spectator list
- `menu_watermark` — small Kirkware Linux watermark

Changes are saved automatically. `kirkware_reset_modules` restores module defaults.

Useful client convars:

```text
kirkware_fov 100
kirkware_zoom_fov 40
kirkware_thirdperson_distance 110
kirkware_entity_distance 2500
```

These are archived client convars, so their numeric values persist between sessions.

## Compatibility boundary

This implementation deliberately stays on Garry's Mod's supported client Lua/addon interfaces. Servers can disable clientside addons with `sv_allowcslua 0`; when they do, this addon is not expected to run there. The Linux port does not try to bypass that server setting or an anti-cheat system.

The current migration excludes the old network/server-crasher group, packet/tickbase abuse, trace-cleaning/evasion behavior, process injection, manual mapping, and remote hooks.

Combat-targeting modules are being kept separate from the visual/movement layer so they can be validated without destabilizing movement prediction or the Insert menu.

## Architecture boundary

The addon is loaded normally by Garry's Mod from its `addons` directory and uses standard client Lua hooks such as `HUDPaint`, `PreDrawHalos`, `CalcView`, `CreateMove`, and `Think`. The protected Windows runtime remains unchanged.
