# PS1 Games via POPSTARTER and Ember

RiptOPL lists **PlayStation 1** games beside the PS2 library on the same device. One PS1
view can contain two row types: `*.VCD` images launch through **POPSTARTER**, while game
folders under `EMBER/games/` launch through **Ember**. The row decides the core; there is
no per-page emulator switch.

> This document concentrates on POPSTARTER and its VCD/BDMA handoff. Ember uses the same PS1
> page but does not use POPSTARTER, BDMA, or the memory-card driver preparation described below.

## 1. PS2 and PS1 libraries on each device

Every game device has a PS2 library and a PS1 library:

- **PS2**: ISO / ZSO / UL / HDL rows, as supported by that device.
- **PS1**: POPSTARTER `*.VCD` and Ember game-folder rows, merged and sorted together on ordinary
  local devices, SMB, MMCE, and APA/PFS HDD.

How those libraries appear is controlled by **PS2/PS1 Game Display** (§2). They can be two
L3-switched views, one combined list with filters, or a single locked library. They remain views of
the same device page — with the same covers, favorites, and per-game settings — rather than extra
device tabs.

**UDPFS and UDPBD are the deliberate exception:** their PS1 pages list **Ember titles only**.
Ember keeps the already-mounted network transport alive. POPSTARTER resets the IOP and cannot
restore either `udpfs:` or the UDPBD block device, so RiptOPL does not list VCD rows that could not
launch.

## 2. Choose Both, Mixed, PS2, or PS1

The same **PS2/PS1 Game Display** picker appears on both **Interface** and
**PS Emulation Settings**. They edit one persisted setting:

| Value | Behavior |
| --- | --- |
| **Both (L3)** (default) | each device page has separate PS2 and PS1 views; **L3** switches between them and each page retains its own position |
| **Mixed** | each device page initially combines its PS2 and PS1 rows; **L3** cycles **Mixed → PS2 → PS1** on that device |
| **PS2** | each applicable device page shows PS2 rows only; L3 is completely inert, with no hint, sound, notification, or pause |
| **PS1** | each applicable device page shows PS1 rows only; L3 is completely inert, with no hint, sound, notification, or pause |

The setting applies to device game pages, including UDPFS and UDPBD. It does **not** control APPS or
Favorites; those pages have the independent behavior described below. Switching back to an
L3-enabled mode restores each device page's retained position. Returning from the Start/settings
menu also returns to the device page you paused on when that page is still visible; it no longer
arbitrarily selects USB.

### APPS and PS1 ELF titles

The independent **APPS Display** picker has two values:

- **Mixed** (default) shows every configured ELF in one list and leaves L3 completely inert.
- **Apps / PS1ELF (L3)** gives APPS two L3 views. Entries whose displayed title contains **`[PS1]`**
  (case-insensitive) appear on **PS1ELF**; all other entries appear on **Apps**. This is only a list
  accommodation for users who package PS1 launchers as ELFs. Either side still launches the entry
  as an ordinary configured ELF.

### Favorite your PS1 games

Press **R3** on a PS1 title to favorite it, exactly as you would a PS2 game. The **Favorites** page
always keeps its own independent four-stop L3 ring:

1. **All in One** — PS2, PS1, and ELF favorites together.
2. **PS2** — PS2 disc favorites only.
3. **PS1** — POPSTARTER and Ember favorites together.
4. **ELF** — homebrew/app favorites only.

A VCD favorite launches through POPSTARTER straight from the Favorites tab — even when the
source device page is currently showing its disc list — and carries its PS1 cover art and
disc badge with it.

The favorite handoff is wired on **every device with a VCD view** — USB, MX4SIO, iLink, the internal
exFAT HDD, SMB, MMCE, and the **APA-formatted internal HDD**. (On the APA HDD the PS1 games are spread across multiple
APA partitions, so opening one of its VCD favorites re-scans those partitions to find the game —
the first launch may take a moment.)

## 3. How PS1 games launch (POPSTARTER only)

