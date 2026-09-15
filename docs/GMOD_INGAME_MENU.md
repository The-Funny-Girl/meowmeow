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
- **Visuals** — player/entity ESP, outlines, chams-style viewmodel tints, tracers, hit feedback
- **Misc** — movement, camera, spectator and HUD helpers
- **Players** — cycle each player through normal / ignore / priority / friend
- **Tuning** — FOV, smoothing, distances, freecam speed and tracer lifetime

Module state is saved to:

```text
garrysmod/data/kirkware_linux/modules.json
```

Player rules are saved to:

```text
garrysmod/data/kirkware_linux/players.json
```

Module hotkeys are saved to:

```text
garrysmod/data/kirkware_linux/binds.json
```

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

Important tuning convars include:

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

- names and 2D boxes
- health/armor bars
- distance, weapon and velocity
- skeleton
- team/usergroup/noclip information
- offscreen arrows
- supported entity names/distances/boxes/indexes
- through-world player halo outlines
- center crosshair
- target line
- hit marker / local hit sound
- first-person hand/weapon material tint
- local bullet tracers

## Misc / movement / camera

Current supported helpers include:

- third person
- normal FOV override
- zoom
- freecam
- bunnyhop helper
- mouse-direction auto-strafe
- auto pistol
- fast stop
- use spam
- spectator list
- enabled-module HUD list
- watermark

Additional tuning convars:

```text
kirkware_fov 100
kirkware_zoom_fov 40
kirkware_thirdperson_distance 110
kirkware_entity_distance 2500
kirkware_freecam_speed 650
kirkware_freecam_boost 3
kirkware_tracer_time 0.8
```

## Player rules

The Players page cycles a player through:

- `normal`
- `ignore`
- `priority`
- `friend`

Aim targeting never selects `ignore` or `friend`. `priority` receives target-selection preference when it is otherwise a valid target.

Console equivalents:

```text
kirkware_players
kirkware_player_rule <userid> <normal|ignore|priority|friend>
kirkware_player_rules_clear
```

## Module hotkeys

Insert is permanently reserved for the menu. Any registered module can otherwise be assigned a toggle key:

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
```

## Reset

```text
kirkware_reset_modules
```

restores every registered module to its default state. The Reset Defaults button in the menu does the same for module toggles.

## Compatibility boundary

This implementation deliberately stays on Garry's Mod's supported client Lua/addon interfaces. A game/server configuration that disables clientside Lua/addons can prevent this addon from loading. The Linux port does not attempt to bypass that policy or an anti-cheat system.

The migration intentionally excludes server crashers, malformed/net-channel attacks, fake latency/packet choking, tickbase/fake-command abuse, spread-seed manipulation, Windows process injection/manual mapping, remote/process hooks, trace cleaning, and stealth/evasion code.

## Architecture

The addon is loaded normally from Garry's Mod's `addons` directory and uses supported client hooks such as `HUDPaint`, `PreDrawHalos`, `CalcView`, `CreateMove`, `Think`, viewmodel drawing hooks, and normal game events. The protected Windows runtime remains unchanged.
