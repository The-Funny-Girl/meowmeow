# Linux in-process component runtime

The Linux port provides a real shared-library component with a stable C ABI so application code can use an in-process protection component without depending on the Windows bootstrap implementation.

## Lifecycle

The Linux component follows the same high-level lifecycle expected by the surrounding runtime:

1. locate the component shared library
2. load it into a cooperating process through that process's supported module/plugin path
3. resolve the Kirkware component ABI
4. initialize and validate the ABI version
5. poll health/integrity state and heartbeat information
6. shut down cleanly before the host unloads the module

The standalone `kirkware` executable can exercise this lifecycle against itself with:

```sh
./build-linux/kirkware --component-self-test ./build-linux/libkirkware_component.so
```

This is also run automatically by CTest.

## ABI

`source/platform/component_api.h` defines ABI version 1. The exported entry points are:

```text
kirkware_component_initialize
kirkware_component_poll
kirkware_component_shutdown
```

The status structure currently reports:

- ABI and structure versions
- heartbeat counter
- Linux `TracerPid` for the current process
- count of file-backed mappings in `/proc/self/maps`
- a short human-readable status string

The ABI is deliberately C-compatible so a supported host integration can call it without depending on C++ name mangling or internal application classes.

## Loader/session layer

`ComponentSession` owns the host-side lifecycle. It uses `dlopen(..., RTLD_LOCAL)` in the current process, resolves the exported ABI, validates the handshake, polls health state, calls shutdown, and releases the shared object.

It does not attach to another process, write another process's memory, manually map an ELF image, or install remote hooks. A game/application integration should arrange for `libkirkware_component.so` to be loaded through an approved module/plugin/startup mechanism and then reuse this ABI and lifecycle.

## Packaging

The component is installed as:

```text
${libdir}/kirkware/libkirkware_component.so
```

The command-line executable remains `${bindir}/kirkware`.

This separation keeps the Linux in-process component testable and versioned independently while leaving the preserved Windows runtime unchanged.