VCD titles always boot through **`POPSTARTER.ELF`**. They never use OPL's built-in core
or Neutrino, so on a VCD game the per-game **Loader Core** selector is locked to an
inert label — choosing a core there has no effect on a PS1 game.

For ordinary VCD files, POPSTARTER needs the VCD's *name*; RiptOPL hands it the selected
title and POPSTARTER finds the matching `*.VCD`. An APA one-game install instead launches
with its literal, case-sensitive `PP.<name>` / `__.<name>` partition label so POPSTARTER can
mount that partition and boot its fixed `IMAGE0.VCD`.

Every local block-device VCD handoff, including iLink, uses POPSTARTER's normal local selector:
`mass:/POPS/XX.<name>.ELF`. OPL's live mount may be `massN:`, but POPSTARTER resets the IOP and
re-registers the selected device driver as bare `mass:` before it resolves that `XX.` argument.

> **Current iLink hardware status:** the selector and external equip are present, but revision 2692
> still returned POPSTARTER to wLaunchELF on an SCPH-39001 in all four SDK flavours. Ember passed on
> the same iLink disk. Treat iLink POPSTARTER as **wired but not hardware-confirmed** until a later
> build passes; the statement above documents the argument contract, not a claimed successful boot.

Where `POPSTARTER.ELF` is loaded from is set by **PS Emulation Settings →
POPSTARTER.ELF Device** — a driver-accurate picker (matching the Neutrino Device picker):

| Choice | Loads `POPS/POPSTARTER.ELF` from |
| --- | --- |
| **Default** | the boot device (where OPL launched, i.e. cwd), then the VCD's own device |
| Memory Card | `mc0:` / `mc1:` |
| USB | the mounted USB drive |
| MX4SIO | the mounted MX4SIO SD card |
| MMCE | `mmce0:` / `mmce1:` (SD2PSX / MemCard PRO2) |
| HDD (exFAT) | the mounted exFAT internal HDD |
| HDD (APA) | see the note below — APA POPSTARTER only applies to HDD-page launches |
| **Custom** | reveals a free-text path field — your own absolute `POPSTARTER.ELF` path |
| **Game's Device** | the VCD's own device only (`<device>:/POPS/POPSTARTER.ELF`) — no boot/cwd fallback and no Default fallthrough; a miss aborts with the usual *Missing POPSTARTER.ELF* warning |
| iLink | the mounted iLink / IEEE 1394 drive |

The picker covers USB / MMCE / MX4SIO / iLink / SMB VCD launches. PS1 VCDs **on the internal
APA HDD** always load `POPSTARTER.ELF` from the HDD (the `__common` then `+OPL` `POPS` folder, as
below) regardless of this setting — and that is also why **HDD (APA)** is inert for launches from
*other* device pages: those launches unmount `pfs0:` during their own teardown *before* the ELF is
read, so a `pfs0:` POPSTARTER can never survive them (OPL falls through to **Default** instead of
freezing on a dead path). Keep your APA copy for HDD-page launches; give the other pages a copy on
the boot device or the VCD's own device. For the **Custom** option the on-screen editor caps at 31
characters; for a longer path set `popstarter_path` in `settings_riptopl.cfg` directly.

### Which *build* of POPSTARTER

The picker above chooses which **copy** of `POPSTARTER.ELF` is loaded. Which **build** that copy is
comes down to the file itself, and the release package ships five of them in
`POPS/POPSTARTER VERSIONS/`:

| Build | Use it when |
| --- | --- |
| **MAIN** | POPSTARTER without the SMB support the shipped default carries. Only if you know you do not need SMB. |
| **DEBUG** | **The shipped default.** SMB-capable, and prints POPSTARTER's own diagnostics on screen. |
| **USBDELAY** | USB devices that need longer to settle before POPSTARTER reads them. |
| **USBDELAY_DEBUG** | USBDELAY plus the diagnostics. |
| **USBDELAY_LONGER_DEBUG** | A longer delay again, with diagnostics. |

