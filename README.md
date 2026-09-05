
<p align="center"><img alt="RiptOPL" src="https://raw.githubusercontent.com/NathanNeurotic/Open-PS2-Loader/rebuild/main/docs/assets/riptopl.png" /></p>



<p align="center">
  <img width="400" height="92" alt="AI-Assisted-Software-Lovers-Only" src="https://github.com/user-attachments/assets/71335775-9fe3-4507-ac2c-caa851abb24c" />
</p>



```HAS THIS EVER HAPPENED TO YOU?```
```You download another OPL fork. It has a new theme. A new menu. Maybe even a file browser, because apparently launching games was too direct and somebody needed a side quest.```
```Then the game still does not work.```
```Introducing RiptOPL, for people who wanted OPL to get better instead of just getting redecorated.```
```Thanks to serious testing from @PixeliGer, @zackcage6, and others, RiptOPL has reached a stable, reliable state overall. Bug reports, suggestions, feature requests, and questions are still welcome, because unlike a paintjob, actual progress requires feedback.```
```RiptOPL is not just a frontend. It is not just a theme pack with confidence. It is not OPL wearing a fake mustache and introducing itself as innovation.```
```RiptOPL keeps compatibility work moving in both places that matter: the OPL core and Neutrino. That means two real launch systems in one setup. If a game does not like one mode, change the game setting and try the other. No app swapping. No fork roulette. No pretending five copies of the same idea equals five solutions.```
```You get the options that can actually change results. You get fewer pointless blockers. You get less feature clutter pretending to be engineering.```
```Fast. Simple. Compatible.```
```Stop chasing the same thing with a new name.```
```Stop mistaking decoration for development.```
# RiptOPL
**An opinionated [Open PS2 Loader](https://github.com/ps2homebrew/Open-PS2-Loader) fork — aiming to be the "definitive build."**
<br>
Based on Open PS2 Loader · Copyright 2013, Ifcaro & jimmikaelkael<br>
Licensed under Academic Free License version 3.0<br>
Review the LICENSE file for further details.<br><br>

[![CI](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/flavours.yml/badge.svg?branch=rebuild/main)](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/flavours.yml)
![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/NathanNeurotic/Open-PS2-Loader/total?style=plastic&logo=github&logoSize=auto&label=Total%20Downloads&labelColor=navy&color=skyblue)
[![Latest release](https://img.shields.io/github/v/release/NathanNeurotic/Open-PS2-Loader?style=plastic&logo=github&label=Latest%20Release&labelColor=navy&color=skyblue&include_prereleases)](https://github.com/NathanNeurotic/Open-PS2-Loader/releases)
[![Discord](https://img.shields.io/discord/1275875800318476381?style=flat&logo=Discord)](https://tinyurl.com/PS2SPACE)
[![Documentation](https://img.shields.io/badge/Documentation-RiptOPL-skyblue?style=flat&logo=githubpages&logoColor=white&labelColor=navy)](https://nathanneurotic.github.io/Open-PS2-Loader/)
[![MEGA Archive](https://img.shields.io/badge/MEGA-Rolling%20Archive-%23D90007?style=flat&logo=mega&logoColor=white)](https://mega.nz/folder/74pRHKRB#9SLDkrkvZAbeKO4Qvxg9LQ)

> **What is RiptOPL?** A downstream fork of Open PS2 Loader with a built-in cover-art **Coverflow** theme (default), a **Favorites** tab, per-game **Neutrino** external-core launching, a reorganized category **settings layout**, DualSense support, and ready-to-use opinionated defaults. Its settings live in their own **`settings_riptopl.cfg`** so they never collide with official OPL or wOPL installed on the same memory card — while artwork, themes, VMCs and **favorites stay shared**. See **[This Fork's Additions](#this-forks-additions)**. For the canonical project, use [ps2homebrew/Open-PS2-Loader](https://github.com/ps2homebrew/Open-PS2-Loader).

> 📖 **Full documentation & guides:** **<https://nathanneurotic.github.io/Open-PS2-Loader/>** — a complete, searchable docs site covering every storage backend, the Neutrino core, PS1/VCD, the Theme Engine (with worked examples and an annotated sample theme), a full settings reference, and troubleshooting.

## External Tools & Services

RiptOPL is intended to work with these maintained companion tools:

- **[PS2-Servers](https://github.com/NathanNeurotic/PS2-Servers)** by **[Ripto](https://github.com/NathanNeurotic)** — all-in-one PC server launcher for **SMBv1, UDPFS and UDPBD**.
- **[udpfs-server](https://github.com/YouKnow-sys/udpfs-server)** by **[YouKnow-sys](https://github.com/YouKnow-sys)** — the same idea **from a phone**: an Android app that shares folders and disk images to the PS2 over **UDPFS**, found by broadcast so there is no server address to type in on the console. Works over a router or a direct cable. Built on **[udpfsd](https://github.com/pcm720/udpfsd)** by **[pcm720](https://github.com/pcm720)**; MIT licensed. A `udpfs-server.url` shortcut ships in every release package.
- **[OrbitPS2 Manager](https://github.com/Luden02/OrbitPS2-Manager)** by **[Luden](https://github.com/Luden02)** — cross-platform PC library manager for importing discs, artwork/screenshots, ZSO compression, per-game settings and VMC management.
- **[OPL PS1 AIO Converter GUI](https://github.com/shaanhomebrew-cloud/OPL-PS1-AIO-Converter-GUI)** by **[shaan](https://github.com/shaanhomebrew-cloud)** — Windows all-in-one PS1/POPStarter preparation tool for converting BIN/CUE backups to VCDs and installing them to USB, MX4SIO, MMCE, iLink, exFAT HDD, SMB and APA internal HDD.
- **[PS2RD CHT Manager](https://github.com/TheRealNextria/PS2RD-CHT-Manager)** by **[TheRealNextria](https://github.com/TheRealNextria)** — PC manager for the PS2RD `.cht` cheat files RiptOPL reads from your device's `CHT` folder. A `PS2RD-CHT-Manager.url` shortcut ships in every release package.
- **[Ember](https://github.com/Gageformer/Ember)** by **[Gageformer](https://github.com/Gageformer)** — a PS1 emulator that runs natively on the PS2, used as RiptOPL's **second PS1 core** alongside POPSTARTER. Unlike the others this one is not just a shortcut: an `EMBER/` folder ships **inside** the release package, ready to drop onto a device. It is bundled unmodified with the author's permission under the Ember Public Beta Testing Licence (`EMBER/LICENSE-BETA.txt` in the package); releases: <https://github.com/Gageformer/Ember/releases>.
- **[POPStarter](https://www.psx-place.com/resources/popstarter.683/)** by **krHACKen** — a PS1 launcher built around Sony's native **POPS** emulator for the PS2, used as RiptOPL's **primary PS1 core** alongside Ember. POPStarter provides the compatibility and launch layer for running PS1 VCDs from USB, MX4SIO, MMCE, iLink, internal HDD, and SMB; RiptOPL's iLink handoff is wired but still awaiting a passing hardware retest. The official POPStarter r13 package contains **no Sony emulator binaries, libraries, or BIOS files**; those components must be supplied separately by the user. Official download, documentation, compatibility information, and releases are maintained on **[PSX-Place](https://www.psx-place.com/resources/popstarter.683/)**.
- **[Neutrino](https://github.com/rickgaiser/neutrino)** by **[rickgaiser](https://github.com/rickgaiser)** — a *"Small, Fast and Modular PS2 Device Emulator"*, and RiptOPL's **second PS2 loader core** alongside OPL's own. Like Ember it is not a shortcut: a ready-to-use `neutrino/` folder ships **inside** the release package, drag-and-drop to `mc?:/neutrino/`. Neutrino is deliberately **UI-agnostic** — it has no interface of its own, which is exactly what lets a front-end like RiptOPL drive it per game. Licensed **AFL-3.0**; releases: <https://github.com/rickgaiser/neutrino/releases>.
## Contents

- [Introduction](#introduction) · [Quick Start](#quick-start) · [Major Features Overview](#major-features-overview) · [Releases](#releases) · [How to Use](#how-to-use) · [USB/MMCE/MX4SIO/iLink](#usbmmcemx4sioilink) · [SMB](#smb) · [HDD](#hdd) · [APPS](#apps) · [Cheats](#cheats) · [NBD Server](#nbd-server) · [ZSO Format](#zso-format) · [PS3 BC](#ps3-bc) · [Frequent Issues](#frequent-issues)

## Introduction

Open PS2 Loader (OPL) is a 100% Open source game and application loader for
the PS2 and PS3 units.
Major capabilities include GSM video mode fixes, Virtual Memory Cards (VMC), PS2RD cheats, DS3/DS4 pad emulation, themes, and homebrew app launching.

It supports six categories of devices:

1. USB mass storage devices;
2. MMCE (Memory Card Mass Storage protocol devices);
3. MX4SIO (SD card connected to memory card port via adapter);
4. iLink (SBP2 compliant storage devices via IEEE 1394);
5. SMB shares (SMBv1 or SMB2, selectable under **Network**);
6. ATA/IDE HDDs, including internal exFAT configurations (MBR/GPT).

Plus an optional **network-block-device boot** (UDPBD / UDPFS, via Neutrino) that streams games
from a PC over the LAN as their own game list — the network protocol defaults to **Off**; the
first protocol you pick in **Network** comes up live. (Network stacks share the one adapter
and stay loaded for the whole boot, so *switching away* from a loaded protocol still needs a
restart — OPL says so when it applies.)
See [This Fork's Additions](#this-forks-additions).

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
- [ ] A RiptOPL build (`RIPTOPL.ELF`) — a tagged `v*` release for stability, or the `rolling` Latest development release for the newest features.
- [ ] Optional: network access (recommended for SMB and remote file management).

### Minimal startup path

1. Download a RiptOPL build (tagged `v*` or `rolling`) from the [Releases](https://github.com/NathanNeurotic/Open-PS2-Loader/releases) page.
2. Copy the `RIPTOPL.ELF` file to your launch method (FMCB, FHDB, or equivalent).
3. Prepare your storage with the expected OPL folders: `DVD`, `CD`, `CFG`, `ART`, `VMC`, and other mode-specific directories as needed.
4. Open OPL settings and enable the device mode you plan to use.
5. Launch one test game, then save settings so OPL reuses your configuration.

For detailed setup steps, jump to the README sections for **USB/MMCE/MX4SIO/iLink**, **SMB**, **HDD**, **APPS**, and **Frequent Issues**.

### Major Features Overview

This section is a fast feature map to improve discoverability of core OPL capabilities and reduce setup friction for first-time and returning users.

- **MMCE support:** OPL supports MMCE devices using the Memory Card Mass Storage protocol for SD-based loading through the Memory Card slot.
- **MX4SIO support:** OPL supports MX4SIO adapters for SD-based loading through the Memory Card slot. See the **USB/MMCE/MX4SIO/iLink** section for filesystem and layout guidance.
- **Internal HDD exFAT support:** the internal ATA HDD can be loaded as **exFAT** — mounted through the Block Device Manager (BDMAssault / "BDMA") into the same `massN:` namespace as USB/MX4SIO — in addition to APA/PFS, including GPT partitioning for large disks, for PS2 **and** PS1 (POPSTARTER) games. See the **HDD** section for formatting, the BDMA equip, and fragmentation guidance.
- **Themes:** Place theme assets in the `THM` folder, then select and apply themes from OPL settings. This fork ships a built-in **`<Coverflow>`** cover-carousel theme (the default) — see the [Theme Engine reference](docs/THEME_ENGINE.md) to author your own themes.
- **Cheats / PS2RD:** OPL supports PS2RD `.cht` cheat files from the `CHT` folder, with both auto-apply and launch-time selection modes.
- **Pad emulation (DS3/DS4):** On any build with PADEMU (the default), a DualShock 3 or DualShock 4 plugged into the console's USB port can navigate the OPL menu right away, with nothing to enable first. To play games with it, turn on **Pad Emulator** under **Settings**, then **Controller Settings** (globally, or per game via **Game Settings**). One caveat: pad emulation shares the SIO2 bus with MX4SIO SD-card loading, so running both can cause a game to hang on a black screen; leave Pad Emulator off if you boot from an MX4SIO card.
- **GSM (video mode handling):** Builds that include GSM allow game video mode handling/overrides for display compatibility.
- **VMC (Virtual Memory Cards):** Create and use VMC images (8MB to 64MB) via the `VMC` folder and per-game options.
- **Per-game settings workflow:** Highlight a game, open **Game Settings**, adjust options (such as compatibility modes, cheats, GSM, PADEMU, and VMC), then save so settings persist per title.
- **App launching (APPS + config methods):** OPL can launch homebrew ELFs using either `conf_apps.cfg` entries or per-app `title.cfg` metadata in `APPS` subfolders.

### This Fork's Additions

This build layers several features on top of upstream OPL:

- **`<Coverflow>` theme (built-in, and the default):** a centered cover-art carousel for
  the game/app list, with an alpha-faded reflection, animated scrolling, a configurable
  cover count, and aspect-correct covers in both 4:3 and widescreen. Tune it live under
  **Coverflow Settings** (shown while the Coverflow theme is active). Authoring details
  and every theme value live in the **[Theme Engine reference](docs/THEME_ENGINE.md)**.
- **Per-device theme placement (`devices=`):** themes can position the device icon, the games
  list and the button hints **differently per device page** (e.g. pin the MMCE icon top-right
  while every other page keeps the shared spot). Add `devices=usb,hdd,…` to a `MenuIcon`,
  `ItemsList` or `HintText` block; the unfiltered element automatically stands down on the pages
  a filtered one covers. Existing themes are untouched. See
  **[Theme Engine reference §5](docs/THEME_ENGINE.md#per-device-placement-devices--this-fork)**.
- **Cover-art `.tar` archive (opt-in):** keep all of a device's covers in a single uncompressed
  **`ART/art.tar`** (entries named `<GAMEID>_<suffix>.png`; VCD entries first use the filename without
  `.VCD`, or the displayed `PP.<name>` / `__.<name>` install name, then fall back to a parsed PS1 ID)
  instead of thousands of loose files.
  Enable **Cover Art .tar Archive** under **Interface → Artwork Settings** (default **off**); when on, each
  cover is read from the archive and *falls back to the loose `.png`* when it isn't there, so the
  two coexist. A small `art_cache.bin` index written beside the archive lets later boots skip the
  re-scan. The format matches wOPL/sOPL art packs, so existing `.tar` packs work unchanged.
- **Favorites tab:** press **R3** on any game to star it; a virtual **Favorites** page
  (alongside the device tabs, switched on in **Game Sources**) gathers your starred games
  from every device into one list, and a star marks favorited titles everywhere. Favorites
  are stored in a shared `favourites.bin`, and RiptOPL will **import an existing uOPL / wOPL
  `favourites.bin` file** if it finds one — so your favorites carry over from those builds.
  Favorites always has its own independent **L3** ring: **All in One → PS2 → PS1 → ELF**.
  The selected row's real type controls its actions and launcher even in All in One; the global
  device-game display setting does not pin or disable this page.
- **Optional APPS / PS1ELF split:** **APPS Display** defaults to **Mixed**, with every configured
  ELF in one list and L3 inert. Choose **Apps / PS1ELF (L3)** to move entries whose displayed title
  contains **`[PS1]`** (case-insensitive) onto a second L3 view. This only organizes the list; both
  views still launch ordinary configured ELFs.
- **Folder browsing (opt-in):** turn on **Browse Folders in Game List** in **Settings** to have
  subdirectories inside your `CD` / `DVD` folders appear as browsable entries (grouped at the
  top of the list, shown with a trailing `/`). Select a folder to open it, and press the
  **cancel button** to go back up — a
  breadcrumb in the page title shows where you are. Each folder view is just the normal game
  list, so covers, favorites, coverflow and per-game settings all work inside folders. Works on
  USB / MX4SIO / iLink / internal-BDM, MMCE and UDPFS-Files. Left off, a flat library looks and
  behaves exactly as before.
- **Controller vibration in the menus (opt-in):** turn on **Controller Vibration in Menus** in
  **Settings** for a little haptic tap as you move around — a light tick when the cursor moves,
  a slightly firmer bump on confirm / cancel / notifications, and one when OPL finishes booting
  and the menu is ready. Needs a **DualShock in analog mode** (a digital-only or clone pad simply
  won't buzz); DS3/DS4/DS5 pads are supported on builds with pad emulation. Left off, nothing
  changes.
> **Credit and licence — Neutrino is created by [rickgaiser](https://github.com/rickgaiser), and its
> official home is <https://github.com/rickgaiser/neutrino>.** Neutrino is an independent PS2 device
> emulator, **not** part of RiptOPL and not our work. Every release package bundles the official
> latest build, re-fetched at publish time, under its **AFL-3.0** licence. We add exactly one file to
> that folder — `config/bsd-udpfsbd.toml`, because Neutrino ships `udpfs_bd.irx` without a matching
> `-bsd` token and RiptOPL launches UDPFS as `-bsd=udpfsbd`. Nothing of rickgaiser's is altered or
> removed. RiptOPL is one of several front-ends built on Neutrino, alongside **NHDDL**, **XEB+
> Plugin**, **RETROLauncher**, **OSD-XMB** and **PSBBN/BBNL**. Please report *launching* problems to
> **us** — the argument building and hand-off are ours — and genuine Neutrino bugs upstream.

- **Neutrino external core (per-game):** hand a game off to an external `neutrino.elf`
  instead of OPL's built-in core, chosen per title, with custom launch flags you can set
  globally and per-game. See **[docs/NEUTRINO.md](docs/NEUTRINO.md)**.
- **UDPBD network boot (Neutrino):** stream games from a PC over the LAN as a network block
  device — they show up as a **UDPBD Games** list with full covers and per-game settings, just
  like a local drive. UDPBD launches via Neutrino, is mutually exclusive with SMB (they share
  the one network adapter), and needs a static PS2 IP (the default is `192.168.1.10`); the
  fork's **network protocol defaults to Off** — pick UDPFS or UDPBD in **Network** and it
  loads live (a restart is only needed to *switch away* from a protocol already loaded). Confirming
  Network Settings applies the current values and reconnects immediately; if the first connection
  fails, press **Select / Refresh** on the failed network page to retry it. Run it from the
  **[PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers)** all-in-one PC launcher. See the
  network-boot section of **[docs/NEUTRINO.md](docs/NEUTRINO.md#4-network-boot--the-network-protocol-selector)**.
- **UDPFS network boot (Neutrino):** a newer network transport (Neutrino's UDPRDMA) offered
  alongside UDPBD. The network controls are split across two pages: **Game Sources** holds the
  **Network Start Mode** row (Off / Manual / Auto), and **Network** holds **Protocol**
  (**SMB / UDPFS / UDPBD**), **SMB Version** (SMBv1 / SMB2, live only while Protocol is SMB),
  and **Access** (Files / IMG — locked to Files
  for SMB and to IMG for UDPBD, free only for UDPFS). UDPFS launches via `-bsd=udpfsbd` with a
  bundled `bsd-udpfsbd.toml`. Use the
  **[PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers)** all-in-one PC launcher for UDPFS,
  SMB and UDPBD; advanced users can run **[pcm720/udpfsd](https://github.com/pcm720/udpfsd)** directly.
  Same static-IP and SMB-exclusivity rules as UDPBD.
<a name="ps1-games-two-cores-one-list"></a>
- **PS1 games — two cores, one list:** RiptOPL plays PS1 titles through **two** cores, and both appear
  together in a single **PS1** library. **PS2/PS1 Game Display** decides whether a device presents
  separate PS2/PS1 views, one combined list with L3 filters, or one locked library.

  - **POPSTARTER** runs `*.VCD` files from `<device>:/POPS/`.
  - **[Ember](https://github.com/Gageformer/Ember)** runs `*.cue` / `*.bin` / `*.exe` from
    `<device>:/EMBER/games/<Game Name>/`.

  The two are interleaved and sorted as one library, so a game you hold for both cores simply shows
  twice. Which core runs a title is a property of that row, decided when you launch it — there is
  nothing to configure and no third toggle.

  **Setting Ember up.** The release package contains a ready-made `EMBER/` folder; copy it to the
  root of a device, beside `POPS/`. Then add **your own PS1 BIOS** as `EMBER/bios.bin` (exactly
  512 KB) — a BIOS is copyrighted, so it is never distributed with RiptOPL, and the folder ships with
  a placeholder file whose name says so. Finally give each game its own folder:

  ```
  <device>:/EMBER/
      ember.elf
      bios.bin                       <- yours, 512 KB
      games/
          Spyro 2 (Ripto's Rage)/
              whatever.cue
              whatever.bin
  ```

  The **folder name** is what you see in the list, and it is also the key for cover art and per-game
  settings — so `ART/Spyro 2 (Ripto's Rage)_COV.png` on that device just works, exactly as it does
  for a `.VCD`. Files inside can be named anything; `.cue` is preferred over `.exe` over `.bin`.
  Ember writes its per-game memory cards (`MC1.vmc`, `MC2.vmc`) into that same folder, so it must be
  on writable media. Available on USB, MX4SIO, iLink, exFAT-ATA, MMCE, SMB, the internal APA/PFS
  hard drive, UDPFS and UDPBD. The two UDP network pages list Ember only; ordinary device pages
  merge Ember and POPSTARTER rows.

  **On an internal APA/PFS drive**, where there is no filesystem root to copy a folder to, the
  `EMBER/` folder goes on a partition of its own: **`__.EMBER`**, or `__.EMBER0` … `__.EMBER9` if you
  want more than one. That is deliberately the same naming shape as the `__.POPS` containers that
  hold your `.VCD` files, so there is only one convention to remember.

  The partition is self-contained — the layout inside it is exactly the one above, starting at the
  partition root:

  ```
  hdd0:__.EMBER
      EMBER/
          ember.elf
          bios.bin                   <- yours, 512 KB
          games/
              Spyro 2 (Ripto's Rage)/
                  whatever.cue
                  whatever.bin
  ```

  Cover art is unchanged and does **not** go here — `ART/<Game Name>_COV.png` on your OPL data
  partition, the same place and the same key a `.VCD` uses.

  One difference is worth knowing about, because it is the reason this works at all: an APA Ember
  title is launched with its partition **still mounted**. Ember inherits that live mount instead of
  re-opening the drive for itself, which is also why the partition is mounted read/write — Ember's
  memory cards and `settings.txt` are written straight through it.

  **Credit and licence — Ember is created by [Gageformer](https://github.com/Gageformer), and its
  official release page is <https://github.com/Gageformer/Ember/releases>.** Ember is an
  independent PS1 emulator written from scratch for the PS2; it is **not** part of RiptOPL and is
  not our work. We bundle it unmodified and with the author's blessing, under the **Ember Public
  Beta Testing Licence**, which ships in the release package as `EMBER/LICENSE-BETA.txt` and governs
  the build it accompanies. That licence permits non-commercial bundling but prohibits selling,
  modifying or repackaging the build. Ember contains no PlayStation BIOS and no game data — you
  supply your own, from hardware and media you lawfully own. Please report Ember problems to *us*
  first rather than to Gageformer: the launching is ours.

  **Ember display mode.** The **PS Emulation Settings** page carries an *Ember Display Mode* setting
  (**Default** / **240p** / **480p**). Ember's `settings.txt` is an optional file, and this keeps it
  that way. Choose **240p** or **480p** and RiptOPL writes that key into
  `<device>:/EMBER/settings.txt` when you launch an Ember title — creating the file if it is not
  there, updating it in place if it is, and on the launching device only. Choose **Default** and it
  goes back to having no setting: the key is removed, and so is the file if that key was all it
  held. Any other lines you or a future Ember put in there are preserved throughout, and *Default*
  never creates a file that was not already there.

- **Alternate POPSTARTER builds:** the release package ships
  `POPS/POPSTARTER VERSIONS/` containing five builds of POPSTARTER — **MAIN**, **DEBUG**,
  **USBDELAY**, **USBDELAY_DEBUG** and **USBDELAY_LONGER_DEBUG**. To switch, copy the
  `POPSTARTER.ELF` you want over `POPS/POPSTARTER.ELF` on your device; nothing in RiptOPL selects
  them for you. The `USBDELAY*` builds give USB devices longer to settle before POPSTARTER reads
  them, and the `*DEBUG*` builds print diagnostics on screen — use one of those when reporting a
  POPSTARTER problem.

  **The shipped `POPS/POPSTARTER.ELF` is the DEBUG build, and that is deliberate** — it is the build
  with SMB support, so it is the right default for the package. On-screen POPSTARTER diagnostics
  during a VCD launch are expected and are not a fault. Do not overwrite it with **MAIN** unless you
  are certain you do not need SMB; the alternate builds are here for the `USBDELAY` cases.

- **Where POPSTARTER's VCDs come from:** the PS1 library belongs to a device page, not a separate
  device tab. The shared **PS2/PS1 Game Display** setting is shown on both **Interface** and
  **PS Emulation Settings**. **Both (L3)** (the default) switches between separate PS2 and PS1
  views; **Mixed** starts with both in one list and L3 cycles Mixed → PS2 → PS1; **PS2** and **PS1**
  lock every applicable device page to one library and make L3 fully inert (no hint, sound,
  notification, or pause). APPS and Favorites remain independent. A `*.VCD` row boots through
  **POPSTARTER** — never OPL's own core and never Neutrino, so the Loader Core selector is inert for
  it. POPSTARTER VCDs work on USB / MMCE / MX4SIO / SMB **and the internal HDD** — both APA
  (exact `__.POPS[0-9]?` containers plus `PP.<name>` / `__.<name>` one-game partitions containing
  `IMAGE0.VCD`) and **exFAT** (BDMA; PS1 games in `massN:/POPS/`). Ember's half of the list has its
  own device coverage, noted above. The iLink POPSTARTER path is wired but still awaiting a passing
  hardware test: RiptOPL reads `POPS/usbd.irx.ilink` and
  `POPS/usbhdfsd.irx.ilink`, copies them to the memory card without the `.ilink` suffix, and hands
  POPSTARTER the normal local-device `XX.` selector, but revision 2692 returned to wLaunchELF in all
  four SDK flavours. Every BDMA pair ships as loose files in
  `POPS/`; none is embedded in `RIPTOPL.ELF`. UDPFS and UDPBD also expose a PS1 view, but those
  network pages list **Ember titles only**: Ember inherits the live connection, while POPSTARTER's
  IOP reset cannot restore either network transport. See **[docs/VCD.md](docs/VCD.md)**.
- **Core-aware per-game settings:** the per-game screen adapts to the selected **Loader Core** —
  under Neutrino it greys the panels Neutrino ignores (GSM, Cheats, PADEMU, OSD Language and the
  OPL-only compat modes) and offers a structured **Neutrino Video** picker (Off / 240p / 480p /
  1080i) plus a Neutrino-only **Mode 7** (`-gc=7`). Its global **Default Device** picker can also
  target a complete `neutrino/` folder on iLink explicitly. iLink is a FAT-model Neutrino backend;
  post-2692 builds automatically pair `-bsd=ilink` with `-qb` so Neutrino keeps the mounted iLink
  environment RiptOPL hands it. See **[docs/NEUTRINO.md](docs/NEUTRINO.md)**.
- **Category settings layout:** the start menu's settings are organised into eight category pages
  instead of one flat list — **Game Sources** (device selection + start modes), **General & System**,
  **Network**, **Interface** (theme, artwork, Coverflow, PS2/PS1 Game Display), **Game Launching**
  (incl. the global Neutrino/OSD defaults), **PS Emulation Settings** (both PS1 cores: POPSTARTER
  and Ember, plus the same PS2/PS1 Game Display picker shown on Interface), **Controller Settings** and
  **Audio Settings** — each with chained sub-pages, plus a **Save Changes** entry at the foot of the
  index. Leaving the start/settings menu returns to the page you paused on when it is still visible;
  it no longer falls back to the first USB page.
- **DualSense / DualShock 5 (USB):** optional controller support — available in the prebuilt
  `RIPTOPL-VARIANTS-*.zip` release bundle (one ELF per SDK flavour), or build with `make DUALSENSE=1`.
- **1080p GSM video mode:** forced progressive 1080p (1920×1080) GSM mode is built directly into all standard builds (`make GSM1080P=1`). Selecting 1080p in the per-game GSM picker is guarded by a **three-step confirmation**; if your display cannot sync it, holding **Triangle + Cross** on console boot forces safe 480p progressive mode.
- **Ready-to-use defaults:** a fresh install boots with sensible options already enabled —
  widescreen, cover art, notifications, sound effects + boot sound, delete/rename, and
  the PS2 logo. Video mode stays **Auto**. Every storage device ships **off**, so the first boot
  lands on the start menu with no tabs — enable exactly the devices your console has under
  **Game Sources**. Change any of it under Settings.
- **Private settings, shared data:** RiptOPL saves its master config as **`settings_riptopl.cfg`**
  (auto-migrated from the older `conf_riptopl.cfg`; not `conf_opl.cfg`), so it can sit on the same memory card as official OPL or wOPL without
  either build clobbering the other's settings. Everything else under the `OPL/` folder —
  artwork, themes, VMCs, per-game configs, and **favorites** — stays **shared** between builds.

## Acknowledgements

This fork stands entirely on the shoulders of the PS2 homebrew community. **None of this
would exist without the [ps2homebrew](https://github.com/ps2homebrew) team** and their many
years of open-source work on Open PS2 Loader and the PS2SDK — kept free, open, and readable
so that people like us can study it, learn from it, and build on it. Every feature in this
fork began as *their* code and *their* ideas. We are deeply grateful that this work was
shared openly; it is the only reason a fork like this is even possible.

RiptOPL is a **direct agglomeration** of the wider OPL family, bringing together features, code,
and ideas from [rickgaiser's OPL](https://github.com/rickgaiser/Open-PS2-Loader),
[neutrino](https://github.com/rickgaiser/neutrino),
[sOPL](https://github.com/mystyq/Stable-Open-PS2-Loader), [uOPL](https://github.com/Wolf3s/uOPL),
[wOPL](https://github.com/KrahJohlito/wOPL), [OPL DB](https://github.com/Jay-Jay-OPL/OPL-Daily-Builds),
[POPSLoader](https://github.com/NathanNeurotic/POPSLoader),
[OPL RetroGEM ID by CosmicScale](https://github.com/CosmicScale/Open-PS2-Loader-Retro-GEM),
[nhddl](https://github.com/pcm720/nhddl),
[Modulo-R1](https://github.com/AdityaKumar7209/Modulo-R1-Beta-Preview---PS2),
[PS2-Launcher](https://github.com/Irfanlesnar/PS2-Launcher), and
[official OPL](https://github.com/ps2homebrew/Open-PS2-Loader).

With special and sincere thanks to:

- **KrahJohlito** — the legend, and the single biggest influence on this fork. Creator of
  **uOPL (Unofficial Open PS2 Loader)** and its continuation
  **[wOPL](https://github.com/KrahJohlito/wOPL)**, where the modern OPL experience was
  invented. The Neutrino external-core loader, the Coverflow interface, and the Favorites
  tab — the features that define RiptOPL — were designed and pioneered by him, and everything
  this fork does with them is a reimplementation of his work. We learned more reading his
  code than anywhere else, and RiptOPL is, above all, a tribute to it. Thank you.
- **Wolf3s** — for contributions across the wOPL effort and the wider OPL scene, and for
  maintaining an independent **[uOPL](https://github.com/Wolf3s/uOPL)** fork that keeps
  unique features and unmerged work alive. Thank you.
- **bbsan2k** — for the **MMCE (Memory Card Mass Storage) protocol** that makes SD-via-memory-card
  loading through the PS2's memory-card slot possible. OPL's MMCE support builds directly on it.
- **saildot4k** — for **BDMA-ATA** (exFAT internal-HDD block-device support), and the fixes,
  feedback, and oversight that shaped this fork's block-device work. A big piece of getting it right.
- **eliminator1403** — for dedicated **testing, bug reports, and real-hardware feedback** that
  has repeatedly caught issues and shaped fixes across this fork. Invaluable QA.
- **Berion** — for the artwork and theme design that has shaped how OPL *looks* for years.
  The visual language this fork builds on owes a great deal to that craft.
- **AdityaKumar7209** — whose [**Modulo-R1**](https://github.com/AdityaKumar7209/Modulo-R1-Beta-Preview---PS2)
  project inspired this fork's **folder browsing** in the game list. We didn't use their code, but the
  idea of navigating game subfolders came from seeing it there — thank you for the spark.
- **Irfanlesnar** — creator of [**PS2-Launcher**](https://github.com/Irfanlesnar/PS2-Launcher),
  for UI, feature ideas, and contributions across the OPL fork ecosystem.
- **Ifcaro** and **jimmikaelkael** — the original Open PS2 Loader authors — and every
  contributor across OPL's long history.

### The wider Open PS2 Loader team

RiptOPL inherits the work of everyone who built Open PS2 Loader over the years. They are
credited in full in [CREDITS](CREDITS), and named here so this fork never obscures whose
work it is built on:

- **Core developers** — Ifcaro, volca, jimmikaelkael, polo35, izdubar, hominem.te.esse and
  SP193, with the original main code based on **Polo**'s HD Project.
- **Contributing developers** — BatRastard, crazyc, dlanor, doctorxyz, reprep, belek666,
  Maximus32 and misfire.
- **Module authors** — **Eugene Plotnikov** (SMSUTILS / SMSMAP / SMSTCPIP), **Marcus R. Brown**
  (DEV9 / ATAD and the derived cdvdman code), **bbsan2k** (MMCE), **icyson55** (OPL-CL /
  network update), **Eric Young** (the DES algorithm in the SMB code), and the **ps2dev** team
  (USB / Network / PS2HDD modules from the PS2SDK).
- **CI/CD** — **fjtrujy** (Docker + GitHub Actions).
- **UI & artwork** — **Berion**.
- **Quality assurance** — RandQalan, yoshi314, EP, LocalH, lee4, El_Patas, ShaolinAssassin,
  algol, gledson999, jolek and zero35.

### Real-hardware testing (this fork)

Enormous thanks to the testers who run every rolling build on real consoles and file the
reports that shape the fixes — **eliminator1403, lucaslmgv, AndrewBento, AcidReach, bodvenomz,
nuno6573, zackcage6 and Blade1984**.

### The name (this fork)

**RiptOPL is named by [Akilluminati47](https://github.com/akilluminati47)** — he came up with it
before the project existed, and it stuck. Every release, every build, every thread carries a name
he thought of first.

### Financial support (this fork)

Heartfelt thanks to **Akilluminati47** for generously **funding this fork's development** — a
kindness that keeps the rolling builds coming and is deeply appreciated.

If you want the canonical, actively-maintained project, it lives at
**[ps2homebrew/Open-PS2-Loader](https://github.com/ps2homebrew/Open-PS2-Loader)** — please
support it. This fork is a downstream labor of love, not a replacement, and it exists only
because that upstream work is open for everyone to learn from.

## Releases

RiptOPL ships **one full-feature build** — GSM video-mode handling (including 1080p), DS3/DS4 pad
emulation (PADEMU), VMC, PS2RD cheats and parental controls are all included in the
standard ELF (no upstream-style per-feature variants). The two upstream `EXTRA_FEATURES`
extras — in-game screenshots (IGS) and right-to-left (RTL) language support — are **not**
compiled into any published main ELF (`EXTRA_FEATURES ?= 0`); they ship in the
`EXTRA_FEATURES=1` builds inside the VARIANTS zip.
DualSense / DualShock 5 (USB) support is available prebuilt in the `RIPTOPL-VARIANTS-*.zip`
bundle, or build your own with `make DUALSENSE=1`.

**RetroAchievements** ships as its own flavour in the same VARIANTS bundle
(`RIPTOPL-ra-pademu*.ELF`, or `make RETROACHIEVEMENTS=1`). It is a **development build, not a
finished feature** — both halves are now written, the menu side included, but none of it has run on a
real console yet — and the standard ELF is completely unaffected by it. See
**[docs/RETROACHIEVEMENTS.md](docs/RETROACHIEVEMENTS.md)** for what it does, which launch paths can
ever support it, and why.

There are two release channels:

| Channel | What it is |
| --- | --- |
| **Rolling (Latest)** (the `rolling` tag) | Continuously rebuilt from the publishing branch on every push — currently `rebuild/main` — and intentionally published as GitHub's **Latest**, full (non-pre-release) development release. Its final download set has up to five archives: the full installable package (`RIPTOPL-<rel>-<sha>.zip`, normally containing all four labelled SDK loader folders + the bundled Neutrino and Ember cores + the canonical POPS folder + five companion-tool shortcuts), plus VARIANTS, exact source, and the DEBUG/language archives when those optional jobs produce them. Best-effort flavours and optional packs are called out if omitted. Floating ELFs, checksums, and SDK/IRX manifests are deliberately removed from GitHub release assets by the normalizer. It is the bleeding edge and may be unstable. |
| **Tagged releases** (`v*` tags) | Curated, known-good versions cut from a tag. Use these for stability. |

See **[ROLLING_RELEASE.md](ROLLING_RELEASE.md)** for exactly what the rolling release
contains and how to pull it.

> **Which rolling build?** The rolling zip ships four loader ELFs that differ only by build
> toolchain — the RiptOPL code in each is identical. Recommended in order of reliability:
> 1. **`APP_RIPTOPL-PS2DEVPINNED/`** (`-PS2DEVPINNED`) — **recommended primary download.** Built on the
>    `ps2dev/ps2dev` SDK pinned by image digest for reproducible, stable behavior.
> 2. **`APP_RIPTOPL-OFFICIALPINNED/`** (`-OFFICIALPINNED`) — **recommended official pin.** Built on the
>    `ps2homebrew/ps2homebrew` official SDK, pinned by image digest.
> 3. **`APP_RIPTOPL-PS2DEVROLLING/`** (`-PS2DEVROLLING`) — **bleeding-edge canary.** Tracks `ps2dev/ps2dev:latest`.
> 4. **`APP_RIPTOPL-OFFICIALROLLING/`** (`-OFFICIALROLLING`) — **bleeding-edge official canary.** Tracks `ps2homebrew:main`.
> See [Which build should I use?](ROLLING_RELEASE.md#which-build-should-i-use).

> 🗄️ **Permanent archive (MEGA):** the GitHub `rolling` release only ever holds the *latest*
> build — every push overwrites it. So **every** rolling build is also archived permanently to MEGA
> as one self-contained zip of the installable payload (all four loader ELFs, the installable
> package zip, the source snapshot, `SHA256SUMS.txt`, and the IRX manifests — the large VARIANTS
> and DEBUG diagnostic bundles stay on the GitHub release only). Click the **MEGA**
> badge at the top of this README — or [browse the archive here](https://mega.nz/folder/74pRHKRB#9SLDkrkvZAbeKO4Qvxg9LQ) —
> to fetch any past build. Each is stored immutably under `RiptOPL/Rolling/<version>/run_<number>/`,
> so nothing is ever overwritten.

## How to use

OPL uses the following directory tree structure across all supported devices —
USB, MMCE, MX4SIO, iLink, SMB, and the internal HDD:

| Folder | Description                                          | Modes       |
| ------ | ---------------------------------------------------- | ----------- |
| `CD`   | for games on CD media - i.e. blue-bottom discs       | All folder devices¹ |
| `DVD`  | for DVD5 and DVD9 images (if filesystem supports +4gb files) | All folder devices¹ |
| `VMC`  | Virtual Memory Card images (headline save feature): stored in `VMC/`, typically 8MB to 64MB, then assigned per game via **Game Settings** | all         |
| `CFG`  | for saving per-game configuration files              | all         |
| `ART`  | for game art images                                  | all         |
| `THM`  | for themes support                                   | all         |
| `LNG`  | for translation support                              | all         |
| `CHT`  | for cheats files                                     | all         |
| `APPS`  | for ELF files                                       | all         |

¹ **Folder-based devices** — USB, MMCE, MX4SIO, iLink, SMB, and the **exFAT** (BDM) HDD — keep games as files in the `CD`/`DVD` folders. The **APA/PFS** HDD instead stores games as HDLoader partitions (no `CD`/`DVD` folders), while still using `CFG`/`ART`/`VMC`/`THM`/`CHT`/`LNG`/`APPS` on the configured OPL data partition (`+OPL` root by default, or `__common/OPL/` for the legacy layout).

Per-game settings are stored per title in the `CFG` context. Typical use cases include compatibility toggles, video options (GSM), cheat toggles, and assigning a VMC file from the `VMC` folder to that game.

OPL will automatically create the above directory structure the first time you launch it and enable your favorite device.

For HDDs formatted with the APA partition scheme, OPL will read `hdd0:__common/OPL/conf_hdd.cfg` for the config entry `hdd_partition` to use as your OPL partition.
If not found a config file, a 128Mb `+OPL` partition will be created. You can edit the config if you wish to use/create a different partition.
All partitions created by OPL will be 128Mb (it is not recommended to enlarge partitions as it will break LBAs, instead remove and recreate manually with uLaunchELF at a larger size if needed).
	
HDDs are also able to be formatted as exFAT to avoid the 2TB limitation.  Please see below in the `HDD` section for more details on this configuration.

## USB/MMCE/MX4SIO/iLink

Supported file systems:
exFAT (since OPL v1.2.0 beta - rev1880) and FAT32, both use the MBR partition table. This section applies to MMCE and MX4SIO SD setups, USB storage, and iLink SBP2 storage.

> [!WARNING]
> **Revision 2692 iLink status is not a blanket pass.** On an SCPH-39001, Ember passed from the same
> IEEE 1394 disk in all four SDK flavours, while the native OPL core, Neutrino, and POPSTARTER failed
> at their reset/handoff boundaries. The next build corrects a confirmed RiptOPL-side Neutrino
> handoff mismatch (`-qb` was not auto-emitted for iLink); only hardware retesting can establish
> whether that was the complete cause. POPSTARTER's
> external `.ilink` BDMA equip and `mass:/POPS/XX.<name>.ELF` handoff are present, but remain
> unconfirmed, and the native OPL iLink reset path is still under investigation. See
> [Neutrino](docs/NEUTRINO.md) and [PS1/VCD](docs/VCD.md) for the exact status and isolation checks.

> [!NOTE]
> MX4SIO game launch requires the matching PS2SDK `freesio2` module to load before
> `mx4sio_bd` after OPL resets the game IOP. See [MX4SIO game-launch notes](docs/MX4SIO.md)
> for the dependency, regression history, and hardware test checklist.

Game files should be *ideally* defragmented either file by file or by whole drive.

> NOTE: Partial file fragmentation is supported (up to 64 fragments!) since OPL v1.2.0 beta - rev1893

If you choose to use the FAT32 file system, games larger than 4gb must use USBExtreme format (see OPLUtil or USBUtil programs).

We do **not** recommend using any defrag programs. The best way for defragmenting - copy all files to pc, format USB, copy all files back.
Repeat it once you faced defragmenting problem again.

## SMB

For loading games by SMB protocol, you need to share a folder (ex: PS2SMB)
on the host machine or NAS device and make sure that it has full read and
write permissions. USB Advance/Extreme format is optional - \*.ISO images
are supported using the folder structure above.

> **SMB version:** the **Network** page has an **SMB Version** row (SMBv1 / SMB2) directly under
> **Protocol**, live only while **Protocol** is **SMB** (greyed out otherwise). It defaults to
> **SMBv1**; setting it to **SMB2** switches *both* sides — browsing loads the SMB2 driver instead
> of the SMBv1 one, and so does the in-game reader — so the server must speak SMB2 for the whole
> session.
>
> **RiptOPL network defaults:** the network protocol selector defaults to **Off** — under
> **Game Sources** set **Network Start Mode** to **Manual** or **Auto**, then in **Network** set
> **Protocol** to **SMB**, before the **NET Games** tab appears. Network Config
> ships static defaults (PS2 `192.168.1.10`, PC `192.168.1.100`, share `games`, user `guest`);
> adjust them to your LAN. The default **SMB Port is `1111`** — a non-privileged port (>1024), so a server
> binds it without admin/root. **Network Config** now opens with **advanced options on**, so
> the **Port** field (and ETH link mode) are editable immediately. If Windows 10/11 has
> disabled SMB1/NTLMv1, set **SMB Version** to **SMB2**; the
> **[PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers)** all-in-one launcher remains the
> fallback for SMBv1-only setups. Choose its SMBv1 server, then set RiptOPL's
> IP, port, share and credentials to the values the launcher displays (PS2 Servers currently uses
> port **1445** by default, so change RiptOPL's saved **1111** when prompted). Release packages
> include **`PS2-Servers.url`** as a direct shortcut to the repository.

## HDD
	
Both PS2 HDD types are **off by default** in RiptOPL — enable the one you use under **Game
Sources**. For PS2, 48-bit LBA internal HDDs are supported. The HDD can be formatted as:

- APA partitioning with PFS filesystem (up to 2TB)
	- OPL will create the `+OPL` partition on the HDD.  To avoid this, create `hdd0:__common/OPL/conf_hdd.cfg` containing the entry `hdd_partition=__common` (or whichever partition you prefer) — the same file and key described above.
- MBR partitioning (up to 2TB) or GPT partitioning (unlimited) with the exFAT filesystem
	- Enable **BDM HDD** in **Game Sources**. The exFAT HDD then mounts through the Block Device Manager (BDMAssault / "BDMA") into the shared `massN:` namespace — the same path as USB/MX4SIO — and appears as an **HDD (exFAT)** games list with the HDD icon.
	- Files should be added contiguously or synchronously to avoid fragmentation. For example, drag and drop files one at a time, or ensure that files are added sequentially.
	- When formatting drives for the exFAT filesystem, please make sure the `Allocation unit size` is set to `Default`.
	- **PS1 games:** PS1 `*.VCD` titles in the HDD's `POPS/` folder list under the **L3** PS1 view like any other device. To boot them, open **PS Emulation Settings → BDMA Settings** from the main menu. **VCD BDMA Apply on Launch** is on by default and equips the matching exFAT driver automatically; turn it off to reveal the manual **BDMA Source** / **BDMA Mode** pickers and set **BDMA Mode → HDD (exFAT)** by hand so POPSTARTER can read the exFAT volume. See **[docs/VCD.md](docs/VCD.md)**.

## APPS

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
3. Add the device prefix and ELF path (for example `mass:` for USB/MX4SIO/iLink/exFAT-HDD, `mmce:` for MMCE, `mc:` for the Memory Card, or `hdd0:`/`pfs0:` for the APA HDD), then the file path to the ELF.

> NOTE: Enter the exact path and exact letter case. OPL is case-sensitive.

The structure should look like this:

```
My App Name=mass:APPS/MYAPP.ELF
```

let's use OPL itself as an example:

```
OPL=mass:APPS/RIPTOPL.ELF
```

With this method, ELFs do not need to be in `APPS`, but keeping them there can make your setup easier to manage.

The `conf_apps.cfg` file can be placed in the `OPL/` folder on your Memory Card or storage device (e.g., `mc0:OPL/conf_apps.cfg`, `mass0:OPL/conf_apps.cfg`), or at the root of the storage device.

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
boot=RIPTOPL.ELF
```

In this method, both the ELF and `title.cfg` must be in the same folder under `APPS`.

> NOTE: In both methods, pay close attention to file names because, as already mentioned, OPL is case-sensitive.

## Cheats

OPL accepts `.cht` files in PS2RD format. Each cheat file corresponds to a specific game and must be stored in the `CHT` directory on your device.
Cheats are structured as hexadecimal codes, with proper headers as descriptions to identify their function.
You can activate cheats via OPL's graphical interface. Navigate to a games settings, enable cheats and select the desired mode.

### Cheat Modes

  * Auto Select Cheats:  
This mode will enable and apply all cheat codes in your `.cht` file to your game automatically.

  * Select Game Cheats:  
When enabled a cheat selection menu will appear when you launch a game. You can navigate the menu and disable undesired cheats for this launch session. Master Codes cannot be disabled as they are required for any other cheats to be applied.

## NBD Server

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

  * Use the latest release or pre-release from the [Releases](https://github.com/NathanNeurotic/Open-PS2-Loader/releases) page if you need newer NBD fixes.
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

## ZSO Format

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

## PS3 BC

Currently, supported only [PS3 Backward Compatible](https://www.psdevwiki.com/ps3/PS2_Compatibility#PS2-Compatibility) (BC) versions. So only [COK-001](https://www.psdevwiki.com/ps3/COK-00x#COK-001) and [COK-002/COK-002W](https://www.psdevwiki.com/ps3/COK-00x#COK-002) boards are supported. USB, SMB, HDD modes are supported.

To run OPL, you need an entry point for running PS2 titles. You can use everything (Swapmagic PS2, for example), but custom firmware with the latest Cobra is preferred. Note: only CFW supports HDD mode.

## Some notes for DEVS

Open PS2 Loader needs the [**latest PS2SDK**](https://github.com/ps2dev/ps2sdk)

## OPL Archive

Every RiptOPL rolling build is permanently archived to MEGA under immutable folders (`RiptOPL/Rolling/<version>/run_<number>/`). You can access the archive by clicking the MEGA badge at the top of this readme or visiting the [MEGA Rolling Archive](https://mega.nz/folder/74pRHKRB#9SLDkrkvZAbeKO4Qvxg9LQ).

## Frequent Issues

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
3. **Recovery steps:** Hold __`Triangle + Cross`__ while OPL initializes to force the video mode to __`480p progressive`__ — a mode virtually every display syncs (Auto resolves to interlaced 480i/576i, which is exactly what some modern displays/upscalers can't lock onto). Once you can see the UI, pick your preferred mode under **Settings**.
4. **Verification:** Start OPL again normally and confirm the interface appears and remains visible.

For GSM/video-mode mistakes, use the same recovery combo above: hold __`Triangle + Cross`__ at boot to force __`480p`__ for OPL's own UI (per-game GSM overrides only apply at game launch and don't affect the OPL menu).

If your issue is still unresolved, report it here: <https://www.psx-place.com/threads/open-ps2-loader-game-bug-reports.19401/>.

