![logo](https://github.com/user-attachments/assets/bdf46f3c-a749-4128-a4c3-e0f968a31897)
# Open PS2 Loader
<br>
Copyright 2013, Ifcaro & jimmikaelkael<br>
Licensed under Academic Free License version 3.0<br>
Review the LICENSE file for further details.<br><br>

![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/ps2homebrew/Open-PS2-Loader/total?style=plastic&logo=github&logoSize=auto&label=Total%20Downloads&labelColor=navy&color=skyblue)
[![CI](https://github.com/ps2homebrew/Open-PS2-Loader/actions/workflows/compilation.yml/badge.svg?branch=master)](https://github.com/ps2homebrew/Open-PS2-Loader/actions/workflows/compilation.yml)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/581556b20d6b4dbf8e8af2232c89d60c)](https://app.codacy.com/gh/ps2homebrew/Open-PS2-Loader/dashboard?utm_source=gh&utm_medium=referral&utm_content=ps2homebrew/Open-PS2-Loader&utm_campaign=Badge_grade)
[![Discord](https://img.shields.io/discord/1275875800318476381?style=flat&logo=Discord)](https://tinyurl.com/PS2SPACE)
## Contents

- [Introduction](#introduction) · [Quick Start](#quick-start) · [Major Features Overview](#major-features-overview) · [Release Types](#release-types) · [How to Use](#how-to-use) · [USB/MMCE/MX4SIO/iLink](#usbmmcemx4sioilink) · [SMB](#smb) · [HDD](#hdd) · [APPS](#apps) · [Cheats](#cheats) · [NBD Server](#nbd-server) · [ZSO Format](#zso-format) · [PS3 BC](#ps3-bc) · [Frequent Issues](#frequent-issues)

## Introduction

Open PS2 Loader (OPL) is a 100% Open source game and application loader for
the PS2 and PS3 units.
Major capabilities include GSM video mode fixes, Virtual Memory Cards (VMC), PS2RD cheats, DS3/DS4 pad emulation, themes, and homebrew app launching.

It supports six categories of devices:

1. USB mass storage devices;
2. MMCE (Memory Card Mass Storage protocol devices);
3. MX4SIO (SD card connected to memory card port via adapter);
4. iLink (SBP2 compliant storage devices via IEEE 1394);
5. SMBv1 shares;
6. ATA/IDE HDDs, including internal exFAT configurations (MBR/GPT).

All of the devices mentioned above support multiple file formats, including:

- ISO;
- ZSO (Compressed ISO);
- USB Extreme (ul);
- Homebrews (Apps) in ELF format;
- HDDs support the HDLoader format.

>[!NOTE]
OPL is developed continuously - anyone can contribute improvements to the project due to its open-source nature.

You can visit the Open PS2 Loader forum at:\
<https://www.psx-place.com/forums/open-ps2-loader-opl.77/>

You can report compatibility game problems at:\
<https://www.psx-place.com/threads/open-ps2-loader-game-bug-reports.19401/>

For an updated compatibility list, you can visit the OPL-CL site at:\
<http://sx.sytes.net/oplcl/games.aspx>

## Quick Start

### What you need

- [ ] A PlayStation 2 or backward-compatible PlayStation 3.
- [ ] One storage option: USB drive, MMCE or MX4SIO SD setup, iLink storage, SMB network share, or internal HDD (APA/PFS or exFAT).
- [ ] The latest Open PS2 Loader (OPL) ELF binary.
- [ ] Optional: network access (recommended for SMB and remote file management).

### Minimal startup path

1. Download the latest OPL release build.
2. Copy the `OPNPS2LD.ELF` file to your launch method (FMCB, FHDB, or equivalent).
3. Prepare your storage with the expected OPL folders: `DVD`, `CD`, `CFG`, `ART`, `VMC`, and other mode-specific directories as needed.
4. Open OPL settings and enable the device mode you plan to use.
5. Launch one test game, then save settings so OPL reuses your configuration.

For detailed setup steps, jump to the README sections for **USB/MMCE/MX4SIO/iLink**, **SMB**, **HDD**, **APPS**, and **Frequent Issues**.


### Major Features Overview

This section is a fast feature map to improve discoverability of core OPL capabilities and reduce setup friction for first-time and returning users.

- **MMCE support:** OPL supports MMCE devices using the Memory Card Mass Storage protocol for SD-based loading through the Memory Card slot.
- **MX4SIO support:** OPL supports MX4SIO adapters for SD-based loading through the Memory Card slot. See the **USB/MMCE/MX4SIO/iLink** section for filesystem and layout guidance.
- **Internal HDD exFAT support:** Internal HDD loading supports exFAT in addition to APA/PFS, including GPT partitioning for large disks. See the **HDD** section for formatting and fragmentation guidance.
- **Themes:** Place theme assets in the `THM` folder, then select and apply themes from OPL settings.
- **Cheats / PS2RD:** OPL supports PS2RD `.cht` cheat files from the `CHT` folder, with both auto-apply and launch-time selection modes.
- **Pad emulation (DS3/DS4):** Builds that include PADEMU allow DualShock 3 and DualShock 4 pad emulation support.
- **GSM (video mode handling):** Builds that include GSM allow game video mode handling/overrides for display compatibility.
- **VMC (Virtual Memory Cards):** Create and use VMC images (8MB to 64MB) via the `VMC` folder and per-game options.
- **Per-game settings workflow:** Highlight a game, open **Game Settings**, adjust options (such as compatibility modes, cheats, GSM, PADEMU, and VMC), then save so settings persist per title.
- **App launching (APPS + config methods):** OPL can launch homebrew ELFs using either `conf_apps.cfg` entries or per-app `title.cfg` metadata in `APPS` subfolders.


<details>
  <summary> <b> Release types </b> </summary>
<p>

Open PS2 Loader bundle included several types of the same OPL version. These
types come with more or fewer features included.

| Type (can be a combination) | Description                                                                             |
| --------------------------- | --------------------------------------------------------------------------------------- |
| `Release`                   | Full-feature build for most users: includes GSM video mode/scaling compatibility tooling (video-mode fixes/overrides), in-game screenshots (IGS), DS3/DS4 pad emulation, VMC support, PS2RD cheats, and parental controls. |
| `DTL_T10000`                | OPL for TOOLs (DevKit PS2)                                                              |
| `IGS`                       | Adds in-game screenshot capture so you can save screenshots while playing.              |
| `PADEMU`                    | Adds DualShock 3/DualShock 4 controller emulation support on compatible setups.         |
| `RTL`                       | OPL with the right to left language support.                                            |

</p>
</details>

<details>
  <summary> <b> How to use </b> </summary>
<p>

OPL uses the following directory tree structure across HDD, SMB, and
USB modes:

| Folder | Description                                          | Modes       |
| ------ | ---------------------------------------------------- | ----------- |
| `CD`   | for games on CD media - i.e. blue-bottom discs       | USB and SMB |
| `DVD`  | for DVD5 and DVD9 images (if filesystem supports +4gb files) | USB and SMB |
| `VMC`  | Virtual Memory Card images (headline save feature): stored in `VMC/`, typically 8MB to 64MB, then assigned per game via **Game Settings** | all         |
| `CFG`  | for saving per-game configuration files              | all         |
| `ART`  | for game art images                                  | all         |
| `THM`  | for themes support                                   | all         |
| `LNG`  | for translation support                              | all         |
| `CHT`  | for cheats files                                     | all         |
| `APPS`  | for ELF files                                       | all         |

Per-game settings are stored per title in the `CFG` context. Typical use cases include compatibility toggles, video options (GSM), cheat toggles, and assigning a VMC file from the `VMC` folder to that game.

OPL will automatically create the above directory structure the first time you launch it and enable your favorite device.

For HDDs formatted with the APA partition scheme, OPL will read `hdd0:__common/OPL/conf_hdd.cfg` for the config entry `hdd_partition` to use as your OPL partition.
If not found a config file, a 128Mb `+OPL` partition will be created. You can edit the config if you wish to use/create a different partition.
All partitions created by OPL will be 128Mb (it is not recommended to enlarge partitions as it will break LBAs, instead remove and recreate manually with uLaunchELF at a larger size if needed).
	
HDDs are also able to be formatted as exFAT to avoid the 2TB limitation.  Please see below in the `HDD` section for more details on this configuration.

</p>
</details>

<details>
  <summary> <b> USB/MMCE/MX4SIO/iLink </b> </summary>
<p>

Supported file systems:
exFAT (since OPL v1.2.0 beta - rev1880) and FAT32, both use the MBR partition table. This section applies to MMCE and MX4SIO SD setups, USB storage, and iLink SBP2 storage.

Game files should be *ideally* defragmented either file by file or by whole drive.

> NOTE: Partial file fragmentation is supported (up to 64 fragments!) since OPL v1.2.0 beta - rev1893

If you choose to use the FAT32 file system, games larger than 4gb must use USBExtreme format (see OPLUtil or USBUtil programs).

We do **not** recommend using any defrag programs. The best way for defragmenting - copy all files to pc, format USB, copy all files back.
Repeat it once you faced defragmenting problem again.

</p>
</details>

<details>
  <summary> <b> SMB </b> </summary>
<p>

For loading games by SMB protocol, you need to share a folder (ex: PS2SMB)
on the host machine or NAS device and make sure that it has full read and
write permissions. USB Advance/Extreme format is optional - \*.ISO images
are supported using the folder structure above.

</p>
</details>

<details>
  <summary> <b> HDD </b> </summary>
<p>
	
For PS2, 48-bit LBA internal HDDs are supported. The HDD can be formatted as:

- APA partitioning with PFS filesystem (up to 2TB)
	- OPL will create the `+OPL` partition on the HDD.  To avoid this, you can create a text file at the location `hdd0:__common:pfs:OPL/conf_hdd.txt` that contains the preferred partition name (for example `__common`).
- MBR partitioning (up to 2TB) or GPT partitioning (unlimited) with the exFAT filesystem
	- Files should be added contiguously or synchronously to avoid fragmentation. For example, drag and drop files one at a time, or ensure that files are added sequentially.
	- When formatting drives for the exFAT filesystem, please make sure the `Allocation unit size` is set to `Default`.

</p>
</details>

<details>
  <summary> <b> APPS </b> </summary>
<p>

There are two supported methods for adding apps to OPL. Keep both available and choose the one that fits your setup:

- Use legacy `conf_apps.cfg` when you want one central list and/or apps stored anywhere on supported devices.
- Prefer folder-based `title.cfg` when you want each app self-contained inside `APPS/<APP_FOLDER>/`.

### conf_apps.cfg method (Legacy)

Each entry uses `Display Name=DevicePathToELF`:
- Left side: the name shown in the OPL app list.
- Right side: full device/path to the ELF.

To begin:

1. Create a text file called `conf_apps.cfg`.
2. In this file, put the name you want to appear in the list of apps, followed by the "=" sign.
3. Add the device prefix and ELF path (for example `mass:` for USB, `mc:` for Memory Card), then the file path to the ELF.

> NOTE: Enter the exact path and exact letter case. OPL is case-sensitive.

The structure should look like this:

```
My App Name=mass:APPS/MYAPP.ELF
```

let's use OPL itself as an example:

```
OPL=mass:APPS/OPNPS2LD.ELF
```

With this method, ELFs do not need to be in `APPS`, but keeping them there can make your setup easier to manage.

The `conf_apps.cfg` file must be in the OPL folder on your Memory Card,\
or at the root of the storage device.


### title.cfg method

This method uses one `title.cfg` per app folder, with two required lines:
- `title=` for the app name shown in OPL.
- `boot=` for the ELF filename to launch.

To begin:

1. In `APPS`, create a folder for the app.
2. Put the ELF in that folder, and create a text file named `title.cfg` in the same folder.
3. In that file, add the following instructions:

```
title=My App Name
boot=MYAPP.ELF
```

Using OPL again as an example:

```
title=Open PS2 Loader
boot=OPNPS2LD.ELF
```

In this method, both the ELF and `title.cfg` must be in the same folder under `APPS`.

> NOTE: In both methods, pay close attention to file names because, as already mentioned, OPL is case-sensitive.

</p>
</details>

<details>
  <summary> <b> Cheats </b> </summary>
<p>

OPL accepts `.cht` files in PS2RD format. Each cheat file corresponds to a specific game and must be stored in the `CHT` directory on your device.
Cheats are structured as hexadecimal codes, with proper headers as descriptions to identify their function.
You can activate cheats via OPL's graphical interface. Navigate to a games settings, enable cheats and select the desired mode.

### Cheat Modes

  * Auto Select Cheats:  
This mode will enable and apply all cheat codes in your `.cht` file to your game automatically.

  * Select Game Cheats:  
When enabled a cheat selection menu will appear when you launch a game. You can navigate the menu and disable undesired cheats for this launch session. Master Codes cannot be disabled as they are required for any other cheats to be applied.

</p>
</details>

<details>
  <summary> <b> NBD Server </b> </summary>
<p>

OPL now uses an [NBD](https://en.wikipedia.org/wiki/Network_block_device) server to share the internal hard drive, instead of HDL server.
NBD is [formally documented](https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md) and developed as a collaborative open standard.

The current implementation of the server is based on [lwNBD](https://github.com/bignaux/lwNBD), go there to contribute on the NBD code itself.

The main advantage of using NBD is that the client will expose the drive to your operating system in a similar way as a directly attached drive.
This means that any utility that worked with the drive when it was directly attached should work the same way with NBD.

OPL currently only supports exporting (sharing out) the PS2's drive.

Version note: feature availability and behavior may differ by build date/tag.

You can use `hdl-dump`, `pfs-shell`, or even directly edit the disk in a hex editor.

For example, to use `hdl_dump` to install a game to the HDD:

  * Connect with your chosen client (OS specific)
  * Run `hdl_dump inject_dvd ps2/nbd "Test Game" ./TEST.ISO`
  * Disconnect the client.

To use the NBD server in OPL:

  * Use the latest release or pre-release from the [Releases](https://github.com/ps2homebrew/Open-PS2-Loader/releases) page if you need newer NBD fixes.
  * Ensure OPL is configured with an IP address (either static or DHCP).
  * Open the menu and select "Start NBD server". Once it's ready, it should update the screen to say "NBD Server running..."
  * Now you can connect with any of the following NBD clients.

### nbd-client

Supported: Linux, [Windows with WSL and custom kernel](https://github.com/microsoft/WSL/issues/5968)

nbd-client requires nbd kernel support. If it isn't loaded,
`sudo modprobe nbd` will do.

list available export:

```sh
nbd-client -l 192.168.1.45
```

connect:

```sh
nbd-client 192.168.1.45 /dev/nbd1
```

disconnect:

```sh
nbd-client -d /dev/nbd1
```

You'll generally need sudo to run these commands in root or
add your user to the right group usually "disk".

### nbdfuse

Supported: Linux, Windows with WSL2

list available export:

```sh
nbdinfo --list nbd://192.168.1.45
```

connect:

```sh
mkdir ps2
nbdfuse ps2/ nbd://192.168.1.45 &
```

disconnect:

```sh
umount ps2
```

### wnbd

Supported: Windows

[WNBD client](https://cloudbase.it/ceph-for-windows/).
Install, reboot, open elevated (with Administrator rights) [PowerShell](https://docs.microsoft.com/en-us/powershell/scripting/windows-powershell/starting-windows-powershell?view=powershell-7.1#how-to-start-windows-powershell-on-earlier-versions-of-windows)

connect:

```sh
wnbd-client.exe map hdd0 192.168.1.22
```

disconnect:

```sh
wnbd-client.exe unmap hdd0
```

### Mac OS

Not supported.

</p>
</details>

<details>
  <summary> <b> ZSO Format </b> </summary>
<p>

As of version 1.2.0, compressed ISO files in ZSO format is supported by OPL.

To handle ZSO files, a python script (ziso.py) is included in the pc folder of this repository.
It requires Python 3 and the LZ4 library:

  ```sh
pip install lz4
```

To compress an ISO file to ZSO:

  ```sh
python ziso.py -c 2 "input.iso" "output.zso"
```

To decompress a ZSO back to the original ISO:

```sh
python ziso.py -c 0 "input.zso" "output.iso"
```

You can copy ZSO files to the same folder as your ISOs and they will be detected by OPL.
To install onto internal HDD, you can use the latest version of HDL-Dump.

</p>
</details>

<details>
  <summary> <b> PS3 BC </b> </summary>
<p>

Currently, supported only [PS3 Backward Compatible](https://www.psdevwiki.com/ps3/PS2_Compatibility#PS2-Compatibility) (BC) versions. So only [COK-001](https://www.psdevwiki.com/ps3/COK-00x#COK-001) and [COK-002/COK-002W](https://www.psdevwiki.com/ps3/COK-00x#COK-002) boards are supported. USB, SMB, HDD modes are supported.

To run OPL, you need an entry point for running PS2 titles. You can use everything (Swapmagic PS2, for example), but custom firmware with the latest Cobra is preferred. Note: only CFW supports HDD mode.

</p>
</details>

<details>
  <summary> <b> Some notes for DEVS </b> </summary>
<p>

Open PS2 Loader needs the [**latest PS2SDK**](https://github.com/ps2dev/ps2sdk)

</p>
</details>

<details>
  <summary> <b> OPL Archive </b> </summary>
<p>

Since 05/07/2021 every OPL build dispatched to the release section of this repository will be uploaded to a [mega account](https://mega.nz/folder/Ndwi1bAK#oLWNhH_g-h0p4BoT4c556A). You can access the archive by clicking the mega badge on top of this readme

</p>
</details>

<details>
  <summary> <b> Frequent Issues </b> </summary>
<p>

### OPL Freezes on logo or grey screen

1. **Symptom:** OPL hangs on the logo or a grey screen during startup.
2. **Likely cause:** OPL is trying to load an incompatible or corrupted config file from an older build.
3. **Recovery steps:** Hold __`START`__ while OPL initializes to skip config loading, open settings, then save a fresh configuration.
4. **Verification:** Reboot OPL normally (without holding buttons) and confirm it reaches the game list/settings screen without freezing.

### Game freezes on white screen

1. **Symptom:** Game boot stops on a white screen or fails to continue loading.
2. **Likely cause:** The game image is fragmented so OPL cannot read it reliably, or the ISO/ZSO/UL image is corrupted/incomplete.
3. **Recovery steps:** Check the game file integrity (size/hash against known-good dump if available), recopy the game image, and ensure files are contiguous (copy all files off the device, reformat, then copy files back in order).
4. **Verification:** Relaunch the same title and confirm it passes the white screen and reaches the game's intro/menu.

### OPL does not display anything on boot

1. **Symptom:** No image is shown after launching OPL (black/blank screen on TV).
2. **Likely cause:** A forced video mode was saved that your display does not support (commonly from GSM video mode/scaling compatibility settings).
3. **Recovery steps:** Hold __`Triangle + Cross`__ while OPL initializes to reset video mode to __`Auto`__.
4. **Verification:** Start OPL again normally and confirm the interface appears and remains visible.

For GSM/video-mode mistakes, use the same recovery combo above: hold __`Triangle + Cross`__ at boot to restore __`Auto`__ video mode.

If your issue is still unresolved, report it here: <https://www.psx-place.com/threads/open-ps2-loader-game-bug-reports.19401/>.

</p>
</details>
