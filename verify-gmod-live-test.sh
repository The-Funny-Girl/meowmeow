#!/usr/bin/env bash
set -Eeuo pipefail

GAME_DIR="${GMOD_DIR:-}"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: ./verify-gmod-live-test.sh [options]

Checks the current -condebug console.txt for Kirkware client initialization.

Options:
  --game-dir PATH   Garry's Mod directory containing garrysmod/
  -h, --help        Show this help
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --game-dir)
            shift
            (( $# > 0 )) || fail "--game-dir requires a path"
            GAME_DIR="$1"
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

LOG="$GAME_DIR/garrysmod/console.txt"
[[ -f "$LOG" ]] || fail \
    "console.txt was not found; launch with ./launch-gmod-private-test.sh first"

printf 'KIRKWARE LIVE GMOD VERIFICATION\n'
printf '%s\n' '-------------------------------'
printf 'Log: %s\n\n' "$LOG"

checks=(
    "base|[kirkware linux] loaded; press Insert to toggle the menu"
    "combat|[kirkware linux] combat modules loaded"
    "player visuals|[kirkware linux] player visual parity loaded"
    "viewmodel/tracer|[kirkware linux] viewmodel/tracer modules loaded"
    "desktop profile|[kirkware linux] desktop profile bridge loaded"
)

passed=0
failed=0
for entry in "${checks[@]}"; do
    label="${entry%%|*}"
    needle="${entry#*|}"
    if grep -Fq -- "$needle" "$LOG"; then
        printf '[PASS] %s\n' "$label"
        ((passed += 1))
    else
        printf '[MISS] %s\n' "$label"
        ((failed += 1))
    fi
done

printf '\nRecent Kirkware console lines:\n'
grep -F -- '[kirkware linux]' "$LOG" | tail -n 30 || true

printf '\nPotential Lua errors mentioning Kirkware:\n'
if grep -iE 'kirkware.*(error|failed)|(?:error|failed).*kirkware' "$LOG" | tail -n 20; then
    true
else
    printf '  none found\n'
fi

printf '\nSummary: %d passed, %d missing\n' "$passed" "$failed"

if (( failed == 0 )); then
    printf 'Kirkware initialized in the live GMod client.\n'
    exit 0
fi

printf 'One or more expected client modules did not report initialization.\n' >&2
exit 1
