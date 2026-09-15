# Garry's Mod in-game menu (Linux)

The Linux port includes a client-side Garry's Mod addon that uses the game's supported Lua/addon interfaces. It does not use the preserved Windows injection, manual-mapping, remote-hooking, anti-cheat-bypass, or network-exploit path.

For the detailed Windows-to-Linux feature checklist, see [LINUX_FEATURE_PARITY.md](LINUX_FEATURE_PARITY.md).

## Install or update

From the repository root:

```sh
./build-linux.sh --install-addon
```

If automatic Garry's Mod discovery is not possible:

```sh
./build-linux.sh --install-addon --game-dir "/path/to/steamapps/common/GarrysMod"
```

The standalone installer can update the addon without rebuilding:

```sh
./install-gmod-addon.sh
```

It only creates/replaces `garrysmod/addons/kirkware_linux`. Restart Garry's Mod after installing or updating the addon.

## Menu

Press **Insert** to open/close the in-game menu. `kirkware_menu` toggles the same menu from the console.

The menu is divided into:

- **Aim** — legit/rage targeting, hitscan, triggerbot, recoil and target controls
- **Visuals** — player/entity ESP, outlines, viewmodel tints, tracers, hit feedback and crosshair options
- **Misc** — movement, camera, pathfinder, notifications and HUD helpers
- **Players** — cycle each player through normal / ignore / priority / friend
- **Hotkeys** — assign persistent toggle keys to registered modules
- **Tuning** — FOV, smoothing, distances, freecam speed and tracer lifetime

Module state is saved to `garrysmod/data/kirkware_linux/modules.json`, player rules to `players.json`, and module hotkeys to `binds.json`.

## Aim modules

Current supported aim modules include:

- `legit_enable`
- `legit_fov_circle`
- `legit_hitscan`
- `legit_triggerbot`
- `legit_recoil`
- `legit_visible_check`
- `rage_enable`
- `rage_autofire`
- `rage_fov_circle`
- `rage_hitscan`
- `rage_norecoil`
- `rage_target_lock`
- `rage_visible_check`
- `esp_target_line`

Important tuning convars:

```text
kirkware_legit_fov 6
kirkware_legit_smoothing 8
kirkware_legit_max_distance 10000
kirkware_trigger_delay 0.03
kirkware_rage_fov 35
kirkware_rage_max_distance 20000
kirkware_aim_teammates 0
kirkware_legit_require_attack 1
```

## Visual modules

Player/entity/viewmodel features include:

- player names and 2D boxes
- health/armor bars and optional numeric values
- distance, weapon and velocity
- skeleton
- team/usergroup/noclip/cloaked information
- offscreen arrows
- supported entity names/distances/boxes/indexes
- through-world player halo outlines
- rule-aware priority/friend/ignore outline colors
- center crosshair with outline/rainbow/size/gap/thickness controls
- team-colored ESP option
- optional Steam profile names
- anonymous ESP-name mode
- target line
- hit marker / local hit sound
- first-person hand/weapon material tint
- local bullet tracers

Additional visual convars:

```text
kirkware_crosshair_size 7
kirkware_crosshair_gap 2
kirkware_crosshair_thickness 1
kirkware_crosshair_rainbow_speed 90
kirkware_tracer_time 0.8
```

## Misc / movement / camera

Current supported helpers include:

- third person
- normal FOV override
- local viewmodel FOV override with restoration on disable
- zoom
- freecam
- bunnyhop helper
- mouse-direction auto-strafe
- auto pistol
- fast stop
- use spam
- spectator list
- enabled-module HUD list
- local join/leave notices that respect anonymous mode
- watermark

Additional tuning convars:

```text
kirkware_fov 100
kirkware_zoom_fov 40
kirkware_viewmodel_fov 54
kirkware_thirdperson_distance 110
kirkware_entity_distance 2500
kirkware_freecam_speed 650
kirkware_freecam_boost 3
```

## Pathfinder

The supported pathfinder consumes an existing map navmesh. It does not generate, edit or save navmesh data.

Modules include:

- `misc_pathfinder_enable`
- `misc_pathfinder_set_target`
- `misc_pathfinder_clear_target`
- `misc_pathfinder_walk_path`
- `misc_pathfinder_visualize`
- `misc_pathfinder_aim`
- `misc_pathfinder_jump`
- `misc_pathfinder_crouch`
- `misc_pathfinder_run`

`set path target` is a one-shot action: it uses the world point under the crosshair, computes an A* route across connected nav areas, and enables path walking on success. Because it is a normal module action, it can also be assigned a key from the Hotkeys page.

Console equivalents:

```text
kirkware_path_set
kirkware_path_clear
kirkware_path_status
```

Tuning:

```text
kirkware_path_tolerance 45
kirkware_path_max_areas 4096
```

Maps without a loaded navmesh cannot use this feature.

## Player rules

The Players page cycles a player through:

- `normal`
- `ignore`
- `priority`
- `friend`

Aim targeting never selects `ignore` or `friend`. `priority` receives target-selection preference when it is otherwise a valid target. Player outlines also use rule-aware colors.

Console equivalents:

```text
kirkware_players
kirkware_player_rule <userid> <normal|ignore|priority|friend>
kirkware_player_rules_clear
```

## Module hotkeys

Insert is permanently reserved for the menu. Any registered module can otherwise be assigned a toggle key from the **Hotkeys** page or console:

```text
kirkware_bind <module_id> <key name>
kirkware_bind <module_id> none
kirkware_binds
kirkware_binds_clear
```

Examples:

```text
kirkware_bind misc_freecam f6
kirkware_bind misc_thirdperson mouse3
kirkware_bind misc_zoom z
kirkware_bind misc_pathfinder_set_target p
```

## Reset

```text
kirkware_reset_modules
```

restores every registered module to its default state. The Reset Defaults button in the menu does the same for module toggles.

## Compatibility boundary

This implementation deliberately stays on Garry's Mod's supported client Lua/addon interfaces. A game/server configuration that disables clientside Lua/addons can prevent this addon from loading. The Linux port does not attempt to bypass that policy or an anti-cheat system.

The migration intentionally excludes server crashers, malformed/net-channel attacks, fake latency/packet choking, tickbase/fake-command abuse, spread-seed manipulation, Windows process injection/manual mapping, remote/process hooks, trace cleaning, screengrab bypasses and stealth/evasion code. Engine settings marked cheat-protected, such as direct aspect-ratio overrides, are also not bypassed.

## Architecture

The addon is loaded normally from Garry's Mod's `addons` directory and uses supported client hooks such as `HUDPaint`, `PreDrawHalos`, `CalcView`, `CreateMove`, `Think`, viewmodel drawing hooks, normal game events and the existing map navmesh APIs. The protected Windows runtime remains unchanged.
