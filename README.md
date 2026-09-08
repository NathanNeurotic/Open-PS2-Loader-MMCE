
<p align="center"><img alt="RiptOPL" src="https://raw.githubusercontent.com/NathanNeurotic/Open-PS2-Loader/rebuild/main/docs/assets/riptopl.png" /></p>



<p align="center">
  <img width="400" height="92" alt="AI-Assisted-Software-Lovers-Only" src="https://github.com/user-attachments/assets/71335775-9fe3-4507-ac2c-caa851abb24c" />
</p>



# RiptOPL
**An opinionated [Open PS2 Loader](https://github.com/ps2homebrew/Open-PS2-Loader) fork — aiming to be the "definitive build."**
<br>
Based on Open PS2 Loader · Copyright 2013, Ifcaro & jimmikaelkael<br>
Licensed under Academic Free License version 3.0<br>
Review the LICENSE file for further details.<br><br>

[![CI](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/flavours.yml/badge.svg?branch=rebuild/main)](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/flavours.yml)
[![Format](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/check-format.yml/badge.svg?branch=rebuild/main)](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/check-format.yml)
[![Rolling Release](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/rolling-release.yml/badge.svg?branch=rebuild/main)](https://github.com/NathanNeurotic/Open-PS2-Loader/actions/workflows/rolling-release.yml)
[![Latest release](https://img.shields.io/github/v/release/NathanNeurotic/Open-PS2-Loader?style=plastic&logo=github&label=Latest%20Release&labelColor=navy&color=skyblue&include_prereleases)](https://github.com/NathanNeurotic/Open-PS2-Loader/releases)
![Released](https://img.shields.io/github/release-date-pre/NathanNeurotic/Open-PS2-Loader?style=plastic&logo=github&label=Released&labelColor=navy&color=skyblue)
![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/NathanNeurotic/Open-PS2-Loader/total?style=plastic&logo=github&logoSize=auto&label=Total%20Downloads&labelColor=navy&color=skyblue)
[![MEGA Archive](https://img.shields.io/badge/MEGA-Rolling%20Archive-%23D90007?style=flat&logo=mega&logoColor=white)](https://mega.nz/folder/74pRHKRB#9SLDkrkvZAbeKO4Qvxg9LQ)

[![License](https://img.shields.io/github/license/NathanNeurotic/Open-PS2-Loader?style=flat&labelColor=navy&color=skyblue&logo=opensourceinitiative&logoColor=white&label=License)](LICENSE)
![Written in C](https://img.shields.io/badge/Written%20in-C-skyblue?style=flat&logo=c&logoColor=white&labelColor=navy)
![Platform](https://img.shields.io/badge/Platform-PlayStation%202-skyblue?style=flat&logo=playstation2&logoColor=white&labelColor=navy)
![Toolchain](https://img.shields.io/badge/Toolchain-ps2dev%20%2F%20PS2SDK-skyblue?style=flat&logo=docker&logoColor=white&labelColor=navy)
![Translations](https://img.shields.io/badge/Translations-31%20languages-skyblue?style=flat&logo=googletranslate&logoColor=white&labelColor=navy)
![Repo Size](https://img.shields.io/github/repo-size/NathanNeurotic/Open-PS2-Loader?style=flat&labelColor=navy&color=skyblue&logo=github&label=Repo%20Size)

![Last Commit](https://img.shields.io/github/last-commit/NathanNeurotic/Open-PS2-Loader/rebuild/main?style=flat&labelColor=navy&color=skyblue&logo=git&logoColor=white&label=Last%20Commit)
![Commits per month](https://img.shields.io/github/commit-activity/m/NathanNeurotic/Open-PS2-Loader/rebuild/main?style=flat&labelColor=navy&color=skyblue&logo=github&label=Commits%2FMonth)
![Contributors](https://img.shields.io/github/contributors/NathanNeurotic/Open-PS2-Loader?style=flat&labelColor=navy&color=skyblue&logo=github&label=Contributors)
[![Open Issues](https://img.shields.io/github/issues/NathanNeurotic/Open-PS2-Loader?style=flat&labelColor=navy&color=skyblue&logo=github&label=Open%20Issues)](https://github.com/NathanNeurotic/Open-PS2-Loader/issues)
[![Stars](https://img.shields.io/github/stars/NathanNeurotic/Open-PS2-Loader?style=flat&labelColor=navy&color=skyblue&logo=github&label=Stars)](https://github.com/NathanNeurotic/Open-PS2-Loader/stargazers)

![Storage](https://img.shields.io/badge/Storage-USB%20%C2%B7%20MX4SIO%20%C2%B7%20iLink%20%C2%B7%20MMCE%20%C2%B7%20HDD-2ea043?style=flat&labelColor=0b3d18)
![Network](https://img.shields.io/badge/Network-SMB%20%C2%B7%20UDPBD%20%C2%B7%20UDPFS%20%C2%B7%20HTTP-2ea043?style=flat&labelColor=0b3d18)
![PS2 Cores](https://img.shields.io/badge/PS2%20Cores-OPL%20%C2%B7%20Neutrino-2ea043?style=flat&labelColor=0b3d18)
![PS1 Cores](https://img.shields.io/badge/PS1%20Cores-POPStarter%20%C2%B7%20Ember-2ea043?style=flat&labelColor=0b3d18)
![RetroAchievements](https://img.shields.io/badge/RetroAchievements-dev%20build%2C%20untested-orange?style=flat&labelColor=7a3e00)

[![Discord](https://img.shields.io/discord/1275875800318476381?style=flat&logo=Discord)](https://tinyurl.com/PS2SPACE)
[![Documentation](https://img.shields.io/badge/Documentation-RiptOPL-skyblue?style=flat&logo=githubpages&logoColor=white&labelColor=navy)](https://nathanneurotic.github.io/Open-PS2-Loader/)

> **What is RiptOPL?** A downstream fork of Open PS2 Loader with a built-in cover-art **Coverflow** theme (default), a **Favorites** tab, per-game **Neutrino** external-core launching, a reorganized category **settings layout**, DualSense support, and ready-to-use opinionated defaults. Its settings live in their own **`settings_riptopl.cfg`** to keep its master settings separate from official OPL and wOPL. Other files, including network and per-game settings, can still be shared. Favorites import from uOPL/wOPL is **one-way**: the next save writes RiptOPL’s own format to `favourites.bin`. See [Where your files live](#where-your-files-live). See **[This Fork's Additions](#this-forks-additions)**. For the canonical project, use [ps2homebrew/Open-PS2-Loader](https://github.com/ps2homebrew/Open-PS2-Loader).

> 📖 **Full documentation & guides:** **<https://nathanneurotic.github.io/Open-PS2-Loader/>** — searchable setup and reference guides covering storage backends, the Neutrino core, PS1/VCD, the Theme Engine (with worked examples and an annotated sample theme), a full settings reference, and troubleshooting.

## Contents

[Releases](#releases) · [Quick Start](#quick-start) · [Sources and cores](#introduction) ·
[Features](#major-features-overview) · [Fork additions](#this-forks-additions) ·
[PS1](#ps1-games-two-cores-one-list) · [HTTP](docs/HTTP.md) · [RetroAchievements](docs/RETROACHIEVEMENTS.md) ·
[Files and folders](#how-to-use) · [USB/MMCE/MX4SIO/iLink](#usbmmcemx4sioilink) ·
[SMB](#smb) · [HDD](#hdd) · [APPS](#apps) · [Cheats](#cheats) · [NBD](#nbd-server) ·
[ZSO](#zso-format) · [PS3 BC](#ps3-bc) · [Troubleshooting](#frequent-issues) ·
[Companion tools](#external-tools--services) · [Credits](#acknowledgements)

## Releases

RiptOPL ships **one full-feature build** — GSM video-mode handling (including 1080p), DS3/DS4 pad
emulation (PADEMU), VMC, PS2RD cheats and parental controls are all included in the
standard ELF (no upstream-style per-feature variants). The two upstream `EXTRA_FEATURES`
extras — in-game screenshots (IGS) and right-to-left (RTL) language support — are **not**
compiled into any published main ELF (`EXTRA_FEATURES ?= 0`); they ship in the
`EXTRA_FEATURES=1` builds inside the VARIANTS zip.
DualSense / DualShock 5 (USB) support is available prebuilt in the `RIPTOPL-VARIANTS-*.zip`
bundle, or build your own with `make DUALSENSE=1`.

**RetroAchievements** ships as its own complete package, `RIPTOPL-RA-*.zip` (or build it with
`make RETROACHIEVEMENTS=1`). It is laid out like the main archive — same `POPS/`, `EMBER/`,
`neutrino/` and shortcuts — with the RA loader in place of the standard one, plus a shortcut to
**xeRAbora**, the PC client the feature talks to. It is a **development build, not a finished
feature** — both halves are now written, the menu side included, but none of it has run on a real
console yet — and the standard ELF is completely unaffected by it. See
**[docs/RETROACHIEVEMENTS.md](docs/RETROACHIEVEMENTS.md)** for what it does, which launch paths can
ever support it, and why.

Choose the current development build or a preserved snapshot:

| Channel | What it is |
| --- | --- |
| **[Rolling (Latest)](https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/rolling)** | Updated from `rebuild/main` by successful publishing runs. A full, non-pre-release GitHub release that remains a **development build**, with compatibility depending on the game and hardware. |
| **[Current Fan Favorite Build](https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/current-fan-favorite)** | A preserved development snapshot selected after positive user feedback. Use it as a fixed comparison point; it is not a universal compatibility guarantee. |

Download the **normal installable package**, `RIPTOPL-<rel>-<sha>.zip`. Separate archives serve
different purposes: `RA` for the experimental RetroAchievements loader, `VARIANTS` for alternate
configurations including DualSense, `DEBUG` for diagnostics, `LANGS` for translations, and `src`
for the exact source snapshot. Optional archives or SDK flavours can be omitted; read the release notes.
GitHub does not publish bare loader ELFs or separate checksum/SDK manifests in the normalized asset set.
The workflow can also publish `v*` tags, but no such stable release is currently offered here.


See **[ROLLING_RELEASE.md](ROLLING_RELEASE.md)** for exactly what the rolling release
contains and how to pull it.

> **Which rolling build?** The rolling zip ships four loader ELFs that differ only by build
> toolchain — the RiptOPL code in each is identical. Start with a pinned SDK for reproducible comparisons:
> 1. **`APP_RIPTOPL-PS2DEVPINNED/`** (`-PS2DEVPINNED`) — **recommended primary download.** Built on the
>    `ps2dev/ps2dev` SDK pinned by image digest for reproducible toolchain selection; pinning alone does not prove hardware compatibility.
> 2. **`APP_RIPTOPL-OFFICIALPINNED/`** (`-OFFICIALPINNED`) — **recommended official pin.** Built on the
>    `ps2homebrew/ps2homebrew` official SDK, pinned by image digest.
> 3. **`APP_RIPTOPL-PS2DEVROLLING/`** (`-PS2DEVROLLING`) — **bleeding-edge canary.** Tracks `ps2dev/ps2dev:latest`.
> 4. **`APP_RIPTOPL-OFFICIALROLLING/`** (`-OFFICIALROLLING`) — **bleeding-edge official canary.** Tracks `ps2homebrew:main`.
> See [Which build should I use?](ROLLING_RELEASE.md#which-build-should-i-use).

> Older published builds may be available in the [MEGA archive](#opl-archive).

## Quick Start

### What you need

- A PS2 with a homebrew ELF launcher, or a supported [backward-compatible PS3](#ps3-bc).
- One prepared storage source from the table below. Ethernet and a reachable server are required for network sources.
- The normal installable `RIPTOPL-<rel>-<sha>.zip` from [Releases](#releases).

### Minimal startup path

1. Extract the normal installable archive. Start with `APP_RIPTOPL-PS2DEVPINNED/RIPTOPL.ELF` when present; see [build choices](ROLLING_RELEASE.md#which-build-should-i-use) for alternatives.
2. Copy that ELF to a location your homebrew launcher can boot. For Neutrino, also copy the complete `neutrino/` folder to `mc0:/neutrino/` or `mc1:/neutrino/`. PS1 needs the companion files described in [PS1 games](#ps1-games-two-cores-one-list); copying the loader ELF alone does not install those cores.
3. Prepare your game's source: `CD/` or `DVD/` for folder-based PS2 libraries, HDLoader partitions for APA, or `games.csv` for HTTP. See [How to use](#how-to-use).
4. In **Settings → Game Sources**, enable the device and its start mode. For a network source, set **Network Start Mode** to **Manual** or **Auto**, then choose the protocol in **Network** and enter your server's settings.
5. Use the server's Test action where available, then choose **Save Changes** before launching a game. A successful menu test is not proof that gameplay works.
6. Launch one test game. If it fails, record the exact build, SDK flavour, source and core before changing settings; see [Frequent Issues](#frequent-issues).

### Known limitations

- **HTTP:** implemented and host-tested, but not yet tested on PS2 hardware. ISO only; no VMC, Neutrino or PS1. DVD9 probing is implemented but unproven. [HTTP guide](docs/HTTP.md).
- **RetroAchievements:** a separate development package; the console integration has not been hardware-tested. It requires xeRAbora on a PC and OPL-core launching. The RA build includes main-menu actions to check and launch a physical PS2 disc with achievements; this path also needs hardware validation. Keep games you play with achievements on a local device — upstream reports on hardware that a game with an achievement set stops loading about a minute in from a network share. [RA guide](docs/RETROACHIEVEMENTS.md).
- **iLink:** revision 2692 passed Ember on the tested SCPH-39001 but failed native OPL, Neutrino and POPSTARTER handoffs. The Neutrino `-qb` correction is implemented; a passing retest is still needed.
- **Core switching is source-dependent:** SMB and HTTP use OPL; UDPFS/UDPBD use Neutrino. See the [source table](#introduction).

## Introduction

Open PS2 Loader (OPL) is a 100% Open source game and application loader for
the PS2 and supported backward-compatible PS3 units (see [PS3 BC](#ps3-bc)). RiptOPL source is AFL-3.0; bundled third-party components retain their own licences.
Major capabilities include GSM video mode fixes, Virtual Memory Cards (VMC), PS2RD cheats, DS3/DS4 pad emulation, themes, and homebrew app launching.

RiptOPL supports local USB, MMCE, MX4SIO, iLink and internal ATA storage, plus
SMB, UDPFS, UDPBD and HTTP over Ethernet. Capabilities depend on both the source and loader core:

| PS2 game source | Loader core | Image/layout | VMC notes |
| --- | --- | --- | --- |
| USB / MX4SIO / MMCE / ATA exFAT | OPL or Neutrino for ISO | ISO; OPL also handles ZSO and UL split images | Supported; Neutrino cards must be on the game's device |
| iLink | OPL or Neutrino paths implemented | Local files | Launch remains pending hardware retest; see [iLink status](#usbmmcemx4sioilink) |
| Internal HDD, APA | OPL or Neutrino | HDLoader partitions | OPL supports VMC; Neutrino drops APA/PFS VMC with a warning |
| SMB | OPL | ISO, ZSO, UL | Server must permit writes for VMC saves |
| UDPFS Files / IMG and UDPBD | Neutrino | Files under `CD/` / `DVD/`, or a served FAT/exFAT disk image | Depends on backend and write access; see [Neutrino](docs/NEUTRINO.md) |
| HTTP | OPL | ISO listed in `games.csv`; compressed images refuse to launch | No VMC; use physical memory cards |

Neutrino does not read local ZSO or UL split images. A compatible UDPFS server can instead
decompress an image on the PC and expose it to the console as an ISO; that is server-side support.
ELF apps and PS1 games have their own launch paths, described below.

>[!NOTE]
OPL is developed continuously - anyone can contribute improvements to the project due to its open-source nature.

You can visit the Open PS2 Loader forum at:\
<https://www.psx-place.com/forums/open-ps2-loader-opl.77/>

You can report compatibility game problems at:\
<https://www.psx-place.com/threads/open-ps2-loader-game-bug-reports.19401/>

For historical upstream OPL compatibility reports (not a current RiptOPL compatibility guarantee), visit:\
<http://sx.sytes.net/oplcl/games.aspx>

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
  `favourites.bin` file** if it finds one. This is a **one-way import**: the next favorites write replaces that file with RiptOPL’s `OFAV` format, which those loaders cannot read back. Keep a copy of the original if you also use them.
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
- **Controller vibration in the menus (on by default):** toggle **Controller Vibration in Menus** in
  **Settings** for a little haptic tap as you move around — a light tick when the cursor moves,
  a slightly firmer bump on confirm / cancel / notifications, and one when OPL finishes booting
  and the menu is ready. Needs a **DualShock in analog mode** (a digital-only or clone pad simply
  won't buzz); DS3/DS4/DS5 pads are supported on builds with pad emulation. Left off, nothing
  changes.
> **Credit and licence — Neutrino is created by [rickgaiser](https://github.com/rickgaiser), and its
> official home is <https://github.com/rickgaiser/neutrino>.** Neutrino is an independent PS2 device
> emulator. Release packaging normally bundles the official
> latest build, re-fetched at publish time (download/extraction failures can omit it; check the release notes), under its **AFL-3.0** licence. We add exactly one file to
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
- **HTTP library (new):** point RiptOPL at a server meeting the [HTTP client response profile](docs/HTTP.md), and it reads a `games.csv` catalog and streams the ISOs straight off it
  through OPL’s own core. The PC side is **[Docmine17](https://github.com/Docmine17)’s**
  **[HTTP server](https://github.com/Docmine17/Open-PS2-Loader-HTTP)**, used **unmodified** — existing catalogs and folder layouts need no conversion. Pick **HTTP** under
  **Network → Protocol**, set the address, port (default **1100**) and base path, then use **Test HTTP
  server**. No VMC, no Neutrino and no PS1 over HTTP, and it has **not been hardware-tested yet**.
  See **[docs/HTTP.md](docs/HTTP.md)**.
- **UDPFS network boot (Neutrino):** a newer network transport (Neutrino's UDPRDMA) offered
  alongside UDPBD. The network controls are split across two pages: **Game Sources** holds the
  **Network Start Mode** row (Off / Manual / Auto), and **Network** holds **Protocol**
  (**SMB / UDPFS / UDPBD / HTTP**), **SMB Version** (SMBv1 / SMB2, live only while Protocol is SMB),
  and **Access** (Files / IMG — locked to Files
  for SMB/HTTP and to IMG for UDPBD, free only for UDPFS). UDPFS launches via `-bsd=udpfsbd` with a
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
  independent PS1 emulator written from scratch for the PS2. It is bundled unmodified under the **Ember Public
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
- **1080p GSM video mode:** forced progressive 1080p (1920×1080) GSM mode is built directly into all standard builds (`make GSM1080P=1`). Selecting 1080p in the per-game GSM picker is guarded by a **three-step confirmation**; if a game loses the picture, disable/change its GSM override before relaunching. **Triangle + Cross** at boot affects only the OPL menu and requires a 480p-capable display/connection.
- **Ready-to-use defaults:** a fresh install boots with sensible options already enabled —
  widescreen, cover art, notifications, sound effects + boot sound, delete/rename, and
  the PS2 logo. Video mode stays **Auto**. Every storage device ships **off**, so the first boot
  lands on the start menu with no tabs — enable exactly the devices your console has under
  **Game Sources**. Change any of it under Settings.
- **Private master settings, shared support files:** `settings_riptopl.cfg` is separate from stock OPL's master file. Other configuration and data can still be shared; favorites migration from uOPL/wOPL is one-way. See [Where your files live](#where-your-files-live).

## How to use

Folder-based PS2 libraries use the following support folders under their configured library prefix.
APA uses its selected PFS data partition. HTTP uses local support folders beside the active settings
home; it does not open these folders on the server. Availability also depends on the core:

| Folder | Description                                          | Modes       |
| ------ | ---------------------------------------------------- | ----------- |
| `CD`   | for games on CD media - i.e. blue-bottom discs       | All folder devices¹ |
| `DVD`  | for DVD5 and DVD9 images (if filesystem supports +4gb files) | All folder devices¹ |
| `VMC`  | Virtual Memory Card images (headline save feature): stored in `VMC/`, typically 8MB to 64MB, then assigned per game via **Game Settings** | See source/core table |
| `CFG`  | for saving per-game configuration files              | See source/core table |
| `ART`  | for game art images                                  | See source/core table |
| `THM`  | for themes support                                   | See source/core table |
| `LNG`  | for translation support                              | See source/core table |
| `CHT`  | for cheats files                                     | See source/core table |
| `APPS`  | for ELF files                                       | See source/core table |

¹ **Folder-based devices** — USB, MMCE, MX4SIO, iLink, SMB, and the **exFAT** (BDM) HDD — keep games as files in the `CD`/`DVD` folders. The **APA/PFS** HDD instead stores games as HDLoader partitions (no `CD`/`DVD` folders), while still using `CFG`/`ART`/`VMC`/`THM`/`CHT`/`LNG`/`APPS` on the configured OPL data partition (`+OPL` root by default, or `__common/OPL/` for the legacy layout).

Per-game settings are stored per title in the `CFG` context. Typical use cases include compatibility toggles, video options (GSM), cheat toggles, and assigning a VMC file from the `VMC` folder to that game.

RiptOPL attempts to create support folders when you enable a writable source. Read-only sources need preparation on the server or PC; enabling a device does not format a disk or create APA partitions.

For APA, RiptOPL uses existing partitions only: a usable `hdd_partition` selection in
`hdd0:__common/OPL/conf_hdd.cfg`, then an existing `+OPL`, then `__common/OPL/`.
If no suitable data partition exists it fails without creating one. See [HDD](#hdd).

HDDs are also able to be formatted as exFAT to avoid the 2TB limitation.  Please see below in the `HDD` section for more details on this configuration.

### Where your files live

`settings_riptopl.cfg` holds RiptOPL's master settings; an older `conf_riptopl.cfg` is imported on
read and migrated on save. The normal settings home starts with the loader's boot directory and
uses discovery/fallbacks when that location cannot be used. **Custom Settings Path** can select a
different home. Check the active location instead of assuming all settings are on `mc0:/OPL/`.

Network configuration (`conf_network.cfg`), app lists, per-game settings, artwork, themes and VMCs
can still be shared with other OPL installations. Separate master filenames do not isolate these
files. Favorites import from uOPL/wOPL is one-way: saving writes RiptOPL's own `OFAV` format.

PS2 folders follow the source's configured library prefix. Local PS1 `POPS/` and `EMBER/` live at
the **device root**, even if PS2 games use a subfolder. APA has its own partition layouts, covered
in [PS1/VCD](docs/VCD.md). HTTP's `CFG/`, `ART/` and `CHT/` are local folders under the active settings
home; placing them on the HTTP server does not make the console load them from there.

## USB/MMCE/MX4SIO/iLink

Supported file systems:
FAT32 and exFAT are the usual folder-based layouts. MBR is the conventional setup for USB and SD media; partition-table support also depends on the device/driver. For large internal disks, see [HDD](#hdd) for GPT/exFAT. This section applies to MMCE and MX4SIO SD setups, USB storage, and iLink SBP2 storage.

> [!WARNING]
> **Revision 2692 iLink status is not a blanket pass.** On an SCPH-39001, Ember passed from the same
> IEEE 1394 disk in all four SDK flavours, while the native OPL core, Neutrino, and POPSTARTER failed
> at their reset/handoff boundaries. The current code corrects a confirmed RiptOPL-side Neutrino
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

> Fragmentation has a bounded budget. OPL’s BDM reader shares a 64-entry fragment table across the game image parts; Neutrino’s block-device budget includes the ISO and VMCs together. Contiguous copies leave the most headroom.

If you choose to use the FAT32 file system, games larger than 4gb must use USBExtreme format (see OPLUtil or USBUtil programs).

If a fragment-limit error is reported, first try a fresh sequential copy with enough free space.
Back up all data before considering a reformat; a screen colour alone does not diagnose fragmentation.

## SMB

For loading games by SMB protocol, you need to share a folder (ex: PS2SMB)
on the host machine or NAS device and grant read access for game loading. Write access is also needed for VMC saves and changes to files stored on the share. USB Advance/Extreme format is optional - \*.ISO images
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
> IP, port, share and credentials to the values the launcher displays. Its documented custom SMBv1
> port is **1111**, matching RiptOPL’s default. Windows’ built-in server or a NAS may use a different
> port; enter the value that server actually listens on. Release packages
> include **`PS2-Servers.url`** as a direct shortcut to the repository.

## HDD
	
Both PS2 HDD types are **off by default** in RiptOPL — enable the one you use under **Game
Sources**. For PS2, 48-bit LBA internal HDDs are supported. The HDD can be formatted as:

- APA partitioning with PFS filesystem (up to 2TB)
	- RiptOPL mounts an existing data partition. A usable `hdd_partition` selection in `__common/OPL/conf_hdd.cfg` takes priority, followed by existing `+OPL`, then `__common/OPL/`. No partition is created, resized or formatted. `+OPL` uses its root for support folders; `__common` uses `OPL/`.
- MBR partitioning (up to 2TB) or GPT partitioning (capacities above the MBR limit, subject to the driver and tested hardware) with the exFAT filesystem
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
3. Add the device prefix and ELF path (for example `mass:` for USB/MX4SIO/iLink/exFAT-HDD, `mmce0:` for MMCE, `mc0:` for the Memory Card, or `hdd0:`/`pfs0:` for the APA HDD), then the file path to the ELF.

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

RiptOPL checks `mc?:OPL/conf_apps.cfg` first, then `conf_apps.cfg` under each enabled source’s configured prefix; the first file found wins. With a default mass-device prefix, use `mass0:/conf_apps.cfg`. `mass0:/OPL/conf_apps.cfg` is only found through this route when `OPL/` is that source’s configured prefix. Files are not merged into one legacy list.

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

OPL accepts `.cht` files in PS2RD format. Name them after the game’s startup ID, for example `CHT/SLUS_123.45.cht`. An uncompressed `CHT/cht.tar` containing `<startup>.cht` members is also supported, with loose files used as a fallback. Each cheat file corresponds to a specific game and must be stored in the `CHT` directory on your device.
Cheats are structured as hexadecimal codes, with proper headers as descriptions to identify their function.
You can activate cheats via OPL's graphical interface. Navigate to a games settings, enable cheats and select the desired mode.

### Cheat Modes

  * Auto Select Cheats:  
This mode will enable and apply all cheat codes in your `.cht` file to your game automatically.

  * Select Game Cheats:  
When enabled a cheat selection menu will appear when you launch a game. You can navigate the menu and disable undesired cheats for this launch session. Master Codes cannot be disabled as they are required for any other cheats to be applied.

## NBD Server

The built-in NBD server **exports the PS2's internal drive to a PC** for tools such as `hdl-dump`
or `pfs-shell`. This is different from UDPBD/UDPFS, which serve game data from a PC to the PS2.
Configure the console's IP (static or DHCP), choose **Start NBD server**, and connect only after
the console reports that it is running. Disconnect the PC client before leaving the server screen.

See the [NBD client setup guide](https://nathanneurotic.github.io/Open-PS2-Loader/nbd.html) for
Linux, WSL and Windows examples. The implementation uses [lwNBD](https://github.com/bignaux/lwNBD)
and the [NBD protocol](https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md).

## ZSO Format

As of version 1.2.0, compressed ISO files in ZSO format is supported by OPL.

To handle ZSO files, a python script (ziso.py) is included in the pc folder of this repository.
It requires Python 3 and the LZ4 library:

  ```sh
pip install lz4
```

To compress an ISO file to ZSO:

  ```sh
python pc/ziso.py -c 2 "input.iso" "output.zso"
```

To decompress a ZSO back to the original ISO:

```sh
python pc/ziso.py -c 0 "input.zso" "output.iso"
```

For OPL-core folder-based sources, copy ZSO files beside your ISOs. Local ZSO is not a Neutrino format, and HTTP refuses compressed-image launches; see the [source table](#introduction).
To install onto internal HDD, you can use the latest version of HDL-Dump.

## PS3 BC

Currently, supported only [PS3 Backward Compatible](https://www.psdevwiki.com/ps3/PS2_Compatibility#PS2-Compatibility) (BC) versions. So only [COK-001](https://www.psdevwiki.com/ps3/COK-00x#COK-001) and [COK-002/COK-002W](https://www.psdevwiki.com/ps3/COK-00x#COK-002) boards are supported. USB, SMB, HDD modes are supported.

To run OPL, you need an entry point for running PS2 titles. You can use everything (Swapmagic PS2, for example), but custom firmware with the latest Cobra is preferred. Note: only CFW supports HDD mode.

## Some notes for DEVS

Repository scripts live in `tools/`; run them from the repository root. Existing
Makefile targets retain their names and output locations.

Use one of the toolchain images in [the CI matrix](.github/workflows/flavours.yml). Both pinned
and rolling SDK images are supported; the latest SDK is not required. For a reproducible baseline,
use the `PS2DEVPINNED` image digest from that matrix. From the repository root with Docker available:

```sh
docker run --rm -v "${PWD}:/src" -w /src ps2dev/ps2dev@sha256:8fba50ecc2229acd7f8da63d34302f12939b7d4fa6848dda1e6a0ce083321a11 make -j4
```

Use a clean checkout/build directory when changing SDK images. See the Makefile for flags such as
`DUALSENSE=1` and `EXTRA_FEATURES=1`; build success does not establish console compatibility.

## OPL Archive

Successful archive uploads preserve rolling builds on MEGA under run-specific folders (`RiptOPL/Rolling/<version>/run_<number>/`). Uploads depend on publishing success and configured credentials; superseded or failed runs may have no archive. The archive includes installable packages, source and build metadata; VARIANTS and DEBUG bundles are excluded. You can access it by clicking the MEGA badge at the top of this readme or visiting the [MEGA Rolling Archive](https://mega.nz/folder/74pRHKRB#9SLDkrkvZAbeKO4Qvxg9LQ).

## Frequent Issues

### OPL freezes on logo or grey screen

Hold **START** while RiptOPL initializes to skip saved configuration. If that restores the menu,
review the active settings location, configure the required source, save, and retry a normal boot.
This isolates configuration as a possibility; it does not diagnose every startup freeze.

### Game freezes on white screen

A white or black launch screen alone does not identify the cause. Record the exact build/SDK,
console, game ID, source and core. Check the image against a known-good dump and test with game
overrides disabled. If the loader reports a fragment-limit error, try a fresh sequential copy.
Back up the device before any reformat; do not reformat solely because of the screen colour.

### OPL does not display anything on boot

Hold **Triangle + Cross** while RiptOPL initializes to force **480p for the OPL menu**. Your display
and connection must accept 480p. Once visible, choose a suitable menu mode under **Interface** and
save. **START** skips saved configuration and uses defaults if you need to undo a saved menu setup.

### The menu works, but launching a game loses the picture

Menu recovery does not clear game video settings. Open that game’s settings and disable/change its
GSM override, or its Neutrino video options if that is the selected core. Check inherited global
video defaults too. Retry with the game's normal video mode before adding overrides again.

Report RiptOPL regressions in [this repository’s issue tracker](https://github.com/NathanNeurotic/Open-PS2-Loader/issues).
Include the version/SHA, SDK flavour, console model, game ID, storage/protocol, loader core,
reproduction steps, relevant settings and any known-good build comparison. For upstream/community
discussion, the [OPL forum](https://www.psx-place.com/forums/open-ps2-loader-opl.77/) remains available.

## External Tools & Services

RiptOPL is intended to work with these maintained companion tools:

- **[PS2-Servers](https://github.com/NathanNeurotic/PS2-Servers)** by **[Ripto](https://github.com/NathanNeurotic)** — all-in-one PC server launcher for **SMB, UDPFS, UDPBD and HTTP**.
- **[udpfs-server](https://github.com/YouKnow-sys/udpfs-server)** by **[YouKnow-sys](https://github.com/YouKnow-sys)** — the same idea **from a phone**: an Android app that shares folders and disk images to the PS2 over **UDPFS**, found by broadcast so there is no server address to type in on the console. Works over a router or a direct cable. Built on **[udpfsd](https://github.com/pcm720/udpfsd)** by **[pcm720](https://github.com/pcm720)**; MIT licensed. A `udpfs-server.url` shortcut ships in installable packages.
- **[OrbitPS2 Manager](https://github.com/Luden02/OrbitPS2-Manager)** by **[Luden](https://github.com/Luden02)** — cross-platform PC library manager for importing discs, artwork/screenshots, ZSO compression, per-game settings and VMC management.
- **[OPL PS1 AIO Converter GUI](https://github.com/shaanhomebrew-cloud/OPL-PS1-AIO-Converter-GUI)** by **[shaan](https://github.com/shaanhomebrew-cloud)** — Windows all-in-one PS1/POPStarter preparation tool for converting BIN/CUE backups to VCDs and installing them to USB, MX4SIO, MMCE, iLink, exFAT HDD, SMB and APA internal HDD.
- **[xeRAbora](https://github.com/hacan359/xerabora)** by **[hacan359](https://github.com/hacan359)** — the PC client for **RetroAchievements** on real PS2 hardware. RiptOPL's RA build streams the running game's memory to it; xeRAbora runs rcheevos, talks to the RetroAchievements servers and unlocks the achievements. It also builds the per-game watch list the console needs. Shipped as a shortcut inside `RIPTOPL-RA-*.zip`; MIT licensed. RiptOPL’s RA integration is not yet hardware-tested.
- **[OPL HTTP PC server](https://github.com/Docmine17/Open-PS2-Loader-HTTP)** by **[Docmine17](https://github.com/Docmine17)** — the PC side of RiptOPL’s **HTTP** protocol: a small static HTTP server with byte-range support that serves your `games.csv` catalog and streams the ISOs themselves. RiptOPL works with it **unmodified** — no new API, no catalog conversion, no changed folder layout — with host conformance checks against the upstream server. PS2 hardware validation is still pending.
- **[PS2RD CHT Manager](https://github.com/TheRealNextria/PS2RD-CHT-Manager)** by **[TheRealNextria](https://github.com/TheRealNextria)** — PC manager for the PS2RD `.cht` cheat files RiptOPL reads from your device's `CHT` folder. A `PS2RD-CHT-Manager.url` shortcut ships in installable packages.
- **[Ember](https://github.com/Gageformer/Ember)** by **[Gageformer](https://github.com/Gageformer)** — a PS1 emulator that runs natively on the PS2, used as RiptOPL's **second PS1 core** alongside POPSTARTER. Unlike the others this one is not just a shortcut: an `EMBER/` folder ships **inside** the release package, ready to drop onto a device. It is bundled unmodified under the Ember Public Beta Testing Licence (`EMBER/LICENSE-BETA.txt` in the package); releases: <https://github.com/Gageformer/Ember/releases>.
- **[POPStarter](https://www.psx-place.com/resources/popstarter.683/)** by **krHACKen** — a PS1 launcher built around Sony's native **POPS** emulator for the PS2, used as RiptOPL's **primary PS1 core** alongside Ember. POPStarter provides the compatibility and launch layer for running PS1 VCDs from USB, MX4SIO, MMCE, iLink, internal HDD, and SMB; RiptOPL's iLink handoff is wired but still awaiting a passing hardware retest. The official POPStarter r13 package contains **no Sony emulator binaries, libraries, or BIOS files**; those components must be supplied separately by the user. Official download, documentation, compatibility information, and releases are maintained on **[PSX-Place](https://www.psx-place.com/resources/popstarter.683/)**.
- **[Neutrino](https://github.com/rickgaiser/neutrino)** by **[rickgaiser](https://github.com/rickgaiser)** — a *"Small, Fast and Modular PS2 Device Emulator"*, and RiptOPL's **second PS2 loader core** alongside OPL's own. Like Ember it is not a shortcut: a ready-to-use `neutrino/` folder normally ships **inside** the installable package (check release notes for download/extraction omissions), drag-and-drop to `mc?:/neutrino/`. Neutrino is deliberately **UI-agnostic** — it has no interface of its own, which is exactly what lets a front-end like RiptOPL drive it per game. Licensed **AFL-3.0**; releases: <https://github.com/rickgaiser/neutrino/releases>.
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
[sOPL](https://github.com/mystyq/Stable-Open-PS2-Loader), uOPL,
[wOPL](https://github.com/KrahJohlito/wOPL), [OPL DB](https://github.com/Jay-Jay-OPL/OPL-Daily-Builds),
[POPSLoader](https://github.com/NathanNeurotic/POPSLoader),
[OPL RetroGEM ID by CosmicScale](https://github.com/CosmicScale/Open-PS2-Loader-Retro-GEM),
[nhddl](https://github.com/pcm720/nhddl),
[hacan359's RetroAchievements OPL](https://github.com/hacan359/Open-PS2-Loader/pull/1),
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
  past work on an independent **uOPL** fork that kept
  unique features and unmerged work alive. Thank you.
- **bbsan2k** — for the **MMCE (Memory Card Mass Storage) protocol** that makes SD-via-memory-card
  loading through the PS2's memory-card slot possible. OPL's MMCE support builds directly on it.
- **hacan359 (yoba)** — for **RetroAchievements on real PS2 hardware** and the
  **[xeRAbora PC client](https://github.com/hacan359/xerabora)** (MIT). The console identifies the
  game and streams the memory addresses its achievements watch; xeRAbora handles rcheevos, the
  RetroAchievements service, and login. RiptOPL implements the published wire protocol in its
  `RIPTOPL-RA-*.zip` build, which includes a shortcut to xeRAbora.
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

Enormous thanks to the testers who run rolling builds on real consoles and file the
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
