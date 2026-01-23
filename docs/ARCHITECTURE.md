# Open PS2 Loader (OPL) Architecture Overview

This document provides a high-level map of the OPL codebase to assist with maintenance and refactoring.

## 1. System Architecture

OPL runs on the PlayStation 2 EE (Emotion Engine) and heavily utilizes the IOP (I/O Processor) for device drivers and protocols.

### Entry Point
- **EE Side:** `src/opl.c` -> `main()`
    - Initializes low-level systems (Reset, IOP, Logging).
    - Loads core modules.
    - Launches the GUI loop.

### Core Components

1.  **Main Loop (`src/opl.c`, `src/gui.c`)**
    - `main()` calls `init()`, then `guiIntroLoop()`, then `guiMainLoop()`.
    - `guiMainLoop()` (in `src/gui.c`) handles:
        - Input reading (`readPads`).
        - Rendering (`guiShow`).
        - Background tasks (`guiHandleDeferredOps`, `guiDrawOverlays`).

2.  **I/O Manager (`src/ioman.c`)**
    - Asynchronous I/O request handler.
    - Used for threaded operations to avoid blocking the UI (e.g., loading config, network requests).
    - Logging now flows through here (`LOG` -> `log_print` -> `ioPrintf`).

3.  **Device Support (`include/supportbase.h`)**
    - OPL supports multiple modes/devices, managed via `opl_io_module_t` and `item_list_t` interfaces.
    - **Modes:**
        - **BDM** (Block Device Manager): USB, MX4SIO, iLink. (`src/bdmsupport.c`)
        - **ETH** (SMB/Network): (`src/ethsupport.c`)
        - **HDD** (Internal HDD): (`src/hddsupport.c`)
        - **APP** (ELF Loader): (`src/appsupport.c`)

4.  **IOP Modules (`modules/`)**
    - OPL loads IRX modules to the IOP to handle hardware.
    - `cdvdman`: Emulates the CD/DVD drive for games.
    - `ingame_smstcpip`: Network stack.
    - `nbd`: Network Block Device server.

## 2. Key Subsystems & "Tangles"

### `src/opl.c` - The Monolith
Contains mixed responsibilities:
-   **Initialization:** Reset, variable defaults.
-   **Network Config:** Stores global IP/SMB config variables (`ps2_ip`, `gPCPassword`, etc.).
-   **Compatibility Update:** Logic to fetch game compatibility lists from the web (`compatUpdate`).
-   **NBD Server:** Logic to load/unload the NBD server (`handleLwnbdSrv`).
-   **Auto Launch:** Logic for "Mini" OPL builds to auto-boot games.

### `src/gui.c` - The UI Monolith
Contains:
-   **Rendering Loop:** Per-frame logic.
-   **Dialogs:** Configuration screens (Network, Video, Audio).
-   **Overlays:** FPS meter, Notifications.
-   **Input:** Pad handling integration.

### Cross-Connections
-   `src/opl.c` exposes many global variables (e.g., `gDefaultDevice`, `ps2_ip`) declared in `include/opl.h`. These are accessed directly by `src/gui.c` and device support modules.
-   **Refactoring Risk:** Moving these globals requires careful checking of all inclusions of `opl.h`.

## 3. Data Flow (Launch)

1.  **Selection:** User selects a game in GUI.
2.  **Config Load:** Game-specific config is loaded (`guiGameLoadConfig`).
3.  **Launch Request:** `itemLaunch` callback is triggered (e.g., `hddLaunchGame`).
4.  **Deinit:** `deinit()` is called to shut down GUI and unneeded drivers.
5.  **Core Load:** `ee_core` is loaded into memory.
6.  **Exec:** `LoadExecPS2` hands control to the game (via `ee_core`).

## 4. Observability (New)

-   **Logging:** `src/log.c` writes to `mass:/opl.log` and a memory buffer.
-   **Repro Dump:** `oplDumpRepro()` dumps state to `mass:/opl_repro.txt`.
