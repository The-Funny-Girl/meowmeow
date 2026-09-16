#!/usr/bin/env python3
"""Install the server-authorized Kirkware multiplayer bridge.

This does not attach to or modify a running process. A Garry's Mod server controlled
by the tester sends the existing Kirkware client Lua with AddCSLuaFile; the client's
native module must already be installed locally in garrysmod/lua/bin/.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
CLIENT_SOURCE = ROOT / "source/gmod_addon/kirkware_linux/lua/autorun/client"
TEMPLATE = ROOT / "source/gmod_server_bridge/kirkware_linux_server_bridge.lua.in"
ADDON_NAME = "kirkware_linux_server_bridge"
BOOTSTRAP_RELATIVE = Path("lua/autorun/kirkware_linux_server_bridge.lua")
PAYLOAD_ROOT_RELATIVE = Path("lua/kirkware_linux/client")
MARKER = "    -- KIRKWARE_PAYLOADS"


class BridgeError(RuntimeError):
    pass


def fail(message: str) -> "NoReturn":
    raise BridgeError(message)


def validate_server_root(value: Path) -> Path:
    root = value.expanduser().resolve()
    if not root.is_dir():
        fail(f"server root is not a directory: {root}")
    if not (root / "garrysmod").is_dir():
        fail(f"server root is missing garrysmod/: {root}")
    return root


def payload_sources() -> list[Path]:
    if not CLIENT_SOURCE.is_dir():
        fail(f"Kirkware client source is missing: {CLIENT_SOURCE}")
    files = sorted(path for path in CLIENT_SOURCE.glob("*.lua") if path.is_file())
    if not files:
        fail(f"no Kirkware client Lua files found in {CLIENT_SOURCE}")
    if not any(path.name == "kirkware_linux.lua" for path in files):
        fail("base client file kirkware_linux.lua is missing")
    return files


def render_bootstrap(payloads: list[Path]) -> str:
    try:
        template = TEMPLATE.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"cannot read server bridge template {TEMPLATE}: {exc}")
    if MARKER not in template:
        fail(f"server bridge template marker is missing: {MARKER!r}")
    entries = "\n".join(
        f'    "kirkware_linux/client/{path.name}",' for path in payloads
    )
    return template.replace(MARKER, entries, 1)


def addon_target(server_root: Path) -> Path:
    return server_root / "garrysmod/addons" / ADDON_NAME


def install_bridge(server_root: Path) -> Path:
    root = validate_server_root(server_root)
    payloads = payload_sources()
    addons = root / "garrysmod/addons"
    addons.mkdir(parents=True, exist_ok=True)
    target = addon_target(root)
    stage = addons / f".{ADDON_NAME}.stage"
    shutil.rmtree(stage, ignore_errors=True)

    payload_dir = stage / PAYLOAD_ROOT_RELATIVE
    payload_dir.mkdir(parents=True, exist_ok=True)
    for source in payloads:
        shutil.copy2(source, payload_dir / source.name)

    bootstrap = stage / BOOTSTRAP_RELATIVE
    bootstrap.parent.mkdir(parents=True, exist_ok=True)
    bootstrap.write_text(render_bootstrap(payloads), encoding="utf-8")

    shutil.rmtree(target, ignore_errors=True)
    stage.replace(target)

    print(f"Installed server-authorized bridge: {target}")
    print(f"Client payload files: {len(payloads)}")
    print("Client native module is not copied to the server.")
    print("Install it on the Linux client with: python3 kirkware.py native install")
    return target


def bridge_status(server_root: Path) -> int:
    root = validate_server_root(server_root)
    target = addon_target(root)
    bootstrap = target / BOOTSTRAP_RELATIVE
    payload_dir = target / PAYLOAD_ROOT_RELATIVE
    installed_payloads = sorted(payload_dir.glob("*.lua")) if payload_dir.is_dir() else []
    expected = payload_sources()

    print(f"Server root: {root}")
    print(f"Bridge:      {target} ({'installed' if target.is_dir() else 'missing'})")
    print(f"Bootstrap:   {bootstrap} ({'yes' if bootstrap.is_file() else 'no'})")
    print(f"Payloads:    {len(installed_payloads)}/{len(expected)}")
    return 0 if bootstrap.is_file() and len(installed_payloads) == len(expected) else 1


def remove_bridge(server_root: Path) -> None:
    root = validate_server_root(server_root)
    target = addon_target(root)
    if target.is_dir():
        shutil.rmtree(target)
        print(f"Removed server bridge: {target}")
    else:
        print(f"Server bridge is not installed: {target}")


def self_test() -> int:
    payloads = payload_sources()
    rendered = render_bootstrap(payloads)
    if MARKER in rendered:
        print("bootstrap render marker was not replaced", file=sys.stderr)
        return 1
    for source in payloads:
        expected = f'"kirkware_linux/client/{source.name}"'
        if expected not in rendered:
            print(f"bootstrap manifest is missing {source.name}", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "garrysmod").mkdir()
        target = install_bridge(root)
        copied = sorted((target / PAYLOAD_ROOT_RELATIVE).glob("*.lua"))
        if len(copied) != len(payloads):
            print("server bridge copied the wrong number of payload files", file=sys.stderr)
            return 1
        if not (target / BOOTSTRAP_RELATIVE).is_file():
            print("server bridge bootstrap was not created", file=sys.stderr)
            return 1
        if bridge_status(root) != 0:
            print("server bridge status self-test failed", file=sys.stderr)
            return 1
        remove_bridge(root)
        if target.exists():
            print("server bridge remove self-test failed", file=sys.stderr)
            return 1

    print("server bridge self-test passed")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Kirkware server-authorized multiplayer bridge")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("self-test", help="run local packaging self-tests")
    for action in ("install", "status", "remove"):
        child = sub.add_parser(action)
        child.add_argument("--server-root", type=Path, required=True,
                           help="Garry's Mod server root containing garrysmod/")
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        if args.command == "self-test":
            return self_test()
        if args.command == "install":
            install_bridge(args.server_root)
            return 0
        if args.command == "status":
            return bridge_status(args.server_root)
        if args.command == "remove":
            remove_bridge(args.server_root)
            return 0
        return 0
    except BridgeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nInterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
