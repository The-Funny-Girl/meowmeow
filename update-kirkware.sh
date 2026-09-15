#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

REMOTE="${KIRKWARE_REMOTE:-origin}"
BRANCH="${KIRKWARE_BRANCH:-main}"
STASH_LOCAL=0
BUILD_AFTER=0
INSTALL_ADDON=0
RUN_AFTER=0
GAME_DIR=""

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: ./update-kirkware.sh [options]

Safely update this downloaded Kirkware repository from GitHub.

Default behavior:
  - fetch origin
  - switch to main
  - fast-forward main to origin/main
  - never discard local changes

Examples:
  ./update-kirkware.sh
  ./update-kirkware.sh --install-addon
  ./update-kirkware.sh --build --install-addon
  ./update-kirkware.sh --install-addon --run
  ./update-kirkware.sh --stash --install-addon

Options:
  --branch NAME         Update a branch other than main
  --stash               Temporarily stash tracked + untracked local changes,
                        update, then re-apply them
  --build               Build and test after updating
  --install-addon       Install/update the Garry's Mod addon after updating
  --game-dir PATH       Garry's Mod directory for --install-addon
  --run                 Build/test and launch kirkware-ui after updating
                        (implies --build)
  -h, --help            Show this help

Environment overrides:
  KIRKWARE_REMOTE       Git remote name (default: origin)
  KIRKWARE_BRANCH       Default branch (default: main)
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --branch)
            shift
            (( $# > 0 )) || fail "--branch requires a branch name"
            BRANCH="$1"
            ;;
        --stash)
            STASH_LOCAL=1
            ;;
        --build)
            BUILD_AFTER=1
            ;;
        --install-addon)
            INSTALL_ADDON=1
            ;;
        --game-dir)
            shift
            (( $# > 0 )) || fail "--game-dir requires a path"
            GAME_DIR="$1"
            ;;
        --run)
            RUN_AFTER=1
            BUILD_AFTER=1
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

[[ -z "$GAME_DIR" || $INSTALL_ADDON -eq 1 ]] || \
    fail "--game-dir is only valid with --install-addon"

command -v git >/dev/null 2>&1 || fail "git is not installed"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
    fail "this script must be inside a Git checkout"
git remote get-url "$REMOTE" >/dev/null 2>&1 || \
    fail "Git remote '$REMOTE' does not exist"

if [[ "$BRANCH" == -* || "$BRANCH" == *' '* ]]; then
    fail "invalid branch name: $BRANCH"
fi

stashed=0
if [[ -n "$(git status --porcelain)" ]]; then
    if (( STASH_LOCAL )); then
        printf '==> Saving local changes before update\n'
        git stash push --include-untracked \
            -m "kirkware updater $(date '+%Y-%m-%d %H:%M:%S')"
        stashed=1
    else
        printf 'Local changes were found. Nothing was modified.\n\n' >&2
        git status --short >&2
        printf '\nCommit them, discard them yourself, or rerun with --stash.\n' >&2
        exit 2
    fi
fi

printf '==> Fetching %s\n' "$REMOTE"
git fetch --prune "$REMOTE"

git show-ref --verify --quiet "refs/remotes/$REMOTE/$BRANCH" || \
    fail "remote branch '$REMOTE/$BRANCH' does not exist"

if git show-ref --verify --quiet "refs/heads/$BRANCH"; then
    printf '==> Switching to %s\n' "$BRANCH"
    git switch "$BRANCH"
else
    printf '==> Creating local %s from %s/%s\n' "$BRANCH" "$REMOTE" "$BRANCH"
    git switch --track -c "$BRANCH" "$REMOTE/$BRANCH"
fi

before="$(git rev-parse --short HEAD)"
printf '==> Fast-forwarding to %s/%s\n' "$REMOTE" "$BRANCH"
if ! git merge --ff-only "$REMOTE/$BRANCH"; then
    printf '\nUpdate stopped because the local branch has diverged.\n' >&2
    printf 'No commits were discarded. Resolve/rebase the local commits manually.\n' >&2
    exit 3
fi
after="$(git rev-parse --short HEAD)"

if [[ -f .gitmodules ]]; then
    printf '==> Updating submodules\n'
    git submodule update --init --recursive
fi

if (( stashed )); then
    printf '==> Re-applying local changes\n'
    if ! git stash pop; then
        printf '\nThe repository update succeeded, but your stashed changes conflicted.\n' >&2
        printf 'Your stash is still available; resolve the conflicts before continuing.\n' >&2
        exit 4
    fi
fi

printf '==> Repository updated: %s -> %s\n' "$before" "$after"

if (( BUILD_AFTER )); then
    [[ -x "$ROOT_DIR/build-linux.sh" ]] || \
        fail "build-linux.sh is missing or not executable"

    build_args=()
    if (( INSTALL_ADDON )); then
        build_args+=(--install-addon)
        if [[ -n "$GAME_DIR" ]]; then
            build_args+=(--game-dir "$GAME_DIR")
        fi
    fi
    if (( RUN_AFTER )); then
        build_args+=(--run)
    fi

    printf '==> Building updated source\n'
    "$ROOT_DIR/build-linux.sh" "${build_args[@]}"
elif (( INSTALL_ADDON )); then
    [[ -x "$ROOT_DIR/install-gmod-addon.sh" ]] || \
        fail "install-gmod-addon.sh is missing or not executable"

    installer_args=()
    if [[ -n "$GAME_DIR" ]]; then
        installer_args+=(--game-dir "$GAME_DIR")
    fi

    printf '==> Installing updated Garry\047s Mod addon\n'
    "$ROOT_DIR/install-gmod-addon.sh" "${installer_args[@]}"
fi

printf '==> Done\n'
