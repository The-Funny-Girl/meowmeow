#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$ROOT_DIR/source/load_harness"
BUILD_DIR="$ROOT_DIR/build-load-harness"
BUILD_TYPE="Release"
SANITIZERS="OFF"
CLEAN=0
RUN_TESTS=1
ACTION="build"
JOBS="${KIRKWARE_JOBS:-}"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

have_color() {
    [[ -t 1 && "${TERM:-dumb}" != "dumb" ]]
}

if have_color; then
    BOLD=$'\033[1m'
    DIM=$'\033[2m'
    ACCENT=$'\033[36m'
    GOOD=$'\033[32m'
    RESET=$'\033[0m'
else
    BOLD=""
    DIM=""
    ACCENT=""
    GOOD=""
    RESET=""
fi

banner() {
    printf '\n%s%sKIRKWARE LINUX LOAD HARNESS%s\n' "$BOLD" "$ACCENT" "$RESET"
    printf '%sCooperative .so loading for a process we control%s\n' "$DIM" "$RESET"
    printf '%s\n' '-----------------------------------------------'
}

usage() {
    cat <<'EOF'
Usage: ./build-load-harness.sh [options]

With no arguments in a terminal, an interactive menu is shown.

Options:
  --clean              Clean before building
  --debug              Build Debug
  --sanitize           Debug build with ASan/UBSan
  --no-tests           Skip CTest
  --target             Build, then run the cooperative target
  --client             Build, then open the loader terminal UI
  --demo               Build, start a target, then open the loader UI
  --self-test          Build and run the direct dlopen/dlclose smoke test
  --build-dir PATH     Override build directory
  --jobs N             Parallel build jobs
  -h, --help           Show this help
EOF
}

resolve_jobs() {
    if [[ -n "$JOBS" ]]; then
        [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
        return
    fi
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    else
        JOBS="2"
    fi
}

build_harness() {
    command -v cmake >/dev/null 2>&1 || fail "cmake is required"
    resolve_jobs

    if (( CLEAN )); then
        [[ -n "$BUILD_DIR" && "$BUILD_DIR" != "/" && "$BUILD_DIR" != "$ROOT_DIR" ]] || \
            fail "refusing to clean unsafe build directory"
        rm -rf -- "$BUILD_DIR"
    fi

    cmake_args=(
        -S "$SOURCE_DIR"
        -B "$BUILD_DIR"
        "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
        "-DKIRKWARE_LOAD_HARNESS_SANITIZERS=$SANITIZERS"
        -DKIRKWARE_LOAD_HARNESS_WERROR=ON
    )
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
        cmake_args+=( -G Ninja )
    fi

    banner
    printf '%s[1/3]%s Configure %s build\n' "$ACCENT" "$RESET" "$BUILD_TYPE"
    cmake "${cmake_args[@]}"

    printf '%s[2/3]%s Build with %s job(s)\n' "$ACCENT" "$RESET" "$JOBS"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"

    if (( RUN_TESTS )); then
        printf '%s[3/3]%s Run tests\n' "$ACCENT" "$RESET"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    else
        printf '%s[3/3]%s Tests skipped\n' "$DIM" "$RESET"
    fi

    printf '\n%sBuild ready%s\n' "$GOOD" "$RESET"
    printf '  target: %s/kirkware-load-target\n' "$BUILD_DIR"
    printf '  client: %s/kirkware-load-client\n' "$BUILD_DIR"
    printf '  module: %s/libkirkware_load_test.so\n' "$BUILD_DIR"
}

run_target() {
    exec "$BUILD_DIR/kirkware-load-target"
}

run_client() {
    exec "$BUILD_DIR/kirkware-load-client"
}

run_self_test() {
    "$BUILD_DIR/kirkware-load-target" --self-test "$BUILD_DIR/libkirkware_load_test.so"
}

run_demo() {
    banner
    printf 'Starting controlled target...\n'
    "$BUILD_DIR/kirkware-load-target" &
    local target_pid=$!
    local socket_path="/tmp/kirkware-load-target-$(id -u)-${target_pid}.sock"

    cleanup_demo() {
        if kill -0 "$target_pid" 2>/dev/null; then
            kill "$target_pid" 2>/dev/null || true
            wait "$target_pid" 2>/dev/null || true
        fi
        rm -f -- "$socket_path"
    }
    trap cleanup_demo EXIT INT TERM

    for _ in {1..50}; do
        [[ -S "$socket_path" ]] && break
        sleep 0.05
    done
    [[ -S "$socket_path" ]] || fail "target socket did not appear"

    printf '\nTarget PID %s is ready. Opening loader UI...\n' "$target_pid"
    "$BUILD_DIR/kirkware-load-client"

    cleanup_demo
    trap - EXIT INT TERM
}

interactive_menu() {
    while true; do
        banner
        cat <<EOF
  1. Build + test
  2. Clean build + test
  3. Start controlled target
  4. Open loader UI
  5. Full demo session
  6. Sanitizer build + test
  7. Direct .so self-test
  0. Exit
EOF
        printf '\nSelect: '
        read -r choice || return 0

        case "$choice" in
            1)
                CLEAN=0
                BUILD_TYPE="Release"
                SANITIZERS="OFF"
                build_harness
                ;;
            2)
                CLEAN=1
                BUILD_TYPE="Release"
                SANITIZERS="OFF"
                build_harness
                CLEAN=0
                ;;
            3)
                build_harness
                run_target
                ;;
            4)
                build_harness
                run_client
                ;;
            5)
                build_harness
                run_demo
                ;;
            6)
                CLEAN=1
                BUILD_TYPE="Debug"
                SANITIZERS="ON"
                build_harness
                CLEAN=0
                ;;
            7)
                build_harness
                run_self_test
                ;;
            0)
                return 0
                ;;
            *)
                printf 'Invalid selection.\n'
                ;;
        esac

        printf '\nPress Enter to continue...'
        read -r _ || true
    done
}

if (( $# == 0 )) && [[ -t 0 ]]; then
    interactive_menu
    exit 0
fi

while (( $# > 0 )); do
    case "$1" in
        --clean)
            CLEAN=1
            ;;
        --debug)
            BUILD_TYPE="Debug"
            ;;
        --sanitize)
            BUILD_TYPE="Debug"
            SANITIZERS="ON"
            ;;
        --no-tests)
            RUN_TESTS=0
            ;;
        --target)
            ACTION="target"
            ;;
        --client)
            ACTION="client"
            ;;
        --demo)
            ACTION="demo"
            ;;
        --self-test)
            ACTION="self-test"
            ;;
        --build-dir)
            shift
            (( $# > 0 )) || fail "--build-dir requires a path"
            BUILD_DIR="$1"
            [[ "$BUILD_DIR" == /* ]] || BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
            ;;
        --jobs)
            shift
            (( $# > 0 )) || fail "--jobs requires a number"
            JOBS="$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
    shift
done

build_harness
case "$ACTION" in
    build)
        ;;
    target)
        run_target
        ;;
    client)
        run_client
        ;;
    demo)
        run_demo
        ;;
    self-test)
        run_self_test
        ;;
    *)
        fail "internal action error"
        ;;
esac
