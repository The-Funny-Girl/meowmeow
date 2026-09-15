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
TARGET_PID=""
CONTROL_ROOT="${KIRKWARE_LOAD_CONTROL_ROOT:-/tmp}"
MANAGED_PID_FILE=""
MANAGED_LOG_FILE=""

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

normalize_control_root() {
    local path="$1"
    if [[ "$path" == "~" ]]; then
        path="$HOME"
    elif [[ "$path" == "~/"* ]]; then
        path="$HOME/${path#~/}"
    fi

    mkdir -p -- "$path" || fail "cannot create control root: $path"
    [[ -d "$path" ]] || fail "control root is not a directory: $path"
    (cd -- "$path" && pwd -P)
}

refresh_managed_paths() {
    local key
    key="$(printf '%s' "$CONTROL_ROOT" | cksum | awk '{print $1}')"
    MANAGED_PID_FILE="/tmp/kirkware-load-target-$(id -u)-managed-${key}.pid"
    MANAGED_LOG_FILE="/tmp/kirkware-load-target-$(id -u)-managed-${key}.log"
}

set_control_root() {
    CONTROL_ROOT="$(normalize_control_root "$1")"
    export KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT"
    refresh_managed_paths
}

set_control_root "$CONTROL_ROOT"

banner() {
    printf '\n%s%sKIRKWARE LINUX LOAD HARNESS%s\n' "$BOLD" "$ACCENT" "$RESET"
    printf '%sCooperative .so loading for a process we control%s\n' "$DIM" "$RESET"
    printf '%sNo Unix socket: private control files + SIGUSR1%s\n' "$DIM" "$RESET"
    printf '%sControl root: %s%s\n' "$DIM" "$CONTROL_ROOT" "$RESET"
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
  --pid N              Build, then open loader UI against an existing PID
  --control-root PATH  Parent folder for per-PID control directories (default: /tmp)
  --demo               Build, start a temporary target, then open loader UI
  --self-test          Build and run the direct dlopen/dlclose smoke test
  --build-dir PATH     Override build directory
  --jobs N             Parallel build jobs
  -h, --help           Show this help

Environment:
  KIRKWARE_LOAD_CONTROL_ROOT  Same as --control-root

Example:
  ./build-load-harness.sh --control-root ~/Downloads --client

The selected PID must still be a cooperating kirkware-load-target process.
Transport uses a private per-PID control directory plus SIGUSR1; no socket is used.
EOF
}

control_dir_for_pid() {
    local pid="$1"
    printf '%s/kirkware-load-target-%s-%s\n' "$CONTROL_ROOT" "$(id -u)" "$pid"
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
    printf '  root:   %s\n' "$CONTROL_ROOT"
}

managed_target_pid() {
    [[ -f "$MANAGED_PID_FILE" ]] || return 1

    local pid
    read -r pid < "$MANAGED_PID_FILE" || return 1
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1

    local control_dir
    control_dir="$(control_dir_for_pid "$pid")"
    if kill -0 "$pid" 2>/dev/null && [[ -d "$control_dir" ]]; then
        printf '%s\n' "$pid"
        return 0
    fi

    rm -f -- "$MANAGED_PID_FILE"
    rm -rf -- "$control_dir"
    return 1
}

start_managed_target() {
    local existing_pid
    if existing_pid="$(managed_target_pid)"; then
        printf '%sManaged target already running%s (PID %s)\n' \
            "$GOOD" "$RESET" "$existing_pid"
        return 0
    fi

    printf 'Starting persistent controlled target under %s...\n' "$CONTROL_ROOT"
    KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" \
        nohup "$BUILD_DIR/kirkware-load-target" >"$MANAGED_LOG_FILE" 2>&1 &
    local target_pid=$!
    printf '%s\n' "$target_pid" > "$MANAGED_PID_FILE"

    local control_dir
    control_dir="$(control_dir_for_pid "$target_pid")"
    for _ in {1..80}; do
        if ! kill -0 "$target_pid" 2>/dev/null; then
            printf 'Managed target exited during startup.\n' >&2
            [[ -f "$MANAGED_LOG_FILE" ]] && cat "$MANAGED_LOG_FILE" >&2
            rm -f -- "$MANAGED_PID_FILE"
            rm -rf -- "$control_dir"
            return 1
        fi
        if [[ -d "$control_dir" ]]; then
            printf '%sManaged target ready%s\n' "$GOOD" "$RESET"
            printf '  PID:     %s\n' "$target_pid"
            printf '  Root:    %s\n' "$CONTROL_ROOT"
            printf '  Control: %s\n' "$control_dir"
            printf '  Signal:  SIGUSR1\n'
            printf '  Log:     %s\n' "$MANAGED_LOG_FILE"
            return 0
        fi
        sleep 0.05
    done

    kill "$target_pid" 2>/dev/null || true
    rm -f -- "$MANAGED_PID_FILE"
    rm -rf -- "$control_dir"
    fail "managed target control directory did not appear"
}

