#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

BUILD_TYPE="Release"
BUILD_DIR=""
BUILD_DIR_EXPLICIT=0
BUILD_TESTS="ON"
BUILD_UI="ON"
ENABLE_SANITIZERS="OFF"
WARNINGS_AS_ERRORS="OFF"
INSTALL_DEPS=0
INSTALL_ADDON=0
GAME_DIR_OVERRIDE=""
CLEAN=0
PACKAGE=0
RUN_UI=0
JOBS="${KIRKWARE_JOBS:-}"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

have_color() {
    [[ -t 1 && "${TERM:-dumb}" != "dumb" ]]
}

if have_color; then
    UI_BOLD=$'\033[1m'
    UI_DIM=$'\033[2m'
    UI_ACCENT=$'\033[36m'
    UI_GOOD=$'\033[32m'
    UI_RESET=$'\033[0m'
else
    UI_BOLD=""
    UI_DIM=""
    UI_ACCENT=""
    UI_GOOD=""
    UI_RESET=""
fi

ui_banner() {
    if have_color; then
        printf '\033[2J\033[H'
    fi
    printf '%s%sKIRKWARE LINUX BUILDER%s\n' "$UI_BOLD" "$UI_ACCENT" "$UI_RESET"
    printf '%sBuild, test, install and launch%s\n' "$UI_DIM" "$UI_RESET"
    printf '%s\n' '----------------------------------------'
}

run_menu_command() {
    printf '\n'
    if env KIRKWARE_NONINTERACTIVE=1 "$@"; then
        printf '\n%sCompleted successfully.%s\n' "$UI_GOOD" "$UI_RESET"
    else
        local rc=$?
        printf '\nCommand failed with exit code %s.\n' "$rc" >&2
    fi
    printf 'Press Enter to return to the menu...'
    read -r _ || true
}

interactive_menu() {
    while true; do
        ui_banner
        cat <<'EOF'
  1. Release build + tests
  2. Clean Release build + tests
  3. Build + install GMod addon
  4. Build + install addon + launch desktop UI
  5. Debug build + tests
  6. Sanitizer build + tests
  7. Build + create packages
  8. Install dependencies + build
  9. Cooperative .so load harness
  0. Exit
EOF
        printf '\nSelect: '
        read -r choice || return 0

        case "$choice" in
            1)
                run_menu_command "$ROOT_DIR/build-linux.sh"
                ;;
            2)
                run_menu_command "$ROOT_DIR/build-linux.sh" --clean
                ;;
            3)
                run_menu_command "$ROOT_DIR/build-linux.sh" --install-addon
                ;;
            4)
                run_menu_command "$ROOT_DIR/build-linux.sh" --install-addon --run
                ;;
            5)
                run_menu_command "$ROOT_DIR/build-linux.sh" --debug
                ;;
            6)
                run_menu_command "$ROOT_DIR/build-linux.sh" --sanitize
                ;;
            7)
                run_menu_command "$ROOT_DIR/build-linux.sh" --package
                ;;
            8)
                run_menu_command "$ROOT_DIR/build-linux.sh" --install-deps
                ;;
            9)
                [[ -x "$ROOT_DIR/build-load-harness.sh" ]] || \
                    fail "build-load-harness.sh is missing or not executable"
                "$ROOT_DIR/build-load-harness.sh"
                ;;
            0)
                return 0
                ;;
            *)
                printf 'Invalid selection. Press Enter...'
                read -r _ || true
                ;;
        esac
    done
}

