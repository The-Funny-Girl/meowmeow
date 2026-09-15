#!/usr/bin/env python3
"""Kirkware Linux desktop control panel.

This UI edits the existing Kirkware Linux addon feature settings and can drive
only the cooperative load-harness protocol.  It does not attach to or modify
unrelated processes.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_NAME = "Kirkware Linux Control"
CONFIG_DIR = Path.home() / ".config" / "kirkware"
UI_CONFIG_PATH = CONFIG_DIR / "control_ui.json"
LOCAL_PROFILE_PATH = CONFIG_DIR / "desktop_feature_profile.conf"
REPO_ROOT = Path(__file__).resolve().parent

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

TUNING = [
    ("kirkware_legit_fov", "Legit FOV", 6.0, 0.25, 45.0, 0.25),
    ("kirkware_legit_smoothing", "Legit smoothing", 8.0, 1.0, 40.0, 0.5),
    ("kirkware_legit_max_distance", "Legit max distance", 10000.0, 100.0, 50000.0, 100.0),
    ("kirkware_trigger_delay", "Trigger delay", 0.03, 0.0, 1.0, 0.01),
    ("kirkware_rage_fov", "Rage FOV", 35.0, 1.0, 180.0, 1.0),
    ("kirkware_rage_max_distance", "Rage max distance", 20000.0, 100.0, 50000.0, 100.0),
    ("kirkware_fov", "Camera FOV", 100.0, 60.0, 130.0, 1.0),
    ("kirkware_zoom_fov", "Zoom FOV", 40.0, 10.0, 100.0, 1.0),
    ("kirkware_thirdperson_distance", "Third-person distance", 110.0, 30.0, 300.0, 1.0),
    ("kirkware_entity_distance", "Entity distance", 2500.0, 250.0, 10000.0, 50.0),
    ("kirkware_freecam_speed", "Freecam speed", 650.0, 50.0, 4000.0, 25.0),
    ("kirkware_freecam_boost", "Freecam boost", 3.0, 1.0, 10.0, 0.5),
    ("kirkware_tracer_time", "Tracer lifetime", 0.8, 0.05, 5.0, 0.05),
]

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


def default_export_path() -> Path:
    candidates = [
        Path.home() / ".local/share/Steam/steamapps/common/GarrysMod/garrysmod/data/kirkware_linux/desktop_profile.conf",
        Path.home() / ".steam/steam/steamapps/common/GarrysMod/garrysmod/data/kirkware_linux/desktop_profile.conf",
        Path.home() / ".steam/debian-installation/steamapps/common/GarrysMod/garrysmod/data/kirkware_linux/desktop_profile.conf",
    ]
    for path in candidates:
        if path.parent.parent.exists():
            return path
    return candidates[0]


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
        self.root.geometry("940x700")
        self.root.minsize(820, 620)
        self.root.configure(bg="#121212")
        self.test_target: subprocess.Popen[str] | None = None

        self.module_vars: dict[str, tk.BooleanVar] = {}
        self.convar_vars: dict[str, tk.StringVar] = {}
        self.status_var = tk.StringVar(value="Ready")
        self.pid_var = tk.StringVar()
        self.control_root_var = tk.StringVar(value="/tmp")
        self.module_path_var = tk.StringVar(value=str(REPO_ROOT / "build-load-harness/libkirkware_load_test.so"))
        self.export_path_var = tk.StringVar(value=str(default_export_path()))
        self.live_export_var = tk.BooleanVar(value=True)

        self._configure_style()
        self._load_ui_config()
        self._create_widgets()
        self._load_profile(silent=True)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", background="#1b1b1b", foreground="#f2f2f2", fieldbackground="#222222")
        style.configure("TFrame", background="#1b1b1b")
        style.configure("TLabel", background="#1b1b1b", foreground="#f2f2f2")
        style.configure("TCheckbutton", background="#1b1b1b", foreground="#f2f2f2")
        style.map("TCheckbutton", background=[("active", "#242424")])
        style.configure("TButton", background="#2b97fa", foreground="#ffffff", padding=6)
        style.map("TButton", background=[("active", "#54adff")])
        style.configure("TNotebook", background="#121212", borderwidth=0)
        style.configure("TNotebook.Tab", background="#242424", foreground="#d7d7d7", padding=(14, 8))
        style.map("TNotebook.Tab", background=[("selected", "#2b97fa")], foreground=[("selected", "#ffffff")])
        style.configure("TLabelframe", background="#1b1b1b", foreground="#ffffff")
        style.configure("TLabelframe.Label", background="#1b1b1b", foreground="#2b97fa")
        style.configure("TEntry", fieldbackground="#222222", foreground="#ffffff")
        style.configure("TSpinbox", fieldbackground="#222222", foreground="#ffffff", arrowsize=14)
        style.configure("TCombobox", fieldbackground="#222222", foreground="#ffffff")

    def _load_ui_config(self) -> None:
        try:
            data = json.loads(UI_CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return
        self.pid_var.set(str(data.get("pid", "")))
        self.control_root_var.set(str(data.get("control_root", "/tmp")))
        self.module_path_var.set(str(data.get("module_path", self.module_path_var.get())))
        self.export_path_var.set(str(data.get("export_path", self.export_path_var.get())))
        self.live_export_var.set(bool(data.get("live_export", True)))

    def _save_ui_config(self) -> None:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        data = {
            "pid": self.pid_var.get().strip(),
            "control_root": self.control_root_var.get().strip(),
            "module_path": self.module_path_var.get().strip(),
            "export_path": self.export_path_var.get().strip(),
            "live_export": bool(self.live_export_var.get()),
        }
        UI_CONFIG_PATH.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def _create_widgets(self) -> None:
        header = ttk.Frame(self.root)
        header.pack(fill="x", padx=14, pady=(12, 6))
        ttk.Label(header, text="kirkware", font=("TkDefaultFont", 18, "bold")).pack(side="left")
        ttk.Label(header, text=" linux control", foreground="#2b97fa").pack(side="left", pady=(5, 0))
        ttk.Label(header, textvariable=self.status_var).pack(side="right", pady=(5, 0))

        notebook = ttk.Notebook(self.root)
        notebook.pack(fill="both", expand=True, padx=12, pady=8)

        loader = ttk.Frame(notebook)
        aim = ScrollableFrame(notebook)
        visuals = ScrollableFrame(notebook)
        misc = ScrollableFrame(notebook)
        config = ttk.Frame(notebook)
        notebook.add(loader, text="Loader")
        notebook.add(aim, text="Aim")
        notebook.add(visuals, text="Visuals")
        notebook.add(misc, text="Misc")
        notebook.add(config, text="Config")

        self._build_loader(loader)
        self._build_feature_tab(aim.body, "Aim", include_tuning=True)
        self._build_feature_tab(visuals.body, "Visuals")
        self._build_feature_tab(misc.body, "Misc")
        self._build_config(config)

    def _build_loader(self, parent: ttk.Frame) -> None:
        box = ttk.LabelFrame(parent, text="Cooperative target")
        box.pack(fill="x", padx=16, pady=16)
        self._entry_row(box, 0, "Target PID", self.pid_var)
        self._entry_row(box, 1, "Control root", self.control_root_var, browse_dir=True)
        self._entry_row(box, 2, "Module .so", self.module_path_var, browse_file=True)

        buttons = ttk.Frame(box)
        buttons.grid(row=3, column=0, columnspan=3, sticky="ew", padx=8, pady=10)
        for label, command in [
            ("List targets", self.list_targets),
            ("Status", lambda: self.loader_action("status")),
            ("Load .so", lambda: self.loader_action("load")),
            ("Unload", lambda: self.loader_action("unload")),
            ("Stop target", lambda: self.loader_action("quit")),
            ("Start test target", self.start_test_target),
        ]:
            ttk.Button(buttons, text=label, command=command).pack(side="left", padx=4)

        output_box = ttk.LabelFrame(parent, text="Loader output")
        output_box.pack(fill="both", expand=True, padx=16, pady=(0, 16))
        self.loader_output = tk.Text(output_box, height=18, bg="#111111", fg="#dddddd", insertbackground="#ffffff", relief="flat")
        self.loader_output.pack(fill="both", expand=True, padx=8, pady=8)
        self._log("The PID must be a cooperating target using the same control root.\n")

    def _entry_row(self, parent: ttk.Frame, row: int, label: str, variable: tk.StringVar,
                   browse_file: bool = False, browse_dir: bool = False) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=8, pady=6)
        entry = ttk.Entry(parent, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", padx=8, pady=6)
        parent.columnconfigure(1, weight=1)
        if browse_file:
            ttk.Button(parent, text="Browse", command=lambda: self._browse_file(variable)).grid(row=row, column=2, padx=8)
        elif browse_dir:
            ttk.Button(parent, text="Browse", command=lambda: self._browse_dir(variable)).grid(row=row, column=2, padx=8)

    def _build_feature_tab(self, parent: ttk.Frame, group: str, include_tuning: bool = False) -> None:
        modules_box = ttk.LabelFrame(parent, text=f"{group} modules")
        modules_box.pack(fill="x", padx=14, pady=12)
        for index, (module_id, label, default) in enumerate(MODULE_GROUPS[group]):
            var = tk.BooleanVar(value=default)
            self.module_vars[module_id] = var
            control = ttk.Checkbutton(modules_box, text=label, variable=var, command=self.feature_changed)
            control.grid(row=index // 2, column=index % 2, sticky="w", padx=10, pady=5)
        modules_box.columnconfigure(0, weight=1)
        modules_box.columnconfigure(1, weight=1)

        if not include_tuning:
            return

        checks = ttk.LabelFrame(parent, text="Aim policy")
        checks.pack(fill="x", padx=14, pady=(0, 12))
        for index, (name, label, default) in enumerate(BOOL_CONVARS):
            var = tk.BooleanVar(value=default)
            self.convar_vars[name] = tk.StringVar(value="1" if default else "0")
            bool_var = var
            def update_bool(n=name, v=bool_var) -> None:
                self.convar_vars[n].set("1" if v.get() else "0")
                self.feature_changed()
            ttk.Checkbutton(checks, text=label, variable=bool_var, command=update_bool).grid(row=0, column=index, sticky="w", padx=10, pady=6)

        tuning_box = ttk.LabelFrame(parent, text="Tuning")
        tuning_box.pack(fill="x", padx=14, pady=(0, 12))
        for row, (name, label, default, minimum, maximum, increment) in enumerate(TUNING):
            var = tk.StringVar(value=f"{default:g}")
            self.convar_vars[name] = var
            ttk.Label(tuning_box, text=label).grid(row=row, column=0, sticky="w", padx=10, pady=4)
            spin = ttk.Spinbox(tuning_box, textvariable=var, from_=minimum, to=maximum, increment=increment, width=14, command=self.feature_changed)
            spin.grid(row=row, column=1, sticky="w", padx=10, pady=4)
            spin.bind("<Return>", lambda _e: self.feature_changed())
            spin.bind("<FocusOut>", lambda _e: self.feature_changed())

        sort_box = ttk.LabelFrame(parent, text="Target sorting")
        sort_box.pack(fill="x", padx=14, pady=(0, 14))
        for row, (name, label, default) in enumerate(SORT_CONVARS):
            display = tk.StringVar(value=default)
            self.convar_vars[name] = tk.StringVar(value=SORT_TO_VALUE[default])
            ttk.Label(sort_box, text=label).grid(row=row, column=0, sticky="w", padx=10, pady=5)
            combo = ttk.Combobox(sort_box, textvariable=display, values=list(SORT_TO_VALUE), state="readonly", width=16)
            combo.grid(row=row, column=1, sticky="w", padx=10, pady=5)
            combo.bind("<<ComboboxSelected>>", lambda _e, n=name, d=display: self._sort_changed(n, d))

    def _build_config(self, parent: ttk.Frame) -> None:
        profile = ttk.LabelFrame(parent, text="Shared feature profile")
        profile.pack(fill="x", padx=16, pady=16)
        self._entry_row(profile, 0, "Export path", self.export_path_var, browse_file=True)
        ttk.Checkbutton(profile, text="Live export whenever a feature changes", variable=self.live_export_var,
                        command=self.feature_changed).grid(row=1, column=0, columnspan=3, sticky="w", padx=8, pady=6)
        buttons = ttk.Frame(profile)
        buttons.grid(row=2, column=0, columnspan=3, sticky="w", padx=8, pady=10)
        ttk.Button(buttons, text="Save local profile", command=self.save_profile).pack(side="left", padx=4)
        ttk.Button(buttons, text="Load local profile", command=self.load_profile).pack(side="left", padx=4)
        ttk.Button(buttons, text="Export now", command=self.export_profile).pack(side="left", padx=4)
        ttk.Button(buttons, text="Import profile...", command=self.import_profile).pack(side="left", padx=4)
        ttk.Button(buttons, text="Reset defaults", command=self.reset_defaults).pack(side="left", padx=4)

        info = ttk.LabelFrame(parent, text="How settings are applied")
        info.pack(fill="both", expand=True, padx=16, pady=(0, 16))
        text = (
            "The control panel writes desktop_profile.conf. The Kirkware addon late-load bridge watches\n"
            "garrysmod/data/kirkware_linux/desktop_profile.conf and applies module booleans through\n"
            "KW.SetModuleEnabled plus the existing client convars. Your separate simulator can watch\n"
            "the same line-based profile from any folder you choose here. Existing feature code is not\n"
            "rewritten by this UI."
        )
        ttk.Label(info, text=text, justify="left").pack(anchor="nw", padx=10, pady=10)

    def _sort_changed(self, name: str, display: tk.StringVar) -> None:
        self.convar_vars[name].set(SORT_TO_VALUE.get(display.get(), "0"))
        self.feature_changed()

    def _browse_file(self, variable: tk.StringVar) -> None:
        initial = Path(variable.get()).expanduser()
        chosen = filedialog.asksaveasfilename(initialdir=str(initial.parent if initial.parent.exists() else Path.home()),
                                              initialfile=initial.name)
        if chosen:
            variable.set(chosen)
            self._save_ui_config()

    def _browse_dir(self, variable: tk.StringVar) -> None:
        chosen = filedialog.askdirectory(initialdir=str(Path(variable.get()).expanduser()))
        if chosen:
            variable.set(chosen)
            self._save_ui_config()

    def _current_profile(self) -> tuple[dict[str, bool], dict[str, str]]:
        modules = {name: bool(var.get()) for name, var in self.module_vars.items()}
        convars = {name: var.get().strip() for name, var in self.convar_vars.items()}
        return modules, convars

    def feature_changed(self) -> None:
        self._save_ui_config()
        self.save_profile(silent=True)
        if self.live_export_var.get():
            self.export_profile(silent=True)

    def save_profile(self, silent: bool = False) -> None:
        modules, convars = self._current_profile()
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        LOCAL_PROFILE_PATH.write_text(serialize_profile(modules, convars), encoding="utf-8")
        if not silent:
            self.status_var.set(f"Saved {LOCAL_PROFILE_PATH}")

    def _load_profile(self, silent: bool = False, path: Path = LOCAL_PROFILE_PATH) -> None:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            return
        modules, convars = parse_profile(text)
        for name, value in modules.items():
            if name in self.module_vars:
                self.module_vars[name].set(value)
        for name, value in convars.items():
            if name in self.convar_vars:
                self.convar_vars[name].set(value)
        if not silent:
            self.status_var.set(f"Loaded {path}")

    def load_profile(self) -> None:
        self._load_profile(silent=False)

    def import_profile(self) -> None:
        chosen = filedialog.askopenfilename(initialdir=str(Path(self.export_path_var.get()).expanduser().parent),
                                            filetypes=[("Kirkware profile", "*.conf"), ("All files", "*")])
        if chosen:
            self._load_profile(path=Path(chosen), silent=False)
            self.feature_changed()

    def export_profile(self, silent: bool = False) -> None:
        destination = Path(self.export_path_var.get()).expanduser()
        if not destination.name:
            if not silent:
                messagebox.showerror(APP_NAME, "Choose a profile file path first.")
            return
        modules, convars = self._current_profile()
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(serialize_profile(modules, convars), encoding="utf-8")
        except OSError as exc:
            self.status_var.set(f"Export failed: {exc}")
            if not silent:
                messagebox.showerror(APP_NAME, str(exc))
            return
        self.status_var.set(f"Exported {destination}")

    def reset_defaults(self) -> None:
        for group in MODULE_GROUPS.values():
            for name, _label, default in group:
                self.module_vars[name].set(default)
        for name, _label, default, _min, _max, _step in TUNING:
            self.convar_vars[name].set(f"{default:g}")
        for name, _label, default in BOOL_CONVARS:
            self.convar_vars[name].set("1" if default else "0")
        for name, _label, default in SORT_CONVARS:
            self.convar_vars[name].set(SORT_TO_VALUE[default])
        self.feature_changed()
        self.status_var.set("Defaults restored")

    def _client_binary(self) -> Path:
        return REPO_ROOT / "build-load-harness/kirkware-load-client"

    def _target_binary(self) -> Path:
        return REPO_ROOT / "build-load-harness/kirkware-load-target"

    def _loader_env(self) -> dict[str, str]:
        env = os.environ.copy()
        env["KIRKWARE_LOAD_CONTROL_ROOT"] = str(Path(self.control_root_var.get()).expanduser())
        return env

    def _log(self, text: str) -> None:
        self.loader_output.insert("end", text)
        self.loader_output.see("end")

    def list_targets(self) -> None:
        client = self._client_binary()
        if not client.exists():
            self._log("Build the load harness first: ./build-load-harness.sh --clean\n")
            return
        result = subprocess.run([str(client), "--list"], env=self._loader_env(), text=True, capture_output=True)
        self._log((result.stdout or "") + (result.stderr or ""))

    def loader_action(self, action: str) -> None:
        pid = self.pid_var.get().strip()
        if not pid.isdigit() or int(pid) <= 0:
            self._log("Enter a positive cooperative target PID first.\n")
            return
        client = self._client_binary()
        if not client.exists():
            self._log("Build the load harness first: ./build-load-harness.sh --clean\n")
            return
        command = [str(client), "--target", pid]
        if action == "status":
            command += ["--status"]
        elif action == "load":
            module = str(Path(self.module_path_var.get()).expanduser().resolve())
            command += ["--load", module]
        elif action == "unload":
            command += ["--unload"]
        elif action == "quit":
            command += ["--quit"]
        result = subprocess.run(command, env=self._loader_env(), text=True, capture_output=True)
        self._log(f"$ {' '.join(command)}\n")
        self._log((result.stdout or "") + (result.stderr or ""))
        self.status_var.set("Loader command succeeded" if result.returncode == 0 else "Loader command failed")
        self._save_ui_config()

    def start_test_target(self) -> None:
        target = self._target_binary()
        if not target.exists():
            self._log("Build the load harness first: ./build-load-harness.sh --clean\n")
            return
        if self.test_target is not None and self.test_target.poll() is None:
            self.pid_var.set(str(self.test_target.pid))
            self._log(f"Test target already running as PID {self.test_target.pid}.\n")
            return
        self.test_target = subprocess.Popen([str(target)], env=self._loader_env(), text=True,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.pid_var.set(str(self.test_target.pid))
        self._save_ui_config()
        self._log(f"Started cooperative test target PID {self.test_target.pid}.\n")

    def close(self) -> None:
        try:
            self._save_ui_config()
            self.save_profile(silent=True)
        finally:
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
        path.write_text(encoded, encoding="utf-8")
        if parse_profile(path.read_text(encoding="utf-8")) != (modules, convars):
            print("profile file round-trip failed", file=sys.stderr)
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