Swapping is manual: copy the `POPSTARTER.ELF` you want over the `POPS/POPSTARTER.ELF` that the
picker resolves to. RiptOPL never chooses a build for you and never rewrites that file.

> **The shipped `POPS/POPSTARTER.ELF` is the DEBUG build, deliberately** — it is the build carrying
> SMB support, so it is the correct default for the package. POPSTARTER diagnostic text during an
> otherwise healthy VCD launch is therefore expected on every device, and is not evidence of a
> problem on its own. Do not replace it with **MAIN** unless you are certain you do not need SMB.

## 4. Where to put your VCD files

| Device | Location |
| --- | --- |
| USB / MMCE / MX4SIO / iLink / SMB | a **`POPS`** folder at the device root, holding `POPSTARTER.ELF` + your `*.VCD` files |
| Internal HDD (APA/PFS) | two layouts, both listed: an exact **`__.POPS`, `__.POPS0` … `__.POPS9`** store partition (many `*.VCD` on its root, named per file), and/or **`PP.<name>`** (visible) / **`__.<name>`** (hidden-label) one-game partitions. A one-game candidate is listed only when its root contains the exact file **`IMAGE0.VCD`**, which prevents similarly named HDD apps from appearing as games; it is shown as `<name>`. `POPSTARTER.ELF` is loaded from a **`POPS`** folder on the **`__common`** partition (then **`+OPL`** as a fallback). |

> The HDD's `XX.*` (BDMA/exFAT) and `SB.*` (SMBv1) launcher partitions point at VCDs that live on
> an exFAT device or an SMB share — those games appear under the **USB/MX4SIO/MMCE** or **SMB** VCD
> views, not the internal-HDD one.

VCD per-game config and art use the VCD **filename without `.VCD`** as their identity. They
otherwise follow the same layout and suffix rules as PS2 games on that device:

- `POPS/SCUS_123.45.Example.VCD`
- `CFG/SCUS_123.45.Example.cfg`
- `ART/SCUS_123.45.Example_COV.png`

When the filename begins with a valid PS1 disc ID, RiptOPL also accepts that ID as a fallback after
the filename lookup misses. For the example above, the fallback names are
`CFG/SCUS_123.45.cfg` and `ART/SCUS_123.45_COV.png`. This is a same-folder compatibility fallback;
`POPS/ART/` and a suffixless `POPS/<name>.png` remain POPSLoader-only layouts.

APA `PP.<name>` and `__.<name>` one-game installs are the naming exception: every partition contains
the same physical `IMAGE0.VCD` filename, so config and art use the displayed `<name>` from the label.

> **Internal HDD covers and config:** VCD entries use the same OPL data prefix as PS2 HDD
> games. On a `+OPL` data partition that means its root `ART/` and `CFG/` folders; when OPL
> data lives under the legacy `__common/OPL/` layout, use `OPL/ART/` and `OPL/CFG/`.

## 5. Block-device PS1 support — the BDMA equip

POPSTARTER reloads its mass-storage driver after resetting the IOP. USB FAT32 can use its built-in
driver; USB exFAT and the MX4SIO, MMCE, ATA and iLink transports need matching external
block-device modules (the BDMAssault / "BDMA" drivers). RiptOPL
*equips* them for you from **PS Emulation Settings → BDMA Settings** — RiptOPL copies the selected
loose pair from a device's `POPS/` folder onto your memory card:

RiptOPL prefers an existing `mc0:/POPSTARTER` or `mc1:/POPSTARTER` folder. On first setup it
creates the folder on the first present card (slot 1, then slot 2). Both replacement modules are
staged before the live pair is changed, so a failed copy leaves the previous pair available.

