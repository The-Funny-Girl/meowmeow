#!/usr/bin/env python3
"""Kirkware Linux desktop UI for the supported private GMod workflow.

The user-facing UI intentionally does not expose the cooperative load-harness PID
controls. For Garry's Mod, Inject prepares the selected per-launch settings and
installs the supported addon/native module before startup. Launch Test is separate
and only starts the validated Steam AppID-4000 private client with already-prepared
files. The cooperative harness remains developer tooling in kirkware.py.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from tkinter import messagebox, ttk

APP_NAME = "Kirkware Linux"
REPO_ROOT = Path(__file__).resolve().parent
MANAGER = REPO_ROOT / "kirkware.py"
NATIVE_ARTIFACT = REPO_ROOT / "build-gmod-native/gmcl_kirkware_native_linux64.dll"
CONFIG_DIR = Path.home() / ".config" / "kirkware"
UI_CONFIG_PATH = CONFIG_DIR / "control_ui.json"
LOCAL_PROFILE_PATH = CONFIG_DIR / "desktop_feature_profile.conf"

MODULE_GROUPS = {
    "Aim": [
        ("legit_enable", "Legit aim", False),
        ("legit_fov_circle", "Legit FOV circle", True),
        ("legit_hitscan", "Legit hitscan", False),
        ("legit_triggerbot", "Triggerbot", False),
        ("legit_triggerbot_ammo", "Triggerbot ammo check", True),
        ("legit_triggerbot_canshoot", "Triggerbot fire-ready check", True),
        ("legit_recoil", "Legit recoil compensation", True),
        ("legit_visible_check", "Legit visible check", True),
        ("rage_enable", "Rage aim", False),
        ("rage_autofire", "Rage autofire", False),
        ("rage_autofire_ammo", "Rage ammo check", True),
        ("rage_can_fire", "Rage fire-ready check", True),
        ("rage_fix_movement", "Rage movement correction", True),
        ("rage_fov_circle", "Rage FOV circle", False),
        ("rage_hitscan", "Rage hitscan", True),
        ("rage_norecoil", "Rage recoil compensation", True),
        ("rage_target_lock", "Rage target lock", True),
        ("rage_visible_check", "Rage visible check", True),
        ("esp_target_line", "Current-target line", False),
    ],
    "Visuals": [
        ("esp_player_enable", "Player visuals master", False),
        ("esp_player_name", "Player names", True),
        ("esp_player_box", "Player boxes", True),
        ("esp_player_hpbar", "Health bar", True),
        ("esp_player_arbar", "Armor bar", False),
        ("esp_player_distance", "Player distance", False),
        ("esp_player_weapon", "Player weapon", False),
        ("esp_player_skeleton", "Player skeleton", False),
        ("esp_player_velocity", "Player velocity", False),
        ("esp_player_team", "Team info", False),
        ("esp_player_usergroup", "Usergroup", False),
        ("esp_player_flag_noclip", "Noclip flag", False),
        ("esp_player_oof", "Offscreen arrows", False),
        ("esp_player_hit_notify", "Hit marker", False),
        ("esp_player_hitsound", "Hit sound", False),
        ("chams_enable", "Player outlines", False),
        ("chams_hands_enable", "Hand chams", False),
        ("chams_weapon_enable", "Weapon chams", False),
        ("esp_other_crosshair", "Crosshair", False),
        ("esp_other_tracer", "Bullet tracers", False),
        ("entities_enable", "Entity visuals master", False),
        ("entities_name", "Entity names", True),
        ("entities_distance", "Entity distance", False),
        ("entities_box", "Entity boxes", False),
        ("entities_index", "Entity index", False),
    ],
    "Misc": [
        ("menu_watermark", "Watermark", True),
        ("menu_spectators", "Spectator list", False),
        ("menu_hotkeys", "Enabled-module HUD", False),
        ("misc_thirdperson", "Third person", False),
        ("misc_fov_changer", "FOV changer", False),
        ("misc_zoom", "Zoom", False),
        ("misc_bunnyhop", "Bunnyhop", False),
        ("misc_auto_strafe", "Auto strafe", False),
        ("misc_auto_pistol", "Auto pistol", False),
        ("misc_freecam", "Freecam", False),
        ("misc_faststop", "Fast stop", False),
        ("misc_use_spam", "Use spam", False),
    ],
}

TUNING_GROUPS = {
    "Aim": [
        ("kirkware_legit_fov", "Legit FOV", 6.0, 0.25, 45.0, 0.25),
        ("kirkware_legit_smoothing", "Legit smoothing", 8.0, 1.0, 40.0, 0.5),
        ("kirkware_legit_max_distance", "Legit max distance", 10000.0, 100.0, 50000.0, 100.0),
        ("kirkware_trigger_delay", "Trigger delay", 0.03, 0.0, 1.0, 0.01),
        ("kirkware_rage_fov", "Rage FOV", 35.0, 1.0, 180.0, 1.0),
        ("kirkware_rage_max_distance", "Rage max distance", 20000.0, 100.0, 50000.0, 100.0),
    ],
    "Visuals": [
        ("kirkware_entity_distance", "Entity distance", 2500.0, 250.0, 10000.0, 50.0),
        ("kirkware_tracer_time", "Tracer lifetime", 0.8, 0.05, 5.0, 0.05),
    ],
    "Misc": [
        ("kirkware_fov", "Camera FOV", 100.0, 60.0, 130.0, 1.0),
        ("kirkware_zoom_fov", "Zoom FOV", 40.0, 10.0, 100.0, 1.0),
        ("kirkware_thirdperson_distance", "Third-person distance", 110.0, 30.0, 300.0, 1.0),
        ("kirkware_freecam_speed", "Freecam speed", 650.0, 50.0, 4000.0, 25.0),
        ("kirkware_freecam_boost", "Freecam boost", 3.0, 1.0, 10.0, 0.5),
    ],
}

BOOL_CONVARS = [
    ("kirkware_aim_teammates", "Aim at teammates", False),
    ("kirkware_legit_require_attack", "Legit only while attacking", True),
]

SORT_CONVARS = [
    ("kirkware_legit_sort", "Legit target sort", "FOV"),
    ("kirkware_rage_sort", "Rage target sort", "FOV"),
]
SORT_TO_VALUE = {"FOV": "0", "Distance": "1", "Health": "2"}
VALUE_TO_SORT = {value: key for key, value in SORT_TO_VALUE.items()}


def bool_text(value: bool) -> str:
    return "true" if value else "false"


def serialize_profile(modules: dict[str, bool], convars: dict[str, str]) -> str:
    lines = [
        "# Kirkware Linux desktop feature profile v1",
        "# Written by kirkware-control-ui.py",
    ]
    for key in sorted(modules):
        lines.append(f"module.{key}={bool_text(bool(modules[key]))}")
    for key in sorted(convars):
        lines.append(f"convar.{key}={convars[key]}")
    return "\n".join(lines) + "\n"


def parse_profile(text: str) -> tuple[dict[str, bool], dict[str, str]]:
    modules: dict[str, bool] = {}
    convars: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = [part.strip() for part in line.split("=", 1)]
        if key.startswith("module."):
            name = key[len("module."):]
            if value.lower() in {"true", "1"}:
                modules[name] = True
            elif value.lower() in {"false", "0"}:
                modules[name] = False
        elif key.startswith("convar."):
            convars[key[len("convar."):]] = value
    return modules, convars


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def manager_command(*parts: str) -> list[str]:
    return [sys.executable, str(MANAGER), *parts]


def discover_profile_path() -> Path:
    """Use the manager's GMod discovery so profile and launch target stay aligned."""
    try:
        import kirkware as manager
        game, _manifest = manager.discover_gmod()
        return game / "garrysmod/data/kirkware_linux/desktop_profile.conf"
    except Exception as exc:
        raise RuntimeError(f"Could not locate the validated Garry's Mod install: {exc}") from exc