usage() {
    cat <<'EOF'
Usage: ./build-linux.sh [options]

Run with no arguments in an interactive terminal to open the builder menu.
Non-interactive no-argument use keeps the original Release build + test behavior.

Quick examples:
  ./build-linux.sh                         Interactive menu in a terminal
  ./build-linux.sh --run                   Build/test, then launch the Linux UI
  ./build-linux.sh --install-addon         Build/test and install the in-game menu
  ./build-linux.sh --install-addon --run   Install the in-game menu, then launch UI
  ./build-linux.sh --clean --run           Clean rebuild, test, then launch UI
  ./build-linux.sh --install-deps          Install Linux Mint/Ubuntu build deps, then build
  ./build-linux.sh --sanitize              Debug ASan/UBSan build with warnings as errors

Options:
  --run                  Launch kirkware-ui after a successful build
  --install-addon        Install/update the supported Garry's Mod in-game addon
  --game-dir PATH        Garry's Mod directory for --install-addon
  --clean                Remove the selected build directory first
  --install-deps         Install Debian/Ubuntu/Linux Mint build dependencies
  --debug                Build Debug instead of Release
  --sanitize             Build Debug with ASan/UBSan and -Werror
  --package              Also create TGZ and DEB packages
  --no-tests             Skip building/running tests
  --no-ui                Build CLI/component only
  --build-dir PATH       Override the build directory
  --jobs N               Parallel build jobs (default: CPU count)
  -h, --help             Show this help
EOF
}

install_deps() {
    command -v apt-get >/dev/null 2>&1 || \
        fail "--install-deps currently supports apt-based systems only"

    local elevate=()
    if (( EUID != 0 )); then
        command -v sudo >/dev/null 2>&1 || \
            fail "sudo is required to install dependencies"
        elevate=(sudo)
    fi

    "${elevate[@]}" apt-get update
    "${elevate[@]}" apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        libsdl2-dev \
        libsdl2-image-dev \
        libgl1-mesa-dev \
        libfreetype6-dev
}

