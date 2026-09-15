#!/usr/bin/env python3
"""Unified Kirkware Linux build, loader, GMod and native-module manager.

The cooperative loader only talks to targets that implement the Kirkware control
protocol. Garry's Mod integration is intentionally separate: the manager validates
Steam AppID 4000, installs supported addon/native-module files before launch, and
tracks only processes whose executable resolves inside that validated GMod install.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Iterable

APP_ID = "4000"
ROOT = Path(__file__).resolve().parent
BUILD_MAIN = ROOT / "build-linux"
BUILD_HARNESS = ROOT / "build-load-harness"
BUILD_NATIVE = ROOT / "build-gmod-native"
HARNESS_SOURCE = ROOT / "source" / "load_harness"
NATIVE_SOURCE = ROOT / "source" / "gmod_native"
ADDON_SOURCE = ROOT / "source" / "gmod_addon" / "kirkware_linux"
GMCOMMON_DEFAULT = ROOT / ".deps" / "garrysmod_common"
CACHE_DIR = Path.home() / ".cache" / "kirkware"


class KirkwareError(RuntimeError):
    pass


def say(message: str = "") -> None:
    print(message, flush=True)


def fail(message: str) -> "NoReturn":
    raise KirkwareError(message)


def run(command: Iterable[object], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, check: bool = True,
        capture: bool = False) -> subprocess.CompletedProcess[str]:
    cmd = [str(part) for part in command]
    say("$ " + " ".join(shlex_quote(part) for part in cmd))
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        check=check,
        text=True,
        capture_output=capture,
    )


def shlex_quote(value: str) -> str:
    import shlex
    return shlex.quote(value)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        fail(f"required command is missing: {name}")
    return path


def cpu_jobs(requested: int | None = None) -> int:
    if requested is not None:
        if requested <= 0:
            fail("jobs must be a positive integer")
        return requested
    return max(1, os.cpu_count() or 2)


def safe_clean(path: Path) -> None:
    resolved = path.resolve()
    if resolved in {Path("/"), ROOT.resolve()}:
        fail(f"refusing to clean unsafe directory: {resolved}")
    shutil.rmtree(resolved, ignore_errors=True)


def cmake_configure(source: Path, build: Path, definitions: dict[str, str]) -> None:
    require_tool("cmake")
    command: list[str] = ["cmake", "-S", str(source), "-B", str(build)]
    for key, value in definitions.items():
        command.append(f"-D{key}={value}")
    if not (build / "CMakeCache.txt").exists() and shutil.which("ninja"):
        command.extend(["-G", "Ninja"])
    run(command)


def cmake_build(build: Path, jobs: int) -> None:
    run(["cmake", "--build", build, "--parallel", jobs])


def ctest(build: Path) -> None:
    require_tool("ctest")
    run(["ctest", "--test-dir", build, "--output-on-failure"])


def build_main(*, clean: bool = False, debug: bool = False,
               sanitize: bool = False, tests: bool = True,
               ui: bool = True, package: bool = False,
               jobs: int | None = None) -> Path:
    build = ROOT / ("build-sanitize" if sanitize else "build-linux-debug" if debug else "build-linux")
    if clean:
        safe_clean(build)
    build_type = "Debug" if debug or sanitize else "Release"
    cmake_configure(ROOT, build, {
        "CMAKE_BUILD_TYPE": build_type,
        "KIRKWARE_BUILD_TESTS": "ON" if tests else "OFF",
        "KIRKWARE_BUILD_UI": "ON" if ui else "OFF",
        "KIRKWARE_ENABLE_SANITIZERS": "ON" if sanitize else "OFF",
        "KIRKWARE_WARNINGS_AS_ERRORS": "ON" if sanitize else "OFF",
    })
    cmake_build(build, cpu_jobs(jobs))
    if tests:
        ctest(build)
    if package:
        require_tool("cpack")
        package_dir = build / "packages"
        package_dir.mkdir(parents=True, exist_ok=True)
        run(["cpack", "--config", build / "CPackConfig.cmake", "-G", "TGZ", "-B", package_dir])
        if shutil.which("dpkg-deb"):
            run(["cpack", "--config", build / "CPackConfig.cmake", "-G", "DEB", "-B", package_dir])
    say(f"Main build ready: {build}")
    return build


def build_harness(*, clean: bool = False, debug: bool = False,
                  sanitize: bool = False, tests: bool = True,
                  jobs: int | None = None) -> Path:
    if clean:
        safe_clean(BUILD_HARNESS)
    cmake_configure(HARNESS_SOURCE, BUILD_HARNESS, {
        "CMAKE_BUILD_TYPE": "Debug" if debug or sanitize else "Release",
        "KIRKWARE_LOAD_HARNESS_SANITIZERS": "ON" if sanitize else "OFF",
        "KIRKWARE_LOAD_HARNESS_WERROR": "ON",
    })
    cmake_build(BUILD_HARNESS, cpu_jobs(jobs))
    if tests:
        ctest(BUILD_HARNESS)
    say(f"Harness client: {BUILD_HARNESS / 'kirkware-load-client'}")
    say(f"Harness target: {BUILD_HARNESS / 'kirkware-load-target'}")
    return BUILD_HARNESS


def steamapps_candidates() -> list[Path]:
    home = Path.home()
    initial = [
        home / ".local/share/Steam/steamapps",
        home / ".steam/steam/steamapps",
        home / ".steam/debian-installation/steamapps",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps",
        home / "snap/steam/common/.local/share/Steam/steamapps",
    ]
    found: list[Path] = []
    seen: set[Path] = set()

    def add(path: Path) -> None:
        try:
            normalized = path.expanduser().resolve()
        except OSError:
            return
        if normalized not in seen:
            seen.add(normalized)
            found.append(normalized)

    for steamapps in initial:
        add(steamapps)
        libraries = steamapps / "libraryfolders.vdf"
        if libraries.is_file():
            try:
                text = libraries.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for raw_path in re.findall(r'"path"\s+"([^"]+)"', text):
                add(Path(raw_path.replace("\\\\", "\\")) / "steamapps")
    return found


def parse_manifest(path: Path) -> dict[str, str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"cannot read Steam manifest {path}: {exc}")
    pairs = dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', text))
    return {key.lower(): value for key, value in pairs.items()}


def validate_gmod_install(game_dir: Path) -> tuple[Path, Path]:
    game = game_dir.expanduser().resolve()
    if not (game / "garrysmod").is_dir():
        fail(f"not a Garry's Mod install (missing garrysmod/): {game}")
    if game.parent.name != "common" or game.parent.parent.name != "steamapps":
        fail("GMod directory must resolve to a Steam steamapps/common installation")
    steamapps = game.parent.parent
    manifest = steamapps / f"appmanifest_{APP_ID}.acf"
    if not manifest.is_file():
        fail(f"Steam AppID {APP_ID} manifest was not found next to this install: {manifest}")
    data = parse_manifest(manifest)
    if data.get("appid") != APP_ID:
        fail(f"manifest does not identify AppID {APP_ID}")
    install_name = data.get("installdir", "")
    if install_name and install_name.casefold() != game.name.casefold():
        fail(f"manifest installdir={install_name!r} does not match {game.name!r}")
    return game, manifest


def discover_gmod(game_dir: Path | None = None) -> tuple[Path, Path]:
    if game_dir is not None:
        return validate_gmod_install(game_dir)
    errors: list[str] = []
    for steamapps in steamapps_candidates():
        manifest = steamapps / f"appmanifest_{APP_ID}.acf"
        if not manifest.is_file():
            continue
        try:
            data = parse_manifest(manifest)
            if data.get("appid") != APP_ID:
                continue
            name = data.get("installdir", "GarrysMod")
            return validate_gmod_install(steamapps / "common" / name)
        except KirkwareError as exc:
            errors.append(str(exc))
    detail = f" ({'; '.join(errors)})" if errors else ""
    fail(f"Steam Garry's Mod AppID {APP_ID} was not found{detail}")


def gmod_executables(game: Path) -> list[Path]:
    candidates = [
        game / "bin/linux64/gmod",
        game / "hl2_linux",
        game / "hl2.sh",
    ]
    return [path.resolve() for path in candidates if path.exists()]


def path_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError):
        return False


def gmod_pids(game: Path) -> list[tuple[int, Path]]:
    matches: list[tuple[int, Path]] = []
    proc = Path("/proc")
    if not proc.is_dir():
        return matches
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        exe_link = entry / "exe"
        try:
            exe = exe_link.resolve(strict=True)
        except (OSError, RuntimeError):
            continue
        if path_within(exe, game):
            matches.append((int(entry.name), exe))
    return sorted(matches)


def print_gmod_info(game_dir: Path | None = None) -> tuple[Path, Path]:
    game, manifest = discover_gmod(game_dir)
    say("Validated Garry's Mod installation")
    say(f"  AppID:    {APP_ID}")
    say(f"  Root:     {game}")
    say(f"  Manifest: {manifest}")
    executables = gmod_executables(game)
    say("  Executables:")
    for executable in executables or [Path("(none detected)")]:
        say(f"    {executable}")
    pids = gmod_pids(game)
    say("  Running processes:")
    if pids:
        for pid, exe in pids:
            say(f"    PID {pid}: {exe}")
    else:
        say("    none")
    return game, manifest


def install_addon(game_dir: Path | None = None) -> Path:
    game, _ = discover_gmod(game_dir)
    if not (ADDON_SOURCE / "lua/autorun/client").is_dir():
        fail(f"addon source is missing: {ADDON_SOURCE}")
    addons = game / "garrysmod/addons"
    target = addons / "kirkware_linux"
    addons.mkdir(parents=True, exist_ok=True)
    stage = addons / f".kirkware_linux.stage.{os.getpid()}"
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(ADDON_SOURCE, stage)
    shutil.rmtree(target, ignore_errors=True)
    stage.replace(target)
    say(f"Installed folder addon: {target}")
    return target


def find_steam_command() -> list[str]:
    steam = shutil.which("steam")
    if steam:
        return [steam]
    flatpak = shutil.which("flatpak")
    if flatpak:
        result = subprocess.run([flatpak, "info", "com.valvesoftware.Steam"],
                                text=True, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            return [flatpak, "run", "com.valvesoftware.Steam"]
    fail("Steam was not found in PATH and Flatpak Steam is not installed")


def rotate_console(game: Path) -> None:
    log = game / "garrysmod/console.txt"
    if log.is_file():
        stamp = time.strftime("%Y%m%d-%H%M%S")
        log.rename(game / "garrysmod" / f"console.kirkware-prev-{stamp}.txt")


def launch_gmod(*, game_dir: Path | None = None, local: bool = True,
                map_name: str = "gm_construct", keep_workshop: bool = False,
                install: bool = True, install_native_module: bool = True,
                allow_running: bool = False) -> list[tuple[int, Path]]:
    game, _ = discover_gmod(game_dir)
    running = gmod_pids(game)
    if running and not allow_running:
        fail("Garry's Mod is already running from the validated install; close it first or use --allow-running")
    if install:
        install_addon(game)
    if install_native_module and native_artifact().is_file():
        install_native(game_dir=game)
    elif install_native_module:
        say("Native module is not built yet; continuing with the Lua addon only.")
    executables = gmod_executables(game)
    if not executables:
        fail(f"no Linux GMod executable was found under {game}")
    say(f"Preselected program root: {game}")
    say(f"Preferred executable: {executables[0]}")
    before = {pid for pid, _ in running}
    if not running:
        rotate_console(game)
    args = ["-applaunch", APP_ID, "-condebug", "-console", "-disableluarefresh", "+developer", "1"]
    if not keep_workshop:
        args.append("-noworkshop")
    if local:
        args.extend(["+sv_lan", "1", "+sv_allowcslua", "1", "+gamemode", "sandbox", "+map", map_name])
    steam_cmd = find_steam_command()
    say("Launching validated Steam AppID 4000")
    subprocess.Popen(steam_cmd + args, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 20.0
    newest: list[tuple[int, Path]] = []
    while time.monotonic() < deadline:
        current = gmod_pids(game)
        newest = [(pid, exe) for pid, exe in current if pid not in before]
        if newest or (allow_running and current):
            break
        time.sleep(0.2)
    current = gmod_pids(game)
    if current:
        say("Validated GMod process(es):")
        for pid, exe in current:
            say(f"  PID {pid}: {exe}")
    else:
        say("Steam launch was requested, but no validated GMod PID is visible yet.")
    return current


def verify_live(game_dir: Path | None = None, require_native: bool = False) -> bool:
    game, _ = discover_gmod(game_dir)
    log = game / "garrysmod/console.txt"
    if not log.is_file():
        fail(f"console log does not exist: {log}")
    text = log.read_text(encoding="utf-8", errors="replace")
    checks = [
        ("base", "[kirkware linux] loaded; press Insert to toggle the menu"),
        ("combat", "[kirkware linux] combat modules loaded"),
        ("player visuals", "[kirkware linux] player visual parity loaded"),
        ("viewmodel/tracer", "[kirkware linux] viewmodel/tracer modules loaded"),
        ("desktop profile", "[kirkware linux] desktop profile bridge loaded"),
        ("dynamic module setter", "[kirkware linux] dynamic module setter loaded"),
    ]
    if require_native:
        checks.append(("native module", "[kirkware linux] native module loaded:"))
    missing = 0
    say("KIRKWARE LIVE GMOD INITIALIZATION CHECK")
    for label, needle in checks:
        if needle in text:
            say(f"[PASS] {label}")
        else:
            say(f"[MISS] {label}")
            missing += 1
    say("This verifies initialization only; gameplay behavior must be tested in the controlled session.")
    return missing == 0


def normalize_control_root(value: str | Path) -> Path:
    root = Path(value).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    if not root.is_dir():
        fail(f"control root is not a directory: {root}")
    return root


def loader_env(control_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["KIRKWARE_LOAD_CONTROL_ROOT"] = str(control_root)
    return env


def harness_client() -> Path:
    return BUILD_HARNESS / "kirkware-load-client"


def harness_target() -> Path:
    return BUILD_HARNESS / "kirkware-load-target"


def harness_test_module() -> Path:
    return BUILD_HARNESS / "libkirkware_load_test.so"


def control_dir(control_root: Path, pid: int) -> Path:
    return control_root / f"kirkware-load-target-{os.getuid()}-{pid}"


def require_harness() -> None:
    if not harness_client().is_file() or not harness_target().is_file():
        build_harness(tests=False)


def managed_paths(control_root: Path) -> tuple[Path, Path]:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(str(control_root).encode()).hexdigest()[:16]
    return CACHE_DIR / f"managed-{key}.pid", CACHE_DIR / f"managed-{key}.log"


def read_managed_pid(control_root: Path) -> int | None:
    pidfile, _ = managed_paths(control_root)
    try:
        pid = int(pidfile.read_text().strip())
    except (OSError, ValueError):
        return None
    try:
        os.kill(pid, 0)
    except OSError:
        pidfile.unlink(missing_ok=True)
        return None
    if not control_dir(control_root, pid).is_dir():
        return None
    return pid


def start_managed_target(control_root: Path) -> int:
    require_harness()
    existing = read_managed_pid(control_root)
    if existing:
        say(f"Managed cooperative target already running: PID {existing}")
        return existing
    pidfile, logfile = managed_paths(control_root)
    log_handle = logfile.open("a", encoding="utf-8")
    process = subprocess.Popen(
        [str(harness_target())],
        env=loader_env(control_root),
        stdout=log_handle,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        text=True,
    )
    pidfile.write_text(f"{process.pid}\n", encoding="utf-8")
    deadline = time.monotonic() + 4.0
    target_dir = control_dir(control_root, process.pid)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            fail(f"cooperative target exited during startup; see {logfile}")
        if target_dir.is_dir():
            say(f"Managed cooperative target ready: PID {process.pid}")
            say(f"Control directory: {target_dir}")
            return process.pid
        time.sleep(0.05)
    process.terminate()
    fail(f"cooperative target control directory did not appear: {target_dir}")


def loader_request(control_root: Path, pid: int, action: str,
                   module: Path | None = None) -> int:
    require_harness()
    if pid <= 0:
        fail("PID must be positive")
    try:
        os.kill(pid, 0)
    except OSError:
        fail(f"PID {pid} is not running or cannot be signalled")
    expected = control_dir(control_root, pid)
    if not expected.is_dir():
        fail(f"PID {pid} is not a cooperative target at {expected}")
    command = [str(harness_client()), "--target", str(pid)]
    if action == "status":
        command.append("--status")
    elif action == "load":
        selected = (module or harness_test_module()).expanduser().resolve()
        if not selected.is_file():
            fail(f"shared object does not exist: {selected}")
        command.extend(["--load", str(selected)])
    elif action == "unload":
        command.append("--unload")
    elif action == "quit":
        command.append("--quit")
    else:
        fail(f"unknown loader action: {action}")
    result = run(command, env=loader_env(control_root), check=False)
    return result.returncode


def list_targets(control_root: Path) -> int:
    require_harness()
    return run([harness_client(), "--list"], env=loader_env(control_root), check=False).returncode


def ensure_gmcommon(path: Path | None = None, fetch: bool = True) -> Path:
    candidate = (path or GMCOMMON_DEFAULT).expanduser().resolve()
    header = candidate / "include/GarrysMod/Lua/Interface.h"
    if header.is_file():
        return candidate
    if not fetch:
        fail(f"garrysmod_common was not found at {candidate}")
    require_tool("git")
    candidate.parent.mkdir(parents=True, exist_ok=True)
    if candidate.exists():
        fail(f"dependency directory exists but is incomplete: {candidate}")
    run(["git", "clone", "--depth", "1", "https://github.com/danielga/garrysmod_common.git", candidate])
    if not header.is_file():
        fail("garrysmod_common clone did not contain the expected Lua headers")
    return candidate


def native_artifact() -> Path:
    return BUILD_NATIVE / "gmcl_kirkware_native_linux64.dll"


def build_native(*, gmcommon: Path | None = None, clean: bool = False,
                 jobs: int | None = None, fetch_deps: bool = True) -> Path:
    common = ensure_gmcommon(gmcommon, fetch=fetch_deps)
    if clean:
        safe_clean(BUILD_NATIVE)
    cmake_configure(NATIVE_SOURCE, BUILD_NATIVE, {
        "CMAKE_BUILD_TYPE": "Release",
        "GARRYSMOD_COMMON_DIR": str(common),
    })
    cmake_build(BUILD_NATIVE, cpu_jobs(jobs))
    artifact = native_artifact()
    if not artifact.is_file():
        fail(f"native module build completed but artifact is missing: {artifact}")
    if artifact.read_bytes()[:4] != b"\x7fELF":
        fail(f"native module is not an ELF binary: {artifact}")
    say(f"Native GMod module ready: {artifact}")
    return artifact


def install_native(*, game_dir: Path | None = None, artifact: Path | None = None) -> Path:
    game, _ = discover_gmod(game_dir)
    source = (artifact or native_artifact()).expanduser().resolve()
    if not source.is_file():
        fail(f"native module is not built: {source}")
    if source.read_bytes()[:4] != b"\x7fELF":
        fail("refusing to install a non-ELF file as the Linux GMod module")
    destination_dir = game / "garrysmod/lua/bin"
    destination_dir.mkdir(parents=True, exist_ok=True)
    destination = destination_dir / "gmcl_kirkware_native_linux64.dll"
    shutil.copy2(source, destination)
    say(f"Installed native module: {destination}")
    return destination


def native_status(game_dir: Path | None = None) -> None:
    game, _ = discover_gmod(game_dir)
    installed = game / "garrysmod/lua/bin/gmcl_kirkware_native_linux64.dll"
    say(f"Built:     {native_artifact()} ({'yes' if native_artifact().is_file() else 'no'})")
    say(f"Installed: {installed} ({'yes' if installed.is_file() else 'no'})")


def open_control_ui() -> int:
    ui = ROOT / "kirkware-control-ui.py"
    if not ui.is_file():
        fail(f"control UI is missing: {ui}")
    return subprocess.call([sys.executable, str(ui)])


def self_test() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        steamapps = root / "steamapps"
        game = steamapps / "common/GarrysMod"
        (game / "garrysmod").mkdir(parents=True)
        executable = game / "bin/linux64/gmod"
        executable.parent.mkdir(parents=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        (steamapps / f"appmanifest_{APP_ID}.acf").write_text(
            '"AppState"\n{\n\t"appid"\t"4000"\n\t"installdir"\t"GarrysMod"\n}\n',
            encoding="utf-8",
        )
        validated, manifest = validate_gmod_install(game)
        if validated != game.resolve() or not manifest.is_file():
            say("GMod validation self-test failed")
            return 1
        if gmod_executables(validated)[0] != executable.resolve():
            say("GMod executable selection self-test failed")
            return 1
        root_control = normalize_control_root(root / "control")
        if control_dir(root_control, 123).name != f"kirkware-load-target-{os.getuid()}-123":
            say("loader control-path self-test failed")
            return 1
    say("kirkware.py self-test passed")
    return 0


def prompt_int(prompt: str) -> int | None:
    raw = input(prompt).strip()
    if not raw:
        return None
    try:
        value = int(raw)
    except ValueError:
        say("Please enter a number.")
        return None
    return value


def loader_menu() -> None:
    root = normalize_control_root(os.environ.get("KIRKWARE_LOAD_CONTROL_ROOT", "/tmp"))
    while True:
        say("\nCOOPERATIVE LOADER")
        say(f"Control root: {root}")
        say(f"Managed PID: {read_managed_pid(root) or 'not running'}")
        say("  1. Build + test harness")
        say("  2. Clean build + test harness")
        say("  3. Start/reuse managed target")
        say("  4. List cooperative targets")
        say("  5. Status for chosen PID")
        say("  6. Load .so into chosen cooperative PID")
        say("  7. Unload from chosen cooperative PID")
        say("  8. Stop chosen cooperative PID")
        say("  9. Change control root")
        say("  0. Back")
        choice = input("Select: ").strip()
        try:
            if choice == "1": build_harness()
            elif choice == "2": build_harness(clean=True)
            elif choice == "3": start_managed_target(root)
            elif choice == "4": list_targets(root)
            elif choice in {"5", "6", "7", "8"}:
                pid = prompt_int("Target PID: ")
                if not pid: continue
                actions = {"5": "status", "6": "load", "7": "unload", "8": "quit"}
                module = None
                if choice == "6":
                    raw = input(f"Module [{harness_test_module()}]: ").strip()
                    module = Path(raw).expanduser() if raw else harness_test_module()
                loader_request(root, pid, actions[choice], module)
            elif choice == "9":
                raw = input(f"New control root [{root}]: ").strip()
                if raw: root = normalize_control_root(raw)
            elif choice == "0": return
            else: say("Invalid selection")
        except KirkwareError as exc:
            say(f"error: {exc}")


def native_menu() -> None:
    while True:
        say("\nGMOD NATIVE MODULE")
        say("  1. Fetch/check garrysmod_common")
        say("  2. Build native Linux module")
        say("  3. Clean build native Linux module")
        say("  4. Install module into validated GMod")
        say("  5. Status")
        say("  0. Back")
        choice = input("Select: ").strip()
        try:
            if choice == "1": say(f"garrysmod_common: {ensure_gmcommon()}")
            elif choice == "2": build_native()
            elif choice == "3": build_native(clean=True)
            elif choice == "4": install_native()
            elif choice == "5": native_status()
            elif choice == "0": return
            else: say("Invalid selection")
        except KirkwareError as exc:
            say(f"error: {exc}")


def interactive_menu() -> int:
    while True:
        say("\nKIRKWARE LINUX MANAGER")
        say("Builder + cooperative loader + GMod native/module workflow")
        say("  1. Release build + tests")
        say("  2. Clean Release build + tests")
        say("  3. Cooperative loader")
        say("  4. GMod native module")
        say("  5. Install/update folder addon")
        say("  6. Launch private gm_construct test")
        say("  7. Launch GMod to main menu")
        say("  8. Show validated GMod install/processes")
        say("  9. Open Aim/Visuals/Misc control UI")
        say(" 10. Verify live initialization")
        say("  0. Exit")
        choice = input("Select: ").strip()
        try:
            if choice == "1": build_main()
            elif choice == "2": build_main(clean=True)
            elif choice == "3": loader_menu()
            elif choice == "4": native_menu()
            elif choice == "5": install_addon()
            elif choice == "6": launch_gmod(local=True)
            elif choice == "7": launch_gmod(local=False)
            elif choice == "8": print_gmod_info()
            elif choice == "9": open_control_ui()
            elif choice == "10": verify_live(require_native=False)
            elif choice == "0": return 0
            else: say("Invalid selection")
        except KirkwareError as exc:
            say(f"error: {exc}")


def path_arg(value: str) -> Path:
    return Path(value).expanduser()


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Unified Kirkware Linux manager")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("menu", help="open the interactive manager")
    sub.add_parser("self-test", help="run dependency-free manager self-tests")

    build = sub.add_parser("build", help="build the main Linux project")
    build.add_argument("--clean", action="store_true")
    build.add_argument("--debug", action="store_true")
    build.add_argument("--sanitize", action="store_true")
    build.add_argument("--no-tests", action="store_true")
    build.add_argument("--no-ui", action="store_true")
    build.add_argument("--package", action="store_true")
    build.add_argument("--jobs", type=int)

    harness = sub.add_parser("harness-build", help="build the cooperative loader harness")
    harness.add_argument("--clean", action="store_true")
    harness.add_argument("--debug", action="store_true")
    harness.add_argument("--sanitize", action="store_true")
    harness.add_argument("--no-tests", action="store_true")
    harness.add_argument("--jobs", type=int)

    loader = sub.add_parser("loader", help="operate the cooperative loader")
    loader.add_argument("--control-root", type=path_arg, default=Path(os.environ.get("KIRKWARE_LOAD_CONTROL_ROOT", "/tmp")))
    loader.add_argument("--list", action="store_true")
    loader.add_argument("--start-target", action="store_true")
    loader.add_argument("--pid", type=int)
    group = loader.add_mutually_exclusive_group()
    group.add_argument("--status", action="store_true")
    group.add_argument("--load", type=path_arg, nargs="?", const=harness_test_module())
    group.add_argument("--unload", action="store_true")
    group.add_argument("--quit", action="store_true")

    native = sub.add_parser("native", help="build/install the supported GMod binary module")
    native.add_argument("action", choices=["deps", "build", "install", "status"])
    native.add_argument("--gmcommon", type=path_arg)
    native.add_argument("--game-dir", type=path_arg)
    native.add_argument("--clean", action="store_true")
    native.add_argument("--no-fetch", action="store_true")
    native.add_argument("--jobs", type=int)

    gmod = sub.add_parser("gmod", help="validated Steam AppID-4000 workflow")
    gmod.add_argument("action", choices=["info", "install-addon", "launch", "verify"])
    gmod.add_argument("--game-dir", type=path_arg)
    gmod.add_argument("--menu", action="store_true", help="launch to menu instead of local map")
    gmod.add_argument("--map", default="gm_construct")
    gmod.add_argument("--keep-workshop", action="store_true")
    gmod.add_argument("--no-install", action="store_true")
    gmod.add_argument("--no-native", action="store_true")
    gmod.add_argument("--allow-running", action="store_true")
    gmod.add_argument("--require-native", action="store_true")

    sub.add_parser("ui", help="open the feature-control UI")
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        if args.command in {None, "menu"}:
            return interactive_menu()
        if args.command == "self-test":
            return self_test()
        if args.command == "build":
            build_main(clean=args.clean, debug=args.debug, sanitize=args.sanitize,
                       tests=not args.no_tests, ui=not args.no_ui,
                       package=args.package, jobs=args.jobs)
            return 0
        if args.command == "harness-build":
            build_harness(clean=args.clean, debug=args.debug, sanitize=args.sanitize,
                          tests=not args.no_tests, jobs=args.jobs)
            return 0
        if args.command == "loader":
            root = normalize_control_root(args.control_root)
            if args.list: return list_targets(root)
            if args.start_target:
                start_managed_target(root)
                return 0
            if args.pid is None:
                fail("loader action requires --pid, --list, or --start-target")
            if args.status: action, module = "status", None
            elif args.load is not None: action, module = "load", args.load
            elif args.unload: action, module = "unload", None
            elif args.quit: action, module = "quit", None
            else: fail("choose one of --status/--load/--unload/--quit")
            return loader_request(root, args.pid, action, module)
        if args.command == "native":
            if args.action == "deps":
                say(str(ensure_gmcommon(args.gmcommon, fetch=not args.no_fetch)))
            elif args.action == "build":
                build_native(gmcommon=args.gmcommon, clean=args.clean,
                             jobs=args.jobs, fetch_deps=not args.no_fetch)
            elif args.action == "install":
                install_native(game_dir=args.game_dir)
            else:
                native_status(args.game_dir)
            return 0
        if args.command == "gmod":
            if args.action == "info": print_gmod_info(args.game_dir)
            elif args.action == "install-addon": install_addon(args.game_dir)
            elif args.action == "launch":
                launch_gmod(game_dir=args.game_dir, local=not args.menu,
                            map_name=args.map, keep_workshop=args.keep_workshop,
                            install=not args.no_install,
                            install_native_module=not args.no_native,
                            allow_running=args.allow_running)
            else:
                return 0 if verify_live(args.game_dir, args.require_native) else 1
            return 0
        if args.command == "ui":
            return open_control_ui()
        return 0
    except KirkwareError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nInterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
