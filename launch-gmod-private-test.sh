#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GAME_DIR="${GMOD_DIR:-}"
MODE="local"
MAP_NAME="gm_construct"
KEEP_WORKSHOP=0
INSTALL_ADDON=1
DRY_RUN=0
STEAM_BIN_OVERRIDE="${STEAM_BIN:-}"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: ./launch-gmod-private-test.sh [options]

Launches a supported Kirkware live test without remote process injection.
By default it installs the local addon, disables Workshop content for isolation,
and starts a private/local Sandbox session on gm_construct with client Lua allowed.

Modes:
  --local              Start a local/private Sandbox map (default)
  --menu               Stop at the GMod main menu; connect manually later

Options:
  --map NAME           Local-test map (default: gm_construct)
  --keep-workshop      Do not pass -noworkshop
  --no-install         Do not reinstall the local kirkware_linux addon first
  --game-dir PATH      Garry's Mod directory containing garrysmod/
  --steam-bin PATH     Steam executable (or set STEAM_BIN)
  --dry-run            Print the resolved launch command without starting GMod
  -h, --help           Show this help

Examples:
  ./launch-gmod-private-test.sh
  ./launch-gmod-private-test.sh --menu
  ./launch-gmod-private-test.sh --local --map gm_flatgrass
  ./launch-gmod-private-test.sh --keep-workshop
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --local)
            MODE="local"
            ;;
        --menu)
            MODE="menu"
            ;;
        --map)
            shift
            (( $# > 0 )) || fail "--map requires a map name"
            MAP_NAME="$1"
            ;;
        --keep-workshop)
            KEEP_WORKSHOP=1
            ;;
        --no-install)
            INSTALL_ADDON=0
            ;;
        --game-dir)
            shift
            (( $# > 0 )) || fail "--game-dir requires a path"
            GAME_DIR="$1"
            ;;
        --steam-bin)
            shift
            (( $# > 0 )) || fail "--steam-bin requires a path"
            STEAM_BIN_OVERRIDE="$1"
            ;;
        --dry-run)
            DRY_RUN=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1 (use --help)"
            ;;
    esac
    shift
done

find_game_dir() {
    local candidate
    local candidates=(
        "$HOME/.local/share/Steam/steamapps/common/GarrysMod"
        "$HOME/.steam/steam/steamapps/common/GarrysMod"
        "$HOME/.steam/root/steamapps/common/GarrysMod"
        "$HOME/snap/steam/common/.local/share/Steam/steamapps/common/GarrysMod"
    )
    for candidate in "${candidates[@]}"; do
        if [[ -d "$candidate/garrysmod" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [[ -z "$GAME_DIR" ]]; then
    GAME_DIR="$(find_game_dir || true)"
fi
[[ -n "$GAME_DIR" ]] || fail \
    "Garry's Mod was not found; use --game-dir PATH or set GMOD_DIR"
GAME_DIR="$(cd -- "$GAME_DIR" 2>/dev/null && pwd)" || fail \
    "Garry's Mod directory does not exist: $GAME_DIR"
[[ -d "$GAME_DIR/garrysmod" ]] || fail \
    "expected garrysmod/ inside: $GAME_DIR"

if (( INSTALL_ADDON )); then
    [[ -x "$ROOT_DIR/install-gmod-addon.sh" ]] || \
        fail "install-gmod-addon.sh is missing or not executable"
    printf '==> Installing/updating local Kirkware addon\n'
    "$ROOT_DIR/install-gmod-addon.sh" --game-dir "$GAME_DIR"
fi

ADDON_DIR="$GAME_DIR/garrysmod/addons/kirkware_linux"
[[ -d "$ADDON_DIR/lua/autorun/client" ]] || fail \
    "Kirkware addon is not installed at $ADDON_DIR"

# -condebug appends console output. Rotate the previous log so verification only
# evaluates the session being launched now.
CONSOLE_LOG="$GAME_DIR/garrysmod/console.txt"
if [[ -f "$CONSOLE_LOG" ]]; then
    stamp="$(date +%Y%m%d-%H%M%S)"
    mv -- "$CONSOLE_LOG" "$GAME_DIR/garrysmod/console.kirkware-prev-$stamp.txt"
fi

steam_cmd=()
if [[ -n "$STEAM_BIN_OVERRIDE" ]]; then
    [[ -x "$STEAM_BIN_OVERRIDE" ]] || fail \
        "Steam executable is not executable: $STEAM_BIN_OVERRIDE"
    steam_cmd=("$STEAM_BIN_OVERRIDE")
elif command -v steam >/dev/null 2>&1; then
    steam_cmd=("$(command -v steam)")
elif command -v flatpak >/dev/null 2>&1 && \
     flatpak info com.valvesoftware.Steam >/dev/null 2>&1; then
    steam_cmd=(flatpak run com.valvesoftware.Steam)
else
    fail "Steam was not found; use --steam-bin PATH"
fi

launch_args=(
    -applaunch 4000
    -condebug
    -console
    -disableluarefresh
    +developer 1
)

if (( ! KEEP_WORKSHOP )); then
    # This disables Workshop content only. Folder addons remain available.
    launch_args+=( -noworkshop )
fi

if [[ "$MODE" == "local" ]]; then
    # A listen-server/private test environment where we control the server policy.
    launch_args+=(
        +sv_lan 1
        +sv_allowcslua 1
        +gamemode sandbox
        +map "$MAP_NAME"
    )
fi

printf '\nKIRKWARE LIVE GMOD TEST\n'
printf '%s\n' '-----------------------'
printf 'Mode:       %s\n' "$MODE"
printf 'Game:       %s\n' "$GAME_DIR"
printf 'Addon:      %s\n' "$ADDON_DIR"
printf 'Workshop:   %s\n' "$([[ $KEEP_WORKSHOP -eq 1 ]] && printf enabled || printf disabled-for-test)"
printf 'Console log:%s\n' " $CONSOLE_LOG"
if [[ "$MODE" == "local" ]]; then
    printf 'Map:        %s\n' "$MAP_NAME"
    printf 'Client Lua: allowed by the private/listen server\n'
else
    printf 'Network:    no server is joined automatically\n'
fi

printf '\nLaunch command:\n  '
printf '%q ' "${steam_cmd[@]}" "${launch_args[@]}"
printf '\n\n'

if (( DRY_RUN )); then
    printf 'Dry run only; Garry\047s Mod was not started.\n'
    exit 0
fi

printf 'After the game reaches the map/menu, verify initialization with:\n'
printf '  %q\n\n' "$ROOT_DIR/verify-gmod-live-test.sh --game-dir $GAME_DIR"

exec "${steam_cmd[@]}" "${launch_args[@]}"
