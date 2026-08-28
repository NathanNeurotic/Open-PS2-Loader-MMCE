# PS1 Games via POPSTARTER (the "VCD" view)

RiptOPL can list and launch your **PlayStation 1** games (`*.VCD` images) right
alongside your PS2 library, on the same device. PS1 titles boot through
**POPSTARTER** — never OPL's built-in core and never Neutrino — so this is a
self-contained path with its own settings.

> The in-app UI deliberately calls this a **VCD** view rather than "PS1" or "POPS",
> so it reads cleanly next to your disc games.

## 1. The VCD view (press L3)

Every device that can hold games — USB, MMCE, MX4SIO, iLink, SMB, and the internal
HDD — has two lists:

- its **disc** games (PS2 ISO / ZSO / UL), and
- its **VCD** games (PS1 `*.VCD`).

Press **L3** on a device page to switch that page between the two. It is a **view**,
not a separate tab — the same page, the same covers, favourites and per-game settings,
just a different list. An on-screen hint shows whether you're looking at discs or VCDs.

## 2. Lock a page with "Default game view"

If you don't want to press L3 every time, open **Interface** and set
**Default game view**:

| Value | Behaviour |
| --- | --- |
| **Both** (default) | every page starts on its disc list; **L3** toggles to the VCD list |
| **ISO** | every page is locked to its disc list (L3 does nothing) |
| **VCD** | every page is locked to its VCD list (L3 does nothing) |

**Favourites follow the active view:** while a page shows VCDs, its favourited VCD
titles appear; while it shows discs, its favourited discs appear. Nothing is lost —
favourites that don't match the current view simply aren't shown until you switch back.

### Favourite your PS1 games

Press **R3** on a PS1 title (while a page is in its VCD view) to favourite it, exactly as
you would a disc game. The **Favourites tab has its own L3 toggle**, just like every device
page: press **L3** there to swap between your **disc favourites** and your **PS1 favourites**.
A VCD favourite launches through POPSTARTER straight from the Favourites tab — even when the
source device page is currently showing its disc list — and carries its PS1 cover art and
disc badge with it.

This works on **every device with a VCD view** — USB, MX4SIO, the internal exFAT HDD, SMB, MMCE,
and the **APA-formatted internal HDD**. (On the APA HDD the PS1 games are spread across multiple
APA partitions, so opening one of its VCD favourites re-scans those partitions to find the game —
the first launch may take a moment.)

## 3. How PS1 games launch (POPSTARTER only)

VCD titles always boot through **`POPSTARTER.ELF`**. They never use OPL's built-in core
or Neutrino, so on a VCD game the per-game **Loader Core** selector is locked to an
inert label — choosing a core there has no effect on a PS1 game.

For ordinary VCD files, POPSTARTER needs the VCD's *name*; RiptOPL hands it the selected
title and POPSTARTER finds the matching `*.VCD`. An APA one-game install instead launches
with its literal, case-sensitive `PP.<name>` / `__.<name>` partition label so POPSTARTER can
mount that partition and boot its fixed `IMAGE0.VCD`.

Where `POPSTARTER.ELF` is loaded from is set by **POPStarter →
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
| **MAIN** | The normal build. Start here if a VCD misbehaves. |
| **DEBUG** | Prints POPSTARTER's own diagnostics on screen — the one to use when reporting a VCD problem. |
| **USBDELAY** | USB devices that need longer to settle before POPSTARTER reads them. |
| **USBDELAY_DEBUG** | USBDELAY plus the diagnostics. |
| **USBDELAY_LONGER_DEBUG** | A longer delay again, with diagnostics. |

Swapping is manual: copy the `POPSTARTER.ELF` you want over the `POPS/POPSTARTER.ELF` that the
picker resolves to. RiptOPL never chooses a build for you and never rewrites that file.

