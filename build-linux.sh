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
CLEAN=0
PACKAGE=0
RUN_UI=0
JOBS="${KIRKWARE_JOBS:-}"

usage() {
    cat <<'EOF'
Usage: ./build-linux.sh [options]

Quick examples:
  ./build-linux.sh                 Build Release + run tests
  ./build-linux.sh --run           Build/test, then launch the Linux UI
  ./build-linux.sh --clean --run   Clean rebuild, test, then launch UI
  ./build-linux.sh --install-deps  Install Linux Mint/Ubuntu build deps, then build
  ./build-linux.sh --sanitize      Debug ASan/UBSan build with warnings as errors

Options:
  --run                  Launch kirkware-ui after a successful build
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

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
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

while (( $# > 0 )); do
    case "$1" in
        --run)
            RUN_UI=1
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
command -v pkg-config >/dev/null 2>&1 || \
    fail "pkg-config is missing; run ./build-linux.sh --install-deps"

if [[ "$BUILD_UI" == "ON" ]]; then
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

printf '==> Build complete\n'
printf '    CLI:       %s/kirkware\n' "$BUILD_DIR"
printf '    Component: %s/libkirkware_component.so\n' "$BUILD_DIR"
if [[ "$BUILD_UI" == "ON" ]]; then
    printf '    UI:        %s/kirkware-ui\n' "$BUILD_DIR"
fi

if (( RUN_UI )); then
    [[ -x "$BUILD_DIR/kirkware-ui" ]] || fail "kirkware-ui was not built"
    [[ -f "$BUILD_DIR/libkirkware_component.so" ]] || \
        fail "libkirkware_component.so was not built"
    printf '==> Launching kirkware-ui\n'
    exec "$BUILD_DIR/kirkware-ui" \
        --component "$BUILD_DIR/libkirkware_component.so"
fi
