# Garry's Mod in-game menu (Linux)

The Linux port includes a client-side Garry's Mod addon that uses the game's supported Lua/addon interfaces. It does not use the preserved Windows injection, manual-mapping, or remote-hooking path.

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

The first Linux module set maps to existing Kirkware configuration names where practical:

- `esp_player_enable` — master player-visual switch
- `esp_player_name` — player names
- `esp_player_box` — simple 2D player boxes
- `esp_other_crosshair` — center crosshair
- `misc_thirdperson` — collision-aware third-person camera
- `menu_watermark` — small Kirkware Linux watermark

Changes are saved immediately. `kirkware_reset_modules` restores the module defaults.

The registry is intentionally small for the first supported Linux in-game implementation. Additional modules can be migrated onto the same registry without changing the Insert-key/menu architecture.

## Architecture boundary

The addon is loaded normally by Garry's Mod from its `addons` directory and uses standard client Lua hooks such as `HUDPaint`, `CalcView`, and `Think`. The protected Windows runtime remains unchanged.
