# Linux client feature parity

This document tracks the supported Garry's Mod client feature port on the
`linux-ingame-menu` branch. The Linux implementation uses normal Garry's Mod
client Lua/addon APIs. It does **not** translate the preserved Windows process
injection/manual-map/hook loader or anti-cheat/server-setting bypass behavior.

## Menu and configuration

Implemented:

- Insert opens/closes the in-game menu.
- Dynamic Aim, Visuals, Misc, Players, and Tuning pages.
- Module state persists in `garrysmod/data/kirkware_linux/modules.json`.
- Per-player state persists in `garrysmod/data/kirkware_linux/players.json`.
- Per-module hotkeys persist in `garrysmod/data/kirkware_linux/binds.json`.
- Reset-to-defaults command/button.
- Generic module bind commands (`kirkware_bind`, `kirkware_binds`).
- Player normal/ignore/priority/friend rules.

## Aim / combat

Implemented through the normal client `CreateMove` path:

- Legit smooth aim.
- Configurable legit FOV and smoothing.
- Legit head/chest/pelvis hitscan.
- Legit visibility filtering.
- Legit recoil compensation.
- Triggerbot with configurable delay.
- Rage target selection.
- Configurable rage FOV.
- Rage head/chest/pelvis hitscan.
- Rage visibility filtering.
- Rage autofire.
- Rage recoil compensation.
- Rage target lock.
- Team filtering.
- Per-player ignore/friend/priority targeting rules.
- Legit/rage FOV circles.
- Current-target line.

Not translated in this supported path:

- spread-seed manipulation / generic no-spread
- fake command/tickbase shifting
- fake latency/fakelag
- backtrack that depends on command/tick manipulation
- fake-angle/network anti-aim
- rapid-fire/tickbase exploits

Those behaviors depend on prediction/network manipulation rather than the
normal supported client-addon surface used by this port.

## Player visuals

Implemented:

- master player visuals switch
- names
- boxes
- health bar
- armor bar
- distance
- active weapon class
- velocity
- skeleton
- team information
- usergroup information
- noclip flag
- off-screen direction arrows
- player outlines through world geometry using the halo renderer
- hit marker
- local hit sound
- current target line
- spectator list

The Linux implementation uses supported HUD/halo drawing instead of translating
Windows render hooks or material hooks installed into the game process.

## Entity visuals

Implemented:

- entity visual master switch
- supported prop/weapon/NPC/scripted/GMod entity classes
- entity class name
- distance
- screen-space box
- entity index
- configurable render distance

## Viewmodel / other visuals

Implemented:

- custom center crosshair
- first-person weapon material tint
- first-person hands material tint
- local bullet tracers with configurable lifetime
- watermark
- enabled-module HUD list

## Camera and movement

Implemented:

- third person with collision trace and configurable distance
- normal camera FOV override
- zoom FOV
- freecam with configurable speed/boost
- bunnyhop helper
- mouse-direction air-strafe helper
- auto-pistol input helper
- fast-stop helper
- use-spam helper

## Player list

Implemented:

- normal / ignore / priority / friend states
- persisted by SteamID64 when available
- Players page in the Insert menu
- priority affects target selection
- ignore/friend are excluded from supported aim targeting
- console listing and rule-management commands

## Intentionally excluded Windows functionality

The following Windows functionality is preserved in the Windows source but is
not being ported into the Linux supported-addon path:

- remote process injection
- manual mapping / PE loading into another process
- remote/process hooks
- hook stealth or fingerprint evasion
- trace cleaning / evidence removal
- anti-cheat bypasses
- screengrab bypasses
- client-Lua/server-setting bypasses
- destructive server crashers
- malformed/net-channel crash or freeze behavior
- packet delimiter abuse
- script dumping/protection bypass
- fake latency / packet choking
- tickbase shifting/fake-command abuse
- other features whose implementation requires unsupported network/prediction
  manipulation rather than documented client addon APIs

## Compatibility boundary

This addon is intentionally a normal client addon. A server or game setup that
disables clientside Lua/addons can prevent the addon from loading. The Linux
port does not attempt to bypass that policy.

Some Windows features depended directly on Source-engine internals reached via
in-process hooks. Where a documented client Lua equivalent exists, the Linux
port implements the user-facing behavior through that supported interface.
Where no equivalent exists without injection, hooks, evasion, or network abuse,
the feature remains excluded instead of being represented by a misleading
checkbox.

## Validation

GitHub Actions includes a focused Garry's Mod addon workflow that:

- validates Bash helper syntax
- parses every addon Lua file
- installs the addon into a fake Garry's Mod tree
- verifies expected addon files
- uninstalls it cleanly

The normal Linux workflow independently protects the preserved Windows runtime
and builds/tests the native Linux CLI/component/UI with GCC, Clang, and
sanitizers.

Runtime gameplay behavior still needs real Garry's Mod testing because GitHub
Actions does not boot a full interactive game client.