- **VCD BDMA Apply on Launch** *(default On)* — POPSTARTER does its own IOP reset and reloads
  its block-device driver from the memory card, so the right exFAT variant must already be on
  the card or the game drops to OSDSYS. When **On**, RiptOPL equips the variant matching the
  PS1 game you're launching (read from that game's own device) automatically, right before
  boot. Turn it **Off** to manage the driver yourself; that reveals the **BDMA Source** / **BDMA
  Mode** pickers below. Automatic equip proves only that the right files were staged; it does not
  turn the still-failing revision 2692 iLink POPSTARTER result into a pass.
- **USB launches always ask** — the PS2 cannot detect whether a USB stick is fat32 or exFAT
  formatted, so every USB .VCD launch first shows a **"fat32 or exFAT USB Mode?"** dialog
  (fat32 is recommended for non-exFAT USB users). Picking **fat32** de-equips to POPSTARTER's
  built-in USB stack (preserving any complete, unmarked manually-managed module pair); picking
  **exFAT** equips the BDMAssault `usbexfat` pair. The pick applies even when Apply-on-Launch is Off,
  and backing out of the dialog cancels the launch.
- **BDMA MODE** *(manual; shown when Apply-on-Launch is Off)* — which driver variant POPSTARTER should use: `USB (FAT32)` (none —
  removes the exFAT modules so POPSTARTER falls back to its built-in FAT32 driver),
  `USB (exFAT)`, `MX4SIO (exFAT)`, `MMCE (exFAT)`, `HDD (exFAT)` (the internal ATA
  HDD via BDMAssault), or `iLink`.
- **BDMA SOURCE** — which device holds the module files in its `POPS` folder: `USB`,
  `MX4SIO`, `MMCE`, `Internal HDD`, or `iLink`. OPL identifies each device by its block-device **driver**
  (`usb` / `mx4sio` / `ata` / `ilink` / `mmce`) and reads from that specific device, so pick the one your
  module files actually sit on. For the internal exFAT HDD choose **`Internal HDD`** — OPL reads the
  files from the same **`massN:/POPS/`** folder it lists that drive's PS1 games from (it never mounts
  an `ata0:` *filesystem*; `ata0:` is only an internal block-device identity, not a readable path).
  It is the same physical volume wLaunchELF shows, so place `POPS/usbd.irx.ata` +
  `POPS/usbhdfsd.irx.ata` there.

> **Module file names matter.** The two driver files in that `POPS/` folder must be named for the
> BDMA **MODE** you pick: **`usbd.irx.<mode>`** and **`usbhdfsd.irx.<mode>`**. For `HDD (exFAT)` that
> is **`usbd.irx.ata`** + **`usbhdfsd.irx.ata`**; iLink uses **`usbd.irx.ilink`** +
> **`usbhdfsd.irx.ilink`** (other modes use `.usbexfat`, `.mx4sio`, `.mmce`). Plain `usbd.irx` /
> `usbhdfsd.irx` with **no suffix** are ignored. If the selected pair is absent from every searched
> device `POPS/` folder, RiptOPL warns and leaves the current memory-card pair unchanged.

When you change either setting, RiptOPL copies the chosen variant's modules from the
SOURCE device's `POPS` folder onto `mc?:/POPSTARTER/` and records the equipped state in a
marker file there (compatible with POPSLoader). Every pair ships as separate files in the release
`POPS/` folder; **no BDMA module is embedded in `RIPTOPL.ELF`**. On apply, RiptOPL strips only the
mode suffix and writes `mc?:/POPSTARTER/usbd.irx` + `mc?:/POPSTARTER/usbhdfsd.irx`. Keep the whole
release `POPS/` folder on the relevant device so automatic launch preparation can find its pair.
SMB is network-only, so the BDMA equip does not apply to it.

The internal **exFAT HDD** (enable *BDM HDD* in **Game Sources**) mounts as a normal BDM
block device, so its PS1 games in `massN:/POPS/` list and launch through the same device page
according to **PS2/PS1 Game Display** — there's no separate page. Equip the `HDD (exFAT)` BDMA mode
so POPSTARTER itself can read them off the exFAT volume.

## 6. PS1 over SMB — network config mirror

