# Live private Garry's Mod test

This workflow exercises the supported Kirkware Linux addon in a real Garry's Mod client. It is intentionally separate from the cooperative `.so` load harness and does not use remote process injection.

## Why this exists

The normal folder addon can be installed directly under `garrysmod/addons/kirkware_linux`; Workshop publication is not required for local development. For a controlled test, the launcher temporarily disables Workshop content by default so unrelated Workshop addons cannot mask or conflict with Kirkware while the feature code is being verified.

## Private live test

```bash
./launch-gmod-private-test.sh
```

The launcher:

1. installs/updates the local Kirkware addon;
2. rotates an earlier `garrysmod/console.txt` if present;
3. starts Garry's Mod with `-condebug`, `-console`, `-disableluarefresh`, and `-noworkshop`;
4. starts a local Sandbox listen-server on `gm_construct` with `sv_lan 1` and `sv_allowcslua 1`;
5. leaves the desktop profile bridge active so `kirkware-control-ui.py` can change the live module/convar values.

Use another map with:

```bash
./launch-gmod-private-test.sh --map gm_flatgrass
```

## Pre-join menu mode

To initialize the local client/addon before connecting anywhere:

```bash
./launch-gmod-private-test.sh --menu
```

This starts GMod without automatically joining or creating a server. Workshop content is still disabled by default for isolation. This is the supported pre-join path for checking initialization order and addon conflicts.

## Re-enable Workshop content

After the isolated test works:

```bash
./launch-gmod-private-test.sh --keep-workshop
```

If this fails while the default isolated launch works, a Workshop addon conflict is likely and can be narrowed down separately.

## Verify the running client

After the map/menu loads:

```bash
./verify-gmod-live-test.sh
```

The verifier reads the current `garrysmod/console.txt` created by `-condebug` and checks that the following real client modules reported initialization:

- base Kirkware client addon;
- combat/aim module;
- player visual module;
- viewmodel/tracer module;
- desktop profile bridge.

It also displays recent Kirkware log lines and potential Kirkware-related Lua errors.

## Desktop control panel

In another terminal:

```bash
python3 kirkware-control-ui.py
```

Leave **Live export** enabled. The desktop profile bridge watches `garrysmod/data/kirkware_linux/desktop_profile.conf` and applies supported module booleans and existing Kirkware convars to the running addon. This makes changes to the Aim, Visuals, Misc and Config pages affect the real private GMod session rather than only a simulator or unit test.

## Useful options

```text
--local              start the private listen-server test (default)
--menu               stop at the main menu; do not join a server
--map NAME           choose the local test map
--keep-workshop      allow Workshop addons during the test
--no-install         do not reinstall the folder addon
--game-dir PATH      override the GMod installation directory
--steam-bin PATH     override the Steam executable
--dry-run            print the command without starting GMod
```

## Scope

This workflow is for a local/private environment you control. It deliberately does not implement arbitrary-process injection, remote memory writes, manual mapping, server-policy bypasses, or anti-cheat evasion.
