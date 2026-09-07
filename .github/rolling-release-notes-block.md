<!--
  REUSABLE ROLLING-RELEASE NOTES BLOCK — read by .github/workflows/rolling-release.yml
  ("Build release notes" step cats this file into notes.md verbatim, right after the
  version/commit/built/run header, on every rolling publish). Edit it here, once; every
  rolling release then carries it without being re-drafted. Do NOT paste a copy into an
  individual release body — that fork would drift from this one.

  This block is the new-lineage statement. The transition beats (parity, old-build
  pointer, re-reporting rules) stay until parity with the pre-rebuild build is declared;
  when that happens, remove those beats AND the cat that reads this file. The PS1, Ember
  and cover-art sections are evergreen -- keep them (here or moved back into the workflow)
  even after the transition beats go.

  ONE HOME PER FACT. Anything covered here is deliberately NOT repeated in the workflow's
  generated "In this build" list, which is a scannable index pointing at these sections.
  If you add a fact, put it in exactly one of the two.

  Keep the voice: warm, direct, no corporate padding. Plain Markdown — no liquid/templating.
-->

**Read this first.**

- **This is a new lineage.** RiptOPL was rebuilt from official OPL after the previous line
  engineered itself into a corner. It is **not yet at parity** with the build it replaces, and
  reaching that comes before new features.
- **This build can be ahead of what has been verified on real hardware.** That is what a rolling
  channel is for — it exists so testing can happen.
- **The old build is still available and still fine to use.** The final release of that lineage is
  on the releases page as *Rolling Alpha*, with the archive on MEGA. If you are happy on it, stay
  on it — it is not going anywhere, and it is still the most trustworthy RiptOPL today.
- **Bug reports are tracked against this lineage only.** Any report is assumed to refer to it
  unless you say otherwise. Reports filed during the transition need re-verification against this
  build.
- **Re-report anything you have seen fixed before.** Regressions are possible and every setup is
  unique. A duplicate report costs nothing; a silent one costs everyone.
- **Reports are what move this.** The last two weeks of fixes came almost entirely from tester
  reports, not from reading code.

## Library navigation and Favorites

- The shared **PS2/PS1 Game Display** picker now appears on both **Interface** and
  **PS Emulation Settings**. **Both (L3)** switches separate PS2/PS1 device views. **Mixed**
  combines them and L3 cycles Mixed → PS2 → PS1. **PS2** and **PS1** lock one library and make
  L3 completely inert: no hint, sound, notification, pause, or state change.
- APPS has an independent display picker: **Mixed** leaves every ELF in one inert list, while
  **Apps / PS1ELF (L3)** puts titles containing `[PS1]` (case-insensitive) on a second L3 view.
  They remain ordinary ELF launches.
- The independent Favorites page always has four L3 stops:
  **All in One → PS2 → PS1 → ELF**. Actions and launches in the mixed view follow the selected
  row's actual type.
- Returning from Start or Settings retains the device page you paused on when it is still visible;
  it no longer falls back to the first USB page.
- User-facing text now consistently uses the American spelling **Favorite / Favorites**. The shared
  compatibility filename remains exactly **`favourites.bin`**.

## Playing PS1 games — two cores, one list

PS1 titles run through **two** cores. On ordinary devices both appear together in one **PS1**
library. **PS2/PS1 Game Display** controls whether that device uses separate L3-switched libraries,
a combined Mixed list with L3 filters, or one locked library.

| Core | Format | Lives in |
| --- | --- | --- |
| **POPSTARTER** | `*.VCD` | `<device>:/POPS/` |
| **Ember** | `*.cue` / `*.bin` / `*.exe` | `<device>:/EMBER/games/<Game Name>/` |

They are interleaved and sorted as one library, so a game you keep for both cores appears twice.
Which core runs a title is decided by that row when you launch it — nothing to configure, no extra
toggle.

**UDPFS and UDPBD now expose PS1 pages too, but list Ember titles only.** Ember inherits the live
network mount. POPSTARTER resets the IOP and cannot restore either network transport, so VCD rows
are deliberately excluded instead of showing titles that cannot launch.

**iLink PS1 status is split, not a blanket pass.** Ember passes from iLink. RiptOPL now prepares the
POPSTARTER iLink handoff too: the package's `POPS/` folder contains the two loose source files,
`usbd.irx.ilink` and `usbhdfsd.irx.ilink`; RiptOPL copies them to `mc?:/POPSTARTER/` as `usbd.irx`
and `usbhdfsd.irx` (removing only `.ilink`) and passes the normal local-device selector
`mass:/POPS/XX.<game>.ELF`. However, revision 2692 hardware testing still returned POPSTARTER to
wLaunchELF on iLink in every SDK flavour. The external equip/handoff is present, but it is **not yet
a hardware-confirmed POPSTARTER iLink success**.