PS1 games on an SMB share need POPSTARTER's own network config (`IPCONFIG.DAT` +
`SMBCONFIG.DAT`) on the memory card, plus its SMB modules. RiptOPL can write those config
files for you: enable **Settings → Network Settings → Write POPSTARTER Network Config**
(**on** by default). On save it mirrors the same IP / share values OPL already uses into
`mc?:/POPSTARTER/`. The SMB modules themselves ship in the release's `POPS/` folder
(copy them to `mc?:/POPSTARTER/`); if they're missing, an SMB VCD launch warns rather than
hanging.

## 7. Troubleshooting — "my VCDs don't show up"

Work down this ladder; each step isolates a different stage (from the #154 forensics):

1. **Does the device page appear at all?** If not, it's the device enables, not VCD: USB/MX4SIO/
   iLink need their toggles on; the **internal exFAT** page needs *BDM devices* + *BDM HDD* ON.
   APA/PFS and exFAT/BDM-ATA can both be enabled because they share the ATA stack; leaving the APA
   page enabled does not suppress the exFAT page.
2. **Do PS2 ISOs list from the device?** If yes, the filesystem/mount layer is proven working and
   the problem is downstream of listing — skip to the VCD-specific steps below.
3. **Check PS2/PS1 Game Display.** **Both (L3)** switches between separate libraries and **Mixed**
   cycles Mixed/PS2/PS1. L3 is intentionally absent and inert in the locked **PS2** and **PS1**
   modes; choose **PS1** when you want the PS1 library without an L3 action.
4. **VCDs are scanned from `<device-root>:/POPS/*.VCD`** — the game-folder prefix
   (`usb_prefix` etc.) is deliberately **not** applied, because POPSTARTER itself only reads
   `/POPS` at the root. A `POPS` folder inside your games subfolder will never be found.
5. **Name rules:** basenames longer than **160 characters** and the reserved name
   **`POPSTARTER.VCD`** are skipped at scan (the debug log says so) — previously they listed but
   could never launch (a dead ✕ button). Rename the file.
6. **Fails only after selecting a game?** Then listing/scan is fine and the handoff is the
   suspect: `POPSTARTER.ELF` present in `/POPS` (or `__common/POPS` on APA)? On a BDMA-backed device, the BDMA
   equip (§5) is best-effort — a failed equip toasts but the launch still proceeds and may land
   on OSDSYS.
7. **iLink returns to wLaunchELF?** Inspect the card after the attempt. A successful RiptOPL equip
   leaves `mc?:/POPSTARTER/bdma_config.txt` containing exactly `ilink`, plus `usbd.irx` (48,500 bytes;
   SHA-256 `5EA4818BA1CF5207F6D7CADB4C13B5AFA88C37C260750C21155B079A8C18F369`) and `usbhdfsd.irx`
   (23,452 bytes; SHA-256 `145CF1C0AF130EA7AC5CEC6696AA7E60B5C66A2AC46B8DEA94DB17BB3B911BCC`). Missing or different files
   put the defect on RiptOPL's external-equip side; exact files narrow it to POPSTARTER/BDMA runtime
   reinitialization after the handoff.
8. **UDPFS or UDPBD shows Ember but no VCDs?** That is expected. POPSTARTER cannot restore either
   network transport after its IOP reset, so those two PS1 pages intentionally publish Ember rows
   only.

## 8. Notes & limitations

- VCD support reuses the normal device pipeline, so covers, favorites and the theme all
  work exactly as they do for disc games.
- POPSTARTER, the patch file and every BDMA module variant are supplied as separate files in the
  release `POPS/` folder. RiptOPL embeds none of them.
- The Loader Core, GSM, Cheats, PADEMU and similar per-game options do not apply to PS1
  games (POPSTARTER ignores them).

See also **[NEUTRINO.md](NEUTRINO.md)** for the separate PS2 external-core loader, and
**[../ROLLING_RELEASE.md](../ROLLING_RELEASE.md)** for what the builds contain.
