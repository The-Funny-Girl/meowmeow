# Kirkware source tree

This repository contains the original Windows source/build assets plus a native Linux port.

For Linux, start with [BUILDING-LINUX.md](BUILDING-LINUX.md). The Linux targets use CMake/C++20 and do not require the Windows PowerShell/PE build pipeline. The build provides both the `kirkware` command-line runtime and the `kirkware-ui` SDL2/OpenGL graphical host.

The supported Garry's Mod client addon provides the Linux in-game menu and persistent module toggles. See [docs/GMOD_INGAME_MENU.md](docs/GMOD_INGAME_MENU.md); after installation, press **Insert** in-game to open or close it.

Current Linux migration status is tracked in [docs/LINUX_PORT_STATUS.md](docs/LINUX_PORT_STATUS.md). The original Windows runtime and UI remain preserved separately from the Linux implementation.