> The `POPSTARTER.ELF` shipped at `POPS/POPSTARTER.ELF` is currently byte-identical to **DEBUG**.
> That is the explanation for POPSTARTER diagnostic text appearing during an otherwise healthy VCD
> launch — including the `Cannot mount __common to pfs1` line reported against MMCE launches. Copy
> **MAIN** over it to silence them.

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

## 5. exFAT PS1 support — the BDMA equip

POPSTARTER's stock driver reads FAT32. To boot PS1 games from an **exFAT** drive,
POPSTARTER needs extra block-device modules (the BDMAssault / "BDMA" drivers). RiptOPL
*equips* them for you from **POPStarter → BDMA Settings** — RiptOPL copies the right pair onto your memory
card; your own module files win when present, and a gzipped built-in pair fills the gap when
they aren't:

RiptOPL prefers an existing `mc0:/POPSTARTER` or `mc1:/POPSTARTER` folder. On first setup it
creates the folder on the first present card (slot 1, then slot 2). Both replacement modules are
staged before the live pair is changed, so a failed copy leaves the previous pair available.

- **VCD BDMA Apply on Launch** *(default On)* — POPSTARTER does its own IOP reset and reloads
  its block-device driver from the memory card, so the right exFAT variant must already be on
  the card or the game drops to OSDSYS. When **On**, RiptOPL equips the variant matching the
  PS1 game you're launching (read from that game's own device) automatically, right before
  boot — so an MX4SIO / HDD exFAT game just works with no manual step. Turn it **Off** to
  manage the driver yourself; that reveals the **BDMA Source** / **BDMA Mode** pickers below.
- **USB launches always ask** — the PS2 cannot detect whether a USB stick is fat32 or exFAT
  formatted, so every USB .VCD launch first shows a **"fat32 or exFAT USB Mode?"** dialog
  (fat32 is recommended for non-exFAT USB users). Picking **fat32** de-equips to POPSTARTER's
  built-in USB stack (preserving any complete, unmarked manually-managed module pair); picking
  **exFAT** equips the BDMAssault `usbexfat` pair. The pick applies even when Apply-on-Launch is Off,
  and backing out of the dialog cancels the launch.
- **BDMA MODE** *(manual; shown when Apply-on-Launch is Off)* — which driver variant POPSTARTER should use: `USB (FAT32)` (none —
  removes the exFAT modules so POPSTARTER falls back to its built-in FAT32 driver),
  `USB (exFAT)`, `MX4SIO (exFAT)`, `MMCE (exFAT)`, or `HDD (exFAT)` (the internal ATA
  HDD via BDMAssault).
- **BDMA SOURCE** — which device holds the module files in its `POPS` folder: `USB`,
  `MX4SIO`, `MMCE`, or `Internal HDD`. OPL identifies each device by its block-device **driver**
  (`usb` / `mx4sio` / `ata` / `mmce`) and reads from that specific device, so pick the one your
  module files actually sit on. For the internal exFAT HDD choose **`Internal HDD`** — OPL reads the
  files from the same **`massN:/POPS/`** folder it lists that drive's PS1 games from (it never mounts
  an `ata0:` *filesystem*; `ata0:` is only an internal block-device identity, not a readable path).
  It is the same physical volume wLaunchELF shows, so place `POPS/usbd.irx.ata` +
  `POPS/usbhdfsd.irx.ata` there.

> **Module file names matter.** The two driver files in that `POPS/` folder must be named for the
> BDMA **MODE** you pick: **`usbd.irx.<mode>`** and **`usbhdfsd.irx.<mode>`**. For `HDD (exFAT)` that
> is **`usbd.irx.ata`** + **`usbhdfsd.irx.ata`** (other modes use `.usbexfat`, `.mx4sio`, `.mmce`).
> Plain `usbd.irx` / `usbhdfsd.irx` with **no suffix** are ignored — OPL then falls back to its
> built-in embedded pair. The *"BDMA module files not found"* message now appears only when that
> embedded fallback also fails to install.

