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
MANAGED_PID_FILE="/tmp/kirkware-load-target-$(id -u)-managed.pid"
MANAGED_LOG_FILE="/tmp/kirkware-load-target-$(id -u)-managed.log"

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
  --client             Build, ensure one managed target is running, then open loader UI
  --demo               Build, start a temporary target, then open loader UI
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
        printf '%s[3/3]%s Tests skipped for run session\n' "$DIM" "$RESET"
    fi

    printf '\n%sBuild ready%s\n' "$GOOD" "$RESET"
    printf '  target: %s/kirkware-load-target\n' "$BUILD_DIR"
    printf '  client: %s/kirkware-load-client\n' "$BUILD_DIR"
    printf '  module: %s/libkirkware_load_test.so\n' "$BUILD_DIR"
}

managed_target_pid() {
    [[ -f "$MANAGED_PID_FILE" ]] || return 1

    local pid
    read -r pid < "$MANAGED_PID_FILE" || return 1
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1

    local socket_path="/tmp/kirkware-load-target-$(id -u)-${pid}.sock"
    if kill -0 "$pid" 2>/dev/null && [[ -S "$socket_path" ]]; then
        printf '%s\n' "$pid"
        return 0
    fi

    rm -f -- "$MANAGED_PID_FILE" "$socket_path"
    return 1
}

start_managed_target() {
    local existing_pid
    if existing_pid="$(managed_target_pid)"; then
        printf '%sManaged target already running%s (PID %s)\n' \
            "$GOOD" "$RESET" "$existing_pid"
        return 0
    fi

    printf 'Starting persistent controlled target...\n'
    nohup "$BUILD_DIR/kirkware-load-target" >"$MANAGED_LOG_FILE" 2>&1 &
    local target_pid=$!
    printf '%s\n' "$target_pid" > "$MANAGED_PID_FILE"

    local socket_path="/tmp/kirkware-load-target-$(id -u)-${target_pid}.sock"
    for _ in {1..80}; do
        if ! kill -0 "$target_pid" 2>/dev/null; then
            printf 'Managed target exited during startup.\n' >&2
            [[ -f "$MANAGED_LOG_FILE" ]] && cat "$MANAGED_LOG_FILE" >&2
            rm -f -- "$MANAGED_PID_FILE" "$socket_path"
            return 1
        fi
        if [[ -S "$socket_path" ]]; then
            printf '%sManaged target ready%s\n' "$GOOD" "$RESET"
            printf '  PID:    %s\n' "$target_pid"
            printf '  Socket: %s\n' "$socket_path"
            printf '  Log:    %s\n' "$MANAGED_LOG_FILE"
            return 0
        fi
        sleep 0.05
    done

    kill "$target_pid" 2>/dev/null || true
    rm -f -- "$MANAGED_PID_FILE" "$socket_path"
    fail "managed target socket did not appear"
}

run_target() {
    exec "$BUILD_DIR/kirkware-load-target"
}

run_client() {
    start_managed_target

    local target_pid
    target_pid="$(managed_target_pid)" || fail "managed target disappeared before client startup"

    printf 'Opening loader directly for managed target PID %s\n' "$target_pid"
    "$BUILD_DIR/kirkware-load-client" --target "$target_pid"
}

run_self_test() {
    "$BUILD_DIR/kirkware-load-target" --self-test "$BUILD_DIR/libkirkware_load_test.so"
}

run_demo() {
    banner
    printf 'Starting temporary controlled target...\n'
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

    printf '\nTemporary target PID %s is ready. Opening loader directly...\n' "$target_pid"
    "$BUILD_DIR/kirkware-load-client" --target "$target_pid"

    cleanup_demo
    trap - EXIT INT TERM
}

interactive_menu() {
    while true; do
        banner
        local managed_status="not running"
        local running_pid
        if running_pid="$(managed_target_pid)"; then
            managed_status="PID $running_pid"
        fi

        cat <<EOF
  Managed target: $managed_status

  1. Build + test
  2. Clean build + test
  3. Start foreground controlled target
  4. Open loader UI (persistent managed target)
  5. Full temporary demo session
  6. Sanitizer build + test
  7. Direct .so self-test
  0. Exit
EOF
        printf '\nSelect: '
        read -r choice || return 0

        case "$choice" in
            1)
                CLEAN=0
                RUN_TESTS=1
                BUILD_TYPE="Release"
                SANITIZERS="OFF"
                build_harness
                ;;
            2)
                CLEAN=1
                RUN_TESTS=1
                BUILD_TYPE="Release"
                SANITIZERS="OFF"
                build_harness
                CLEAN=0
                ;;
            3)
                RUN_TESTS=0
                build_harness
                run_target
                ;;
            4)
                RUN_TESTS=0
                build_harness
                run_client
                ;;
            5)
                RUN_TESTS=0
                build_harness
                run_demo
                ;;
            6)
                CLEAN=1
                RUN_TESTS=1
                BUILD_TYPE="Debug"
                SANITIZERS="ON"
                build_harness
                CLEAN=0
                ;;
            7)
                RUN_TESTS=0
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
            RUN_TESTS=0
            ;;
        --client)
            ACTION="client"
            RUN_TESTS=0
            ;;
        --demo)
            ACTION="demo"
            RUN_TESTS=0
            ;;
        --self-test)
            ACTION="self-test"
            RUN_TESTS=0
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