def validated_gmod_processes() -> list[tuple[int, Path]]:
    """Return only processes recognized by the manager for the validated install."""
    try:
        import kirkware as manager
        game, _manifest = manager.discover_gmod()
        return manager.gmod_pids(game)
    except Exception as exc:
        raise RuntimeError(f"Could not inspect the validated Garry's Mod install: {exc}") from exc


class ScrollableFrame(ttk.Frame):
    def __init__(self, parent: tk.Misc) -> None:
        super().__init__(parent)
        canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, bg="#121212")
        scrollbar = ttk.Scrollbar(self, orient="vertical", command=canvas.yview)
        self.body = ttk.Frame(canvas)
        self.body.bind("<Configure>", lambda _e: canvas.configure(scrollregion=canvas.bbox("all")))
        window_id = canvas.create_window((0, 0), window=self.body, anchor="nw")
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(window_id, width=e.width))
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")


class ControlUi:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_NAME)
        self.root.geometry("960x720")
        self.root.minsize(840, 640)
        self.root.configure(bg="#121212")

        self.module_vars: dict[str, tk.BooleanVar] = {}
        self.convar_vars: dict[str, tk.StringVar] = {}
        self.bool_convar_ui_vars: dict[str, tk.BooleanVar] = {}
        self.sort_display_vars: dict[str, tk.StringVar] = {}

        self.username_var = tk.StringVar()
        self.password_var = tk.StringVar()
        self.map_var = tk.StringVar(value="gm_construct")
        self.keep_workshop_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Login required")
        self.busy = False
        self.main_frame: ttk.Frame | None = None
        self.output: tk.Text | None = None

        self._configure_style()
        self._load_ui_config()
        self._create_login()
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", background="#1b1b1b", foreground="#f2f2f2", fieldbackground="#222222")
        style.configure("TFrame", background="#1b1b1b")
        style.configure("TLabel", background="#1b1b1b", foreground="#f2f2f2")
        style.configure("TCheckbutton", background="#1b1b1b", foreground="#f2f2f2")
        style.map("TCheckbutton", background=[("active", "#242424")])
        style.configure("TButton", background="#2b97fa", foreground="#ffffff", padding=7)
        style.map("TButton", background=[("active", "#54adff")])
        style.configure("TNotebook", background="#121212", borderwidth=0)
        style.configure("TNotebook.Tab", background="#242424", foreground="#d7d7d7", padding=(14, 8))
        style.map("TNotebook.Tab", background=[("selected", "#2b97fa")], foreground=[("selected", "#ffffff")])
        style.configure("TLabelframe", background="#1b1b1b", foreground="#ffffff")
        style.configure("TLabelframe.Label", background="#1b1b1b", foreground="#2b97fa")
        style.configure("TEntry", fieldbackground="#222222", foreground="#ffffff")
        style.configure("TSpinbox", fieldbackground="#222222", foreground="#ffffff")
        style.configure("TCombobox", fieldbackground="#222222", foreground="#ffffff")

    def _load_ui_config(self) -> None:
        try:
            data = json.loads(UI_CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return
        self.username_var.set(str(data.get("username", "")))
        self.map_var.set(str(data.get("map", "gm_construct")) or "gm_construct")
        self.keep_workshop_var.set(bool(data.get("keep_workshop", False)))

    def _save_ui_config(self) -> None:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        data = {
            "username": self.username_var.get().strip(),
            "map": self.map_var.get().strip() or "gm_construct",
            "keep_workshop": bool(self.keep_workshop_var.get()),
        }
        atomic_write(UI_CONFIG_PATH, json.dumps(data, indent=2) + "\n")

    def _create_login(self) -> None:
        frame = ttk.Frame(self.root)
        frame.place(relx=0.5, rely=0.5, anchor="center", width=430, height=330)
        self.login_frame = frame

        ttk.Label(frame, text="kirkware", font=("TkDefaultFont", 24, "bold")).pack(pady=(26, 2))
        ttk.Label(frame, text="linux", foreground="#2b97fa").pack()

        form = ttk.LabelFrame(frame, text="Local login")
        form.pack(fill="x", padx=30, pady=22)
        ttk.Label(form, text="Username").grid(row=0, column=0, sticky="w", padx=10, pady=8)
        ttk.Entry(form, textvariable=self.username_var).grid(row=0, column=1, sticky="ew", padx=10, pady=8)
        ttk.Label(form, text="Password").grid(row=1, column=0, sticky="w", padx=10, pady=8)
        password = ttk.Entry(form, textvariable=self.password_var, show="•")
        password.grid(row=1, column=1, sticky="ew", padx=10, pady=8)
        password.bind("<Return>", lambda _event: self.login())
        form.columnconfigure(1, weight=1)

        ttk.Button(frame, text="Login", command=self.login).pack(pady=4)
        ttk.Label(
            frame,
            text="Local session gate only — no remote authentication backend is configured yet.",
            foreground="#aaaaaa",
        ).pack(pady=(8, 0))

    def login(self) -> None:
        if not self.username_var.get().strip() or not self.password_var.get():
            messagebox.showerror(APP_NAME, "Enter a username and password.")
            return
        self.password_var.set("")
        self._save_ui_config()
        self.login_frame.destroy()
        self.status_var.set(f"Logged in locally as {self.username_var.get().strip()}")
        self._create_main()

    def _create_main(self) -> None:
        self.module_vars = {}
        self.convar_vars = {}
        self.bool_convar_ui_vars = {}
        self.sort_display_vars = {}

        container = ttk.Frame(self.root)
        container.pack(fill="both", expand=True)
        self.main_frame = container

        header = ttk.Frame(container)
        header.pack(fill="x", padx=14, pady=(12, 6))
        ttk.Label(header, text="kirkware", font=("TkDefaultFont", 18, "bold")).pack(side="left")
        ttk.Label(header, text=" linux", foreground="#2b97fa").pack(side="left", pady=(5, 0))
        ttk.Label(header, textvariable=self.status_var).pack(side="right", pady=(5, 8))
        ttk.Button(header, text="Logout", command=self.logout).pack(side="right", padx=10)

        notebook = ttk.Notebook(container)
        notebook.pack(fill="both", expand=True, padx=12, pady=8)
        inject = ttk.Frame(notebook)
        aim = ScrollableFrame(notebook)
        visuals = ScrollableFrame(notebook)
        misc = ScrollableFrame(notebook)
        notebook.add(inject, text="Inject")
        notebook.add(aim, text="Aim")
        notebook.add(visuals, text="Visuals")
        notebook.add(misc, text="Misc")

        self._build_inject(inject)
        self._build_feature_tab(aim.body, "Aim")
        self._build_feature_tab(visuals.body, "Visuals")
        self._build_feature_tab(misc.body, "Misc")
        self._load_profile(silent=True)

    def logout(self) -> None:
        if self.module_vars:
            self.save_profile(silent=True)
        if self.main_frame is not None:
            self.main_frame.destroy()
            self.main_frame = None
        self.output = None
        self.status_var.set("Login required")
        self._create_login()

    def _build_inject(self, parent: ttk.Frame) -> None:
        session = ttk.LabelFrame(parent, text="Garry's Mod private session")
        session.pack(fill="x", padx=16, pady=16)
        ttk.Label(session, text="Map").grid(row=0, column=0, sticky="w", padx=10, pady=8)
        ttk.Entry(session, textvariable=self.map_var, width=28).grid(row=0, column=1, sticky="w", padx=10, pady=8)
        ttk.Checkbutton(
            session,
            text="Keep Workshop addons enabled",
            variable=self.keep_workshop_var,
            command=self._save_ui_config,
        ).grid(row=1, column=0, columnspan=2, sticky="w", padx=10, pady=6)
        ttk.Label(
            session,
            text=(
                "Inject prepares Kirkware before GMod starts: it writes this launch's settings and\n"
                "installs/updates the supported folder addon plus native module. Launch Test is\n"
                "separate and only starts the private AppID-4000 test using the prepared files."
            ),
            justify="left",
            foreground="#bbbbbb",
        ).grid(row=2, column=0, columnspan=4, sticky="w", padx=10, pady=(8, 12))

        buttons = ttk.Frame(session)
        buttons.grid(row=3, column=0, columnspan=4, sticky="w", padx=8, pady=(0, 10))
        ttk.Button(buttons, text="Inject", command=self.inject).pack(side="left", padx=4)
        ttk.Button(buttons, text="Launch Test", command=self.launch_test).pack(side="left", padx=4)
        ttk.Button(buttons, text="Apply Settings", command=self.apply_settings).pack(side="left", padx=4)
        ttk.Button(buttons, text="Verify Loaded", command=self.verify_loaded).pack(side="left", padx=4)

        output_box = ttk.LabelFrame(parent, text="Session output")
        output_box.pack(fill="both", expand=True, padx=16, pady=(0, 16))
        self.output = tk.Text(
            output_box,
            height=20,
            bg="#111111",
            fg="#dddddd",
            insertbackground="#ffffff",
            relief="flat",
        )
        self.output.pack(fill="both", expand=True, padx=8, pady=8)
        self._log("Ready. Configure settings, click Inject, then Launch Test.\n")

    def _build_feature_tab(self, parent: ttk.Frame, group: str) -> None:
        modules_box = ttk.LabelFrame(parent, text=f"{group} modules")
        modules_box.pack(fill="x", padx=14, pady=12)
        for index, (module_id, label, default) in enumerate(MODULE_GROUPS[group]):
            var = tk.BooleanVar(value=default)
            self.module_vars[module_id] = var
            ttk.Checkbutton(
                modules_box,
                text=label,
                variable=var,
                command=self.feature_changed,
            ).grid(row=index // 2, column=index % 2, sticky="w", padx=10, pady=5)
        modules_box.columnconfigure(0, weight=1)
        modules_box.columnconfigure(1, weight=1)

        if group == "Aim":
            policy = ttk.LabelFrame(parent, text="Aim policy")
            policy.pack(fill="x", padx=14, pady=(0, 12))
            for index, (name, label, default) in enumerate(BOOL_CONVARS):
                ui_var = tk.BooleanVar(value=default)
                self.bool_convar_ui_vars[name] = ui_var
                self.convar_vars[name] = tk.StringVar(value="1" if default else "0")

                def update_bool(n=name, v=ui_var) -> None:
                    self.convar_vars[n].set("1" if v.get() else "0")
                    self.feature_changed()

                ttk.Checkbutton(policy, text=label, variable=ui_var, command=update_bool).grid(
                    row=0, column=index, sticky="w", padx=10, pady=6
                )

        tuning = TUNING_GROUPS.get(group, [])
        if tuning:
            tuning_box = ttk.LabelFrame(parent, text="Tuning")
            tuning_box.pack(fill="x", padx=14, pady=(0, 12))
            for row, (name, label, default, minimum, maximum, increment) in enumerate(tuning):
                var = tk.StringVar(value=f"{default:g}")
                self.convar_vars[name] = var
                ttk.Label(tuning_box, text=label).grid(row=row, column=0, sticky="w", padx=10, pady=4)
                spin = ttk.Spinbox(
                    tuning_box,
                    textvariable=var,
                    from_=minimum,
                    to=maximum,
                    increment=increment,
                    width=14,
                    command=self.feature_changed,
                )
                spin.grid(row=row, column=1, sticky="w", padx=10, pady=4)
                spin.bind("<Return>", lambda _e: self.feature_changed())
                spin.bind("<FocusOut>", lambda _e: self.feature_changed())

        if group == "Aim":
            sort_box = ttk.LabelFrame(parent, text="Target sorting")
            sort_box.pack(fill="x", padx=14, pady=(0, 14))
            for row, (name, label, default) in enumerate(SORT_CONVARS):
                display = tk.StringVar(value=default)
                self.sort_display_vars[name] = display
                self.convar_vars[name] = tk.StringVar(value=SORT_TO_VALUE[default])
                ttk.Label(sort_box, text=label).grid(row=row, column=0, sticky="w", padx=10, pady=5)
                combo = ttk.Combobox(
                    sort_box,
                    textvariable=display,
                    values=list(SORT_TO_VALUE),
                    state="readonly",
                    width=16,
                )
                combo.grid(row=row, column=1, sticky="w", padx=10, pady=5)
                combo.bind("<<ComboboxSelected>>", lambda _e, n=name, d=display: self._sort_changed(n, d))

    def _sort_changed(self, name: str, display: tk.StringVar) -> None:
        self.convar_vars[name].set(SORT_TO_VALUE.get(display.get(), "0"))
        self.feature_changed()

    def _sync_convar_widgets(self) -> None:
        for name, ui_var in self.bool_convar_ui_vars.items():
            stored = self.convar_vars.get(name)
            value = stored.get().strip().lower() if stored is not None else "0"
            ui_var.set(value in {"1", "true"})
        for name, display in self.sort_display_vars.items():
            stored = self.convar_vars.get(name)
            value = stored.get().strip() if stored is not None else "0"
            display.set(VALUE_TO_SORT.get(value, "FOV"))

    def _current_profile(self) -> tuple[dict[str, bool], dict[str, str]]:
        modules = {name: bool(var.get()) for name, var in self.module_vars.items()}
        convars = {name: var.get().strip() for name, var in self.convar_vars.items()}
        return modules, convars

    def feature_changed(self) -> None:
        self.save_profile(silent=True)

    def save_profile(self, silent: bool = False) -> None:
        modules, convars = self._current_profile()
        atomic_write(LOCAL_PROFILE_PATH, serialize_profile(modules, convars))
        if not silent:
            self.status_var.set(f"Saved {LOCAL_PROFILE_PATH}")

    def _load_profile(self, silent: bool = False) -> bool:
        try:
            text = LOCAL_PROFILE_PATH.read_text(encoding="utf-8")
        except OSError:
            return False
        modules, convars = parse_profile(text)
        for name, value in modules.items():
            if name in self.module_vars:
                self.module_vars[name].set(value)
        for name, value in convars.items():
            if name in self.convar_vars:
                self.convar_vars[name].set(value)
        self._sync_convar_widgets()
        if not silent:
            self.status_var.set(f"Loaded {LOCAL_PROFILE_PATH}")
        return True

    def export_profile(self) -> Path:
        destination = discover_profile_path()
        modules, convars = self._current_profile()
        atomic_write(destination, serialize_profile(modules, convars))
        self.status_var.set(f"Settings ready: {destination}")
        return destination

    def apply_settings(self) -> None:
        try:
            self.save_profile(silent=True)
            destination = self.export_profile()
        except Exception as exc:
            messagebox.showerror(APP_NAME, str(exc))
            self.status_var.set("Could not apply settings")
            return
        self._log(f"Applied settings to {destination}\n")

    def _run_background(self, label: str, commands: list[list[str]]) -> None:
        if self.busy:
            self._log("Another Kirkware action is already running.\n")
            return
        self.busy = True
        self.status_var.set(label)

        def worker() -> None:
            success = True
            output_parts: list[str] = []
            for command in commands:
                output_parts.append("$ " + " ".join(command) + "\n")
                try:
                    result = subprocess.run(
                        command,
                        cwd=str(REPO_ROOT),
                        text=True,
                        capture_output=True,
                    )
                except OSError as exc:
                    output_parts.append(f"error: {exc}\n")
                    success = False
                    break
                output_parts.append(result.stdout or "")
                output_parts.append(result.stderr or "")
                if result.returncode != 0:
                    success = False
                    break

            text = "".join(output_parts)

            def finish() -> None:
                self._log(text)
                self.busy = False
                self.status_var.set("Ready" if success else "Action failed")
                if not success:
                    messagebox.showerror(APP_NAME, "The Kirkware action failed. See Session output.")

            self.root.after(0, finish)

        threading.Thread(target=worker, daemon=True).start()

    def inject(self) -> None:
        try:
            running = validated_gmod_processes()
            if running:
                details = ", ".join(f"PID {pid}" for pid, _exe in running)
                raise RuntimeError(
                    "Close Garry's Mod before Inject. This supported injection path installs the "
                    f"addon/native module before startup. Running process(es): {details}"
                )
            self._save_ui_config()
            self.save_profile(silent=True)
            destination = self.export_profile()
        except Exception as exc:
            messagebox.showerror(APP_NAME, str(exc))
            self.status_var.set("Inject blocked")
            return

        self._log(f"Per-inject settings: {destination}\n")
        commands: list[list[str]] = []
        if not NATIVE_ARTIFACT.is_file():
            commands.append(manager_command("native", "build", "--clean"))
        commands.append(manager_command("gmod", "install-addon"))
        commands.append(manager_command("native", "install"))
        self._run_background("Preparing Kirkware for GMod", commands)

    def launch_test(self) -> None:
        self._save_ui_config()
        launch = manager_command(
            "gmod",
            "launch",
            "--no-install",
            "--no-native",
            "--map",
            self.map_var.get().strip() or "gm_construct",
        )
        if self.keep_workshop_var.get():
            launch.append("--keep-workshop")
        self._log("Launching private test without reinstalling Kirkware.\n")
        self._run_background("Launching private GMod test", [launch])

    def verify_loaded(self) -> None:
        self._run_background(
            "Verifying GMod initialization",
            [manager_command("gmod", "verify", "--require-native")],
        )

    def _log(self, text: str) -> None:
        if self.output is None:
            return
        self.output.insert("end", text)
        self.output.see("end")

    def close(self) -> None:
        try:
            self._save_ui_config()
            if self.module_vars:
                self.save_profile(silent=True)
        finally:
            self.password_var.set("")
            self.root.destroy()


def self_test() -> int:
    modules = {"legit_enable": True, "esp_player_enable": False}
    convars = {"kirkware_legit_fov": "7.5", "kirkware_legit_sort": "1"}
    encoded = serialize_profile(modules, convars)
    decoded_modules, decoded_convars = parse_profile(encoded)
    if decoded_modules != modules or decoded_convars != convars:
        print("profile round-trip failed", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "profile.conf"
        atomic_write(path, encoded)
        if parse_profile(path.read_text(encoding="utf-8")) != (modules, convars):
            print("atomic profile round-trip failed", file=sys.stderr)
            return 1
    print("control UI profile self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=APP_NAME)
    parser.add_argument("--self-test", action="store_true", help="test profile serialization without opening the UI")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = tk.Tk()
    ControlUi(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