When you change either setting, RiptOPL copies the chosen variant's modules from the
SOURCE device's `POPS` folder onto `mc?:/POPSTARTER/` and records the equipped state in a
marker file there (compatible with POPSLoader). RiptOPL also ships a gzipped copy of each
BDMAssault variant pair inside the ELF as a final fallback, so an MC-only boot with no `POPS/`
folder anywhere still equips. Your own `usbd.irx.<mode>` / `usbhdfsd.irx.<mode>` files always win
when found — the embedded pair only fills the gap when no seek-path device carries them. SMB is
network-only, so the BDMA equip does not apply to it.

The internal **exFAT HDD** (enable *BDM HDD* in **Game Sources**) mounts as a normal BDM
block device, so its PS1 games in `massN:/POPS/` list and launch through the same VCD view
(press **L3**) as USB/MX4SIO — there's no separate page. Equip the `HDD (exFAT)` BDMA mode
so POPSTARTER itself can read them off the exFAT volume.

## 6. PS1 over SMB — network config mirror

PS1 games on an SMB share need POPSTARTER's own network config (`IPCONFIG.DAT` +
`SMBCONFIG.DAT`) on the memory card, plus its SMB modules. RiptOPL can write those config
files for you: enable **Settings → Network Settings → Write POPSTARTER Network Config**
(**on** by default). On save it mirrors the same IP / share values OPL already uses into
`mc?:/POPSTARTER/`. The SMB modules themselves ship in the release's `POPSTARTER/` folder
(copy them to `mc?:/POPSTARTER/`); if they're missing, an SMB VCD launch warns rather than
hanging.

## 7. Troubleshooting — "my VCDs don't show up"

Work down this ladder; each step isolates a different stage (from the #154 forensics):

1. **Does the device page appear at all?** If not, it's the device enables, not VCD: USB/MX4SIO/
   iLink need their toggles on; the **internal exFAT** page needs *BDM devices* + *BDM HDD* ON and
   the **APA HDD start mode OFF** — the two internal-HDD backends are mutually exclusive, and a
   hand-edited/cross-version config that enables both gets auto-reconciled at boot (you now get a
   toast when that happens; check **Game Sources**).
2. **Do PS2 ISOs list from the device?** If yes, the filesystem/mount layer is proven working and
   the problem is downstream of listing — skip to the VCD-specific steps below.
3. **Press L3.** The VCD view is per device and only reachable when *Default game view* is
   "Both" (or the page is locked to VCD). No L3 response = check that setting.
4. **VCDs are scanned from `<device-root>:/POPS/*.VCD`** — the game-folder prefix
   (`usb_prefix` etc.) is deliberately **not** applied, because POPSTARTER itself only reads
   `/POPS` at the root. A `POPS` folder inside your games subfolder will never be found.
5. **Name rules:** basenames longer than **160 characters** and the reserved name
   **`POPSTARTER.VCD`** are skipped at scan (the debug log says so) — previously they listed but
   could never launch (a dead ✕ button). Rename the file.
6. **Fails only after selecting a game?** Then listing/scan is fine and the handoff is the
   suspect: `POPSTARTER.ELF` present in `/POPS` (or `__common/POPS` on APA)? On exFAT, the BDMA
   equip (§5) is best-effort — a failed equip toasts but the launch still proceeds and may land
   on OSDSYS.

## 8. Notes & limitations

- VCD support reuses the normal device pipeline, so covers, favourites and the theme all
  work exactly as they do for disc games.
- POPSTARTER and the patch file are supplied by you (or bundled in the release `POPS/` folder)
  — RiptOPL embeds neither. The BDMA module variants **are** embedded (gzipped) as a last-resort
  fallback; your own copies still take priority.
- The Loader Core, GSM, Cheats, PADEMU and similar per-game options do not apply to PS1
  games (POPSTARTER ignores them).

See also **[NEUTRINO.md](NEUTRINO.md)** for the separate PS2 external-core loader, and
**[../ROLLING_RELEASE.md](../ROLLING_RELEASE.md)** for what the builds contain.