All BDMA pairs stay as separate files in `POPS/`; none is embedded in `RIPTOPL.ELF`. Keep the
package's `POPS/` contents together when copying it to a device.

Neutrino's **Default Device** picker also has an explicit **iLink** option for a complete
`neutrino/` folder stored on IEEE 1394 media. Existing saved picker values keep their meanings;
iLink was appended as a new choice. Revision 2692 already emitted Neutrino's documented
`-bsd=ilink` backend, but its inherited-stack handoff had a concrete mismatch: RiptOPL preserved the
mounted iLink stack into Neutrino without sending `-qb`, leaving Neutrino free to reset that stack
away. This build auto-emits `-qb` for iLink, matching Neutrino's quick-boot contract and NHDDL's
iLink handoff. The four-flavour failure pattern makes that a strong candidate, not a proven complete
cause; the correction still needs a new hardware retest before it is called a pass.

The native OPL core also remains **unconfirmed on iLink**: all four revision 2692 flavours failed at
the game handoff (browser return or black screen; one test powered the drive down). Because Ember
passes and both SDK lineages fail, this does not look compiler-specific or like a menu-side mount
failure. No regular iLink IOP module was replaced or repackaged in this follow-up; the native reset
path remains under investigation.

**Setting Ember up.** Copy this package's `EMBER/` folder to the **root** of a device, beside
`POPS/`. Then add **your own PS1 BIOS** as `EMBER/bios.bin`, exactly 512 KB — a BIOS is copyrighted
and is never distributed with RiptOPL or Ember, so the folder ships with a placeholder file whose
name reminds you. Give each game its own folder:

```
<device>:/EMBER/
    ember.elf
    bios.bin                       <- yours, 512 KB, not included
    settings.txt                   <- optional, written only if you set a display mode
    games/
        Spyro 2 (Ripto's Rage)/
            whatever.cue
            whatever.bin
```

The **folder name** is what you see in the list, and it is the key for cover art and per-game
settings — so `ART/Spyro 2 (Ripto's Rage)_COV.png` on that device just works, exactly as for a
`.VCD`. Files inside can be named anything; `.cue` is preferred over `.exe` over `.bin`. Ember
writes its per-game memory cards (`MC1.vmc`, `MC2.vmc`) into that game folder, so it must be on
writable media. Available on USB, MX4SIO, iLink, exFAT-ATA, MMCE, SMB, the internal APA/PFS hard
drive, UDPFS and UDPBD.

**On an internal APA/PFS drive** there is no filesystem root to copy to, so the `EMBER/` folder goes
on a partition of its own — **`__.EMBER`**, or `__.EMBER0` … `__.EMBER9` for more than one. Same
naming shape as the `__.POPS` containers that hold your `.VCD` files.

The partition is self-contained: `EMBER/ember.elf`, `EMBER/bios.bin` and `EMBER/games/<Game Name>/`
all start at its root. Cover art does **not** go here — `ART/<Game Name>_COV.png` stays on your OPL
data partition, exactly as for a `.VCD`.

An APA title launches with its partition **still mounted read/write**: Ember inherits that live mount
rather than re-opening the drive, and writes its memory cards and `settings.txt` straight through
it.

**Display mode.** *Settings → PS Emulation Settings → Ember Display Mode* (Default / 240p / 480p).
Ember's `settings.txt` is optional and stays that way: **240p** or **480p** writes the key to
`<device>:/EMBER/settings.txt` on launch, on that device only, creating the file if needed.
**Default** removes the key again — and the file too, if that key was all it held. Any other lines
in the file are preserved throughout. Ember's own default is 480p, so *Default* and no file mean the
same thing.

Ordinary device pages merge Ember and POPSTARTER rows. The **UDPFS** and **UDPBD** PS1 pages list
Ember only because POPSTARTER cannot restore either network transport after its IOP reset.

Confirming **Network Settings** now applies the edited values and starts a fresh connection attempt
immediately. If a router or server was slow and the network page failed, **Select / Refresh** retries
the connection directly; reopening settings or saving the whole configuration is no longer required.

### Ember credit and licence

**Ember is created by Gageformer.** Release page: <https://github.com/Gageformer/Ember/releases>

