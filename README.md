# Open PS2 Loader (OPL)

Copyright 2013, Ifcaro & jimmikaelkael
Licensed under Academic Free License version 3.0
Review the LICENSE file for further details.

[![CI](https://github.com/ps2homebrew/Open-PS2-Loader/actions/workflows/build.yml/badge.svg)](https://github.com/ps2homebrew/Open-PS2-Loader/actions/workflows/build.yml)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/99032a6a180243bfa0d0e23efeb0608d)](https://www.codacy.com/gh/ps2homebrew/Open-PS2-Loader/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=ps2homebrew/Open-PS2-Loader&amp;utm_campaign=Badge_Grade)
[![Discord](https://img.shields.io/discord/652861436992946216?style=flat&logo=Discord)](https://discord.gg/CVFUa9xh6B)

## Introduction

Open PS2 Loader (OPL) is a 100% Open source game and application loader for the PS2 and PS3 units. It supports USB mass storage, MX4SIO, iLink, SMBv1 shares, and the PlayStation 2 HDD unit.

## Development

We have modernized the OPL codebase to be more modular and contributor-friendly.

### Architecture
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for a high-level overview of the system, including the Main Loop, I/O Manager, and Device Drivers.

### Building
**Prerequisites:** [PS2SDK](https://github.com/ps2dev/ps2sdk) must be installed and the environment variable `PS2SDK` set.

Use the provided build wrapper for consistent builds:

```bash
# Build Release (outputs artifacts/OPL.elf)
./tools/build.sh

# Build Debug (outputs artifacts/OPL-dbg.elf)
# Note: Debug builds enable internal logging and may be slower.
```

### Testing
We use a lightweight unit test framework for configuration and logic that does not depend on PS2 hardware.

```bash
# Run unit tests
make test
```

### Observability
OPL includes a centralized logging system.
- **Runtime:** Logs are buffered in RAM to avoid I/O stutter.
- **Crash/Debug:** A "Repro Bundle" (Config, State, Logs) can be dumped to `mass:/opl_repro.txt` via the new `oplDumpRepro` function (integration pending UI trigger).

### Code Style
We use `clang-format` to maintain code consistency.

```bash
# Check formatting
./tools/check.sh

# Apply formatting
make format
```

## Release types

| Type | Description |
| --- | --- |
| `Release` | Regular OPL release with GSM, IGS, PADEMU, VMC, PS2RD Cheat Engine & Parental Controls. |
| `DTL_T10000` | OPL for TOOLs (DevKit PS2) |
| `IGS` | OPL with InGame Screenshot feature. |
| `PADEMU` | OPL with Pad Emulation for DS3 & DS4. |
| `RTL` | OPL with the right to left language support. |

## Usage & Features

OPL uses the following directory tree structure across HDD, SMB, and USB modes:

| Folder | Description |
| --- | --- |
| `CD` | for games on CD media |
| `DVD` | for DVD5 and DVD9 images |
| `VMC` | for Virtual Memory Card images |
| `CFG` | for saving per-game configuration files |
| `ART` | for game art images |
| `THM` | for themes support |
| `LNG` | for translation support |
| `CHT` | for cheats files |

For detailed usage instructions, please visit the [Open PS2 Loader forum](https://www.psx-place.com/forums/open-ps2-loader-opl.77/).
