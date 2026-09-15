#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$ROOT_DIR/source/gmod_addon/kirkware_linux"
GAME_DIR="${GMOD_DIR:-}"
DISCOVERY_BIN="${KIRKWARE_DISCOVERY_BIN:-$ROOT_DIR/build-linux/kirkware}"
UNINSTALL=0

usage() {
    cat <<'EOF'
Usage: ./install-gmod-addon.sh [options]

Installs the supported Kirkware Linux Garry's Mod addon.

Options:
  --game-dir PATH   Garry's Mod install directory (the folder containing garrysmod/)
  --uninstall       Remove only the kirkware_linux addon directory
  -h, --help        Show this help

You can also set GMOD_DIR to the Garry's Mod install directory.
EOF
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

while (( $# > 0 )); do
    case "$1" in
        --game-dir)
            shift
            (( $# > 0 )) || fail "--game-dir requires a path"
            GAME_DIR="$1"
            ;;
        --uninstall)
            UNINSTALL=1
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
    local candidate=""

    if [[ -x "$DISCOVERY_BIN" ]]; then
        candidate="$(
            "$DISCOVERY_BIN" --game-info --no-workspace 2>/dev/null |
                sed -n 's/^install-root=//p' |
                head -n 1
        )"
        if [[ -n "$candidate" && -d "$candidate/garrysmod" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    local candidates=(
        "$HOME/.local/share/Steam/steamapps/common/GarrysMod"
        "$HOME/.steam/steam/steamapps/common/GarrysMod"
        "$HOME/.steam/debian-installation/steamapps/common/GarrysMod"
        "$HOME/.steam/root/steamapps/common/GarrysMod"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/GarrysMod"
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
    "expected a garrysmod/ directory inside: $GAME_DIR"

ADDONS_DIR="$GAME_DIR/garrysmod/addons"
TARGET_DIR="$ADDONS_DIR/kirkware_linux"

if (( UNINSTALL )); then
    if [[ -e "$TARGET_DIR" ]]; then
        rm -rf -- "$TARGET_DIR"
        printf 'Removed %s\n' "$TARGET_DIR"
    else
        printf 'Kirkware Linux addon is not installed at %s\n' "$TARGET_DIR"
    fi
    exit 0
fi

[[ -d "$SOURCE_DIR/lua/autorun/client" ]] || fail \
    "addon source is missing: $SOURCE_DIR"

mkdir -p -- "$ADDONS_DIR"
STAGE_DIR="$ADDONS_DIR/.kirkware_linux.stage.$$"
cleanup_stage() {
    rm -rf -- "$STAGE_DIR"
}
trap cleanup_stage EXIT

rm -rf -- "$STAGE_DIR"
cp -a -- "$SOURCE_DIR" "$STAGE_DIR"

# Replace only our own addon directory. Never touch any other Garry's Mod addon.
rm -rf -- "$TARGET_DIR"
mv -- "$STAGE_DIR" "$TARGET_DIR"
trap - EXIT

printf 'Installed Kirkware Linux addon to:\n  %s\n' "$TARGET_DIR"
printf 'Restart Garry\047s Mod if it is already running. Press Insert in-game to open the menu.\n'