Ember is an independent PS1 emulator written from scratch for the PS2, bundled unmodified under the
**Ember Public Beta Testing Licence** — shipped as **`EMBER/LICENSE-BETA.txt`**, which governs the
build it accompanies. In short: free to use and to bundle non-commercially; no selling, no modifying
or repackaging the build; and no BIOS or game content is included, ever.

Please report Ember problems to *us* first rather than to Gageformer — the launching is ours, and
most issues turn out to be on our side.

## The Neutrino core

The package ships a ready-to-use **`neutrino/`** folder — drag it onto your memory card as
`mc?:/neutrino/` and per-game Neutrino launching works with nothing further to set up. It is the
official latest build, re-fetched from upstream every time a release is published, so it does not go
stale behind us.

RiptOPL hands a game to Neutrino **per game**, not globally: everything else stays on OPL's own
core. That is the whole point of having two — if a title dislikes one, switch that one title.

### Neutrino credit and licence

**Neutrino is created by rickgaiser.** Home: <https://github.com/rickgaiser/neutrino>

Neutrino is an independent *"Small, Fast and Modular PS2 Device Emulator"*, bundled under its
**AFL-3.0** licence.

One honest note on "unmodified": we add exactly **one** file to the folder,
`config/bsd-udpfsbd.toml`. Neutrino ships `udpfs_bd.irx` but no matching `-bsd` token for it, and
RiptOPL launches UDPFS as `-bsd=udpfsbd`. Nothing of rickgaiser's is altered or removed.

Neutrino is deliberately **UI-agnostic** — it has no interface of its own, which is precisely what
lets a front-end drive it. RiptOPL is one of several: **NHDDL**, **XEB+ Plugin**, **RETROLauncher**,
**OSD-XMB** and **PSBBN/BBNL** are others, and they are worth a look if you want a different shape
of launcher over the same core.

Please report *launching* problems to **us** — building the arguments and handing off are ours, and
most reports land on our side of that line. Genuine Neutrino bugs belong upstream.

## Alternate POPSTARTER builds

`POPS/POPSTARTER VERSIONS/` holds five builds. To switch, copy the `POPSTARTER.ELF` you want over
`POPS/POPSTARTER.ELF` on your device. RiptOPL never selects one for you — the folder is there so you
do not have to go hunting for them.

| Build | Use it when |
| --- | --- |
| **DEBUG** | **The shipped default.** SMB-capable, and prints diagnostics on screen. |
| **MAIN** | POPSTARTER without the SMB support the default carries. Only if you know you do not need SMB. |
| **USBDELAY** | USB devices that need longer to settle before POPSTARTER reads them. |
| **USBDELAY_DEBUG** | USBDELAY, with the diagnostics. |
| **USBDELAY_LONGER_DEBUG** | A longer delay still, with diagnostics. |

The default being **DEBUG** is deliberate — it is the build carrying SMB support. POPSTARTER
diagnostics during a VCD launch are expected, not a fault. Do not copy **MAIN** over it unless you
are certain you do not need SMB; the alternates are here mainly for the `USBDELAY` cases.

**The redundant `POPSTARTER/` folder is gone from this package.** It held a second copy of nine
files `POPS/` already contains, plus a `bdma_config.txt` reading `fat32` — a marker RiptOPL *writes
itself* to record which BDMAssault variant it installed, so shipping it pre-filled told a fresh
install that a variant was equipped when it was not. Everything you need is in `POPS/`, including
the separate `.ilink` pair used for iLink POPSTARTER handoff preparation (hardware retest pending).

## Cover art and game metadata

Most "looks wrong on my setup" reports come down to files not being named the way the loader looks
for them. **OrbitPS2 Manager**, by **[Luden](https://github.com/Luden02)**, writes the `ART` folder and per-game configs in exactly the layout
RiptOPL expects — open **`OrbitPS2-Manager.url`** from the package, or visit
<https://github.com/Luden02/OrbitPS2-Manager>. It is a cross-platform (Windows / macOS / Linux)
desktop app that also imports games from disc images, fetches cover art and screenshots, compresses
ISOs to ZSO, edits per-game settings and manages virtual memory cards.

It is third-party and independently maintained — **please report OrbitPS2 Manager issues to its own
repository, not here.**

<!-- The "which flavour to download" guidance deliberately does NOT live here: it belongs
     to the "## Get started" section below (in the workflow), which lists all four
     flavours. It used to sit at this block's tail, stranded above the detailed list --
     one home, one copy. -->