run_target() {
    exec env KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" \
        "$BUILD_DIR/kirkware-load-target"
}

run_client() {
    start_managed_target

    local target_pid
    target_pid="$(managed_target_pid)" || fail "managed target disappeared before client startup"

    printf 'Opening loader directly for managed target PID %s\n' "$target_pid"
    env KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" \
        "$BUILD_DIR/kirkware-load-client" --target "$target_pid"
}

run_client_for_pid() {
    local pid="$1"

    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || fail "target PID must be a positive integer"
    kill -0 "$pid" 2>/dev/null || \
        fail "no process with PID $pid, or you lack permission to signal it"

    local control_dir
    control_dir="$(control_dir_for_pid "$pid")"
    if [[ ! -d "$control_dir" ]]; then
        printf '%sNote:%s no cooperative control directory at %s\n' \
            "$DIM" "$RESET" "$control_dir" >&2
        printf '%sThis PID is running, but it is not currently a cooperating load target using this control root.%s\n' \
            "$DIM" "$RESET" >&2
    fi

    printf 'Opening loader for PID %s using control root %s\n' "$pid" "$CONTROL_ROOT"
    env KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" \
        "$BUILD_DIR/kirkware-load-client" --target "$pid"
}

run_self_test() {
    "$BUILD_DIR/kirkware-load-target" --self-test "$BUILD_DIR/libkirkware_load_test.so"
}

run_demo() {
    banner
    printf 'Starting temporary controlled target under %s...\n' "$CONTROL_ROOT"
    KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" "$BUILD_DIR/kirkware-load-target" &
    local target_pid=$!
    local control_dir
    control_dir="$(control_dir_for_pid "$target_pid")"

    cleanup_demo() {
        if kill -0 "$target_pid" 2>/dev/null; then
            kill "$target_pid" 2>/dev/null || true
            wait "$target_pid" 2>/dev/null || true
        fi
        rm -rf -- "$control_dir"
    }
    trap cleanup_demo EXIT INT TERM

    for _ in {1..50}; do
        [[ -d "$control_dir" ]] && break
        sleep 0.05
    done
    [[ -d "$control_dir" ]] || fail "target control directory did not appear"

    printf '\nTemporary target PID %s is ready. Opening loader directly...\n' "$target_pid"
    env KIRKWARE_LOAD_CONTROL_ROOT="$CONTROL_ROOT" \
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
  Control root:   $CONTROL_ROOT

  1. Build + test
  2. Clean build + test
  3. Start foreground controlled target
  4. Open loader UI (persistent managed target)
  5. Full temporary demo session
  6. Sanitizer build + test
  7. Direct .so self-test
  8. Open loader UI for a chosen PID
  9. Change control-root folder
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
            8)
                printf 'Enter target PID: '
                if ! read -r chosen_pid; then
                    printf '\n'
                    continue
                fi
                printf 'Control-root folder [%s]: ' "$CONTROL_ROOT"
                if ! read -r chosen_root; then
                    printf '\n'
                    continue
                fi
                if [[ -n "$chosen_root" ]]; then
                    set_control_root "$chosen_root"
                fi
                RUN_TESTS=0
                build_harness
                run_client_for_pid "$chosen_pid"
                ;;
            9)
                printf 'New control-root folder [%s]: ' "$CONTROL_ROOT"
                if ! read -r chosen_root; then
                    printf '\n'
                    continue
                fi
                if [[ -n "$chosen_root" ]]; then
                    set_control_root "$chosen_root"
                    printf 'Control root set to %s\n' "$CONTROL_ROOT"
                fi
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
        --pid)
            shift
            (( $# > 0 )) || fail "--pid requires a PID"
            TARGET_PID="$1"
            ACTION="client"
            RUN_TESTS=0
            ;;
        --control-root)
            shift
            (( $# > 0 )) || fail "--control-root requires a path"
            set_control_root "$1"
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
        if [[ -n "$TARGET_PID" ]]; then
            run_client_for_pid "$TARGET_PID"
        else
            run_client
        fi
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