if (( $# == 0 )) && [[ -t 0 && -t 1 ]] && [[ "${KIRKWARE_NONINTERACTIVE:-0}" != "1" ]]; then
    interactive_menu
    exit 0
fi

while (( $# > 0 )); do
    case "$1" in
        --run)
            RUN_UI=1
            ;;
        --install-addon)
            INSTALL_ADDON=1
            ;;
        --game-dir)
            shift
            (( $# > 0 )) || fail "--game-dir requires a path"
            GAME_DIR_OVERRIDE="$1"
            ;;
        --clean)
            CLEAN=1
            ;;
        --install-deps)
            INSTALL_DEPS=1
            ;;
        --debug)
            BUILD_TYPE="Debug"
            ;;
        --sanitize)
            BUILD_TYPE="Debug"
            ENABLE_SANITIZERS="ON"
            WARNINGS_AS_ERRORS="ON"
            ;;
        --package)
            PACKAGE=1
            ;;
        --no-tests)
            BUILD_TESTS="OFF"
            ;;
        --no-ui)
            BUILD_UI="OFF"
            ;;
        --build-dir)
            shift
            (( $# > 0 )) || fail "--build-dir requires a path"
            BUILD_DIR="$1"
            BUILD_DIR_EXPLICIT=1
            ;;
        --jobs)
            shift
            (( $# > 0 )) || fail "--jobs requires a positive integer"
            JOBS="$1"
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

if [[ -n "$GAME_DIR_OVERRIDE" ]] && (( ! INSTALL_ADDON )); then
    fail "--game-dir is only valid with --install-addon"
fi

if (( INSTALL_DEPS )); then
    install_deps
fi

if [[ -z "$JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')"
    else
        JOBS="2"
    fi
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"

if (( ! BUILD_DIR_EXPLICIT )); then
    if [[ "$ENABLE_SANITIZERS" == "ON" ]]; then
        BUILD_DIR="$ROOT_DIR/build-sanitize"
    elif [[ "$BUILD_TYPE" == "Debug" ]]; then
        BUILD_DIR="$ROOT_DIR/build-linux-debug"
    else
        BUILD_DIR="$ROOT_DIR/build-linux"
    fi
elif [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
fi

if (( RUN_UI )) && [[ "$BUILD_UI" != "ON" ]]; then
    fail "--run cannot be combined with --no-ui"
fi

command -v cmake >/dev/null 2>&1 || \
    fail "cmake is missing; run ./build-linux.sh --install-deps"

if [[ "$BUILD_UI" == "ON" ]]; then
    command -v pkg-config >/dev/null 2>&1 || \
        fail "pkg-config is missing; run ./build-linux.sh --install-deps"
    missing_modules=()
    for module in sdl2 SDL2_image freetype2; do
        if ! pkg-config --exists "$module"; then
            missing_modules+=("$module")
        fi
    done
    if (( ${#missing_modules[@]} > 0 )); then
        fail "missing UI dependencies (${missing_modules[*]}); run ./build-linux.sh --install-deps"
    fi
fi

if (( CLEAN )); then
    [[ -n "$BUILD_DIR" && "$BUILD_DIR" != "/" && "$BUILD_DIR" != "$ROOT_DIR" ]] || \
        fail "refusing to clean unsafe build directory: $BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
fi

cmake_args=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DKIRKWARE_BUILD_TESTS=$BUILD_TESTS"
    "-DKIRKWARE_BUILD_UI=$BUILD_UI"
    "-DKIRKWARE_ENABLE_SANITIZERS=$ENABLE_SANITIZERS"
    "-DKIRKWARE_WARNINGS_AS_ERRORS=$WARNINGS_AS_ERRORS"
)

# Reuse the existing generator when reconfiguring. For a fresh build prefer Ninja.
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
    cmake_args+=( -G Ninja )
fi

printf '==> Configuring %s build in %s\n' "$BUILD_TYPE" "$BUILD_DIR"
cmake "${cmake_args[@]}"

printf '==> Building with %s job(s)\n' "$JOBS"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [[ "$BUILD_TESTS" == "ON" ]]; then
    printf '==> Running tests\n'
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

if (( PACKAGE )); then
    command -v cpack >/dev/null 2>&1 || fail "cpack is missing"
    PACKAGE_DIR="$BUILD_DIR/packages"
    mkdir -p -- "$PACKAGE_DIR"
    printf '==> Creating TGZ package\n'
    cpack --config "$BUILD_DIR/CPackConfig.cmake" -G TGZ -B "$PACKAGE_DIR"
    if command -v dpkg-deb >/dev/null 2>&1; then
        printf '==> Creating DEB package\n'
        cpack --config "$BUILD_DIR/CPackConfig.cmake" -G DEB -B "$PACKAGE_DIR"
    else
        printf '==> dpkg-deb not found; skipping DEB package\n'
    fi
fi

if (( INSTALL_ADDON )); then
    [[ -x "$ROOT_DIR/install-gmod-addon.sh" ]] || \
        fail "install-gmod-addon.sh is missing or not executable"
    installer_args=()
    if [[ -n "$GAME_DIR_OVERRIDE" ]]; then
        installer_args+=( --game-dir "$GAME_DIR_OVERRIDE" )
    fi
    printf '==> Installing Garry\047s Mod in-game addon\n'
    "$ROOT_DIR/install-gmod-addon.sh" "${installer_args[@]}"
fi

printf '==> Build complete\n'
printf '    CLI:       %s/kirkware\n' "$BUILD_DIR"
printf '    Component: %s/libkirkware_component.so\n' "$BUILD_DIR"
if [[ "$BUILD_UI" == "ON" ]]; then
    printf '    UI:        %s/kirkware-ui\n' "$BUILD_DIR"
fi
if (( INSTALL_ADDON )); then
    printf '    In-game:   press Insert after Garry\047s Mod loads\n'
fi

if (( RUN_UI )); then
    [[ -x "$BUILD_DIR/kirkware-ui" ]] || fail "kirkware-ui was not built"
    [[ -f "$BUILD_DIR/libkirkware_component.so" ]] || \
        fail "libkirkware_component.so was not built"
    printf '==> Launching kirkware-ui\n'
    exec "$BUILD_DIR/kirkware-ui" \
        --component "$BUILD_DIR/libkirkware_component.so"
fi
