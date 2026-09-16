# Garry's Mod server-authorized multiplayer bridge

This workflow is for a Garry's Mod server you control. It does **not** attach to a
running process and it does not bypass server addon/client-Lua policy.

The private server installs a small bridge addon. At startup the bridge uses
`AddCSLuaFile` to send the existing Kirkware client Lua to players who join that
server. The Linux client keeps the native module installed locally at:

```text
garrysmod/lua/bin/gmcl_kirkware_native_linux64.dll
```

The server never receives or distributes the native binary. The client payload's
normal native bridge calls `require("kirkware_native")` after the server-authorized
Lua state initializes.

## 1. Prepare the Linux client

Build and install the native module before starting Garry's Mod:

```bash
cd ~/Desktop/meowmeow-main
python3 kirkware.py native deps
python3 kirkware.py native build --clean
python3 kirkware.py native install
```

The local folder addon is not required for the server-delivery test. The server
bridge sends the feature Lua when the client joins the controlled server.

## 2. Install the bridge on the controlled server

The server root is the directory that contains its `garrysmod/` folder.

```bash
python3 kirkware-server-bridge.py install \
  --server-root /path/to/your/gmod-server
```

Check the generated bridge:

```bash
python3 kirkware-server-bridge.py status \
  --server-root /path/to/your/gmod-server
```

The installer creates:

```text
garrysmod/addons/kirkware_linux_server_bridge/
├── lua/
│   ├── autorun/
│   │   └── kirkware_linux_server_bridge.lua
│   └── kirkware_linux/
│       └── client/
│           └── ...existing Kirkware client files...
```

## 3. Start the server and join normally

The server console should print a bridge-ready line during startup. When a client
finishes initialization it reports a small status handshake back to the server,
including whether the local native module loaded and its version.

Expected server-side line:

```text
[kirkware server bridge] client ready: <name> [<steamid64>] native=yes version=<version>
```

The server console command:

```text
kirkware_server_bridge_status
```

prints the clients that have acknowledged the bridge.

## 4. Remove the bridge

```bash
python3 kirkware-server-bridge.py remove \
  --server-root /path/to/your/gmod-server
```

## Why this works for multiplayer testing

The feature Lua originates from the server you control, so the test runs in an
actual multiplayer client state rather than a single-player-only local-addon path.
The native module remains a normal GMod client binary module loaded through
`require()`. This keeps server authorization explicit and avoids remote process
injection, ptrace, manual mapping, or memory-writing techniques.
