# Linux shared-object load harness

This repository includes a **cooperative** Linux `.so` load harness for testing code-loading and anti-cheat telemetry in a process we control.

It is deliberately different from remote process injection: the loader client never writes into another process, uses `ptrace`, performs remote `dlopen`, or manual-maps a library. Instead, the controlled target exposes a private Unix-domain socket and performs `dlopen()` on itself after receiving a same-user request.

## Components

- `build-load-harness.sh` — build/test helper with an interactive terminal menu
- `kirkware-load-target` — controlled process that accepts same-user load/status/unload requests
- `kirkware-load-client` — terminal UI and CLI for selecting a running harness target
- `libkirkware_load_test.so` — small test module that prints when it is loaded and unloaded

The target socket is created as:

```text
/tmp/kirkware-load-target-<uid>-<pid>.sock
```

The socket is mode `0600`, the target verifies the peer UID with `SO_PEERCRED`, and requested modules must:

- use an absolute path
- resolve to a regular file
- be owned by the current user
- not be group/world writable

## Build

From the repository root:

```sh
./build-load-harness.sh
```

When run interactively with no arguments, the helper presents:

```text
KIRKWARE LINUX LOAD HARNESS
Cooperative .so loading for a process we control
-----------------------------------------------
  1. Build + test
  2. Clean build + test
  3. Start controlled target
  4. Open loader UI
  5. Full demo session
  6. Sanitizer build + test
  7. Direct .so self-test
  0. Exit
```

For a non-interactive build:

```sh
./build-load-harness.sh --no-tests
```

The normal build output is under `build-load-harness/`:

```text
build-load-harness/kirkware-load-target
build-load-harness/kirkware-load-client
build-load-harness/libkirkware_load_test.so
```

## Easiest test

Run:

```sh
./build-load-harness.sh --demo
```

This builds the harness, starts a controlled target, waits for its private socket, and opens the loader terminal UI.

Select the target, then choose **Load test .so**. Press Enter at the module-path prompt to use the test module built beside the client.

The target terminal will print a message similar to:

```text
[kirkware load test] module loaded in pid 12345
```

Choosing **Unload module** produces the corresponding unload message.

## Two-terminal test

Terminal 1:

```sh
./build-load-harness.sh --target
```

Terminal 2:

```sh
./build-load-harness.sh --client
```

The client lists only same-user harness sockets. Pick the target PID and use the interactive actions.

## CLI operation

List controlled targets:

```sh
./build-load-harness/kirkware-load-client --list
```

Query a target:

```sh
./build-load-harness/kirkware-load-client --target 12345 --status
```

Ask that target to load the test module into itself:

```sh
./build-load-harness/kirkware-load-client \
  --target 12345 \
  --load "$PWD/build-load-harness/libkirkware_load_test.so"
```

Unload it:

```sh
./build-load-harness/kirkware-load-client --target 12345 --unload
```

Stop the target cleanly:

```sh
./build-load-harness/kirkware-load-client --target 12345 --quit
```

## Direct smoke test

The target also has a self-test mode which loads and unloads the module without starting the socket service:

```sh
./build-load-harness.sh --self-test
```

CTest runs the same load/unload check during a normal harness build.

## Sanitizers

```sh
./build-load-harness.sh --sanitize
```

This creates a Debug ASan/UBSan build and runs the load/unload smoke test.

## Scope

This harness is intended for controlled process-loading experiments, module lifecycle testing, `/proc/self/maps` observation, defensive anti-cheat instrumentation, and other tests where both sides are ours.

It does not attach to or force code into Garry's Mod or another unrelated running process.
