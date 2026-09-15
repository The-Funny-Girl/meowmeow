# Kirkware source tree

This repository contains the original Windows source/build assets plus a native Linux port.

For Linux, start with [BUILDING-LINUX.md](BUILDING-LINUX.md). The Linux targets use CMake/C++20 and do not require the Windows PowerShell/PE build pipeline. The build provides both the `kirkware` command-line runtime and the `kirkware-ui` SDL2/OpenGL graphical host.

Current Linux migration status is tracked in [docs/LINUX_PORT_STATUS.md](docs/LINUX_PORT_STATUS.md). The original Windows runtime and UI remain preserved separately from the Linux implementation.
