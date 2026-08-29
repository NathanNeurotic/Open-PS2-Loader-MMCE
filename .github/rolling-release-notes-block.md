<!--
  REUSABLE ROLLING-RELEASE NOTES BLOCK — read by .github/workflows/rolling-release.yml
  ("Build release notes" step cats this file into notes.md verbatim, right after the
  version/commit/built/run header, on every rolling publish). Edit it here, once; every
  rolling release then carries it without being re-drafted. Do NOT paste a copy into an
  individual release body — that fork would drift from this one.

  This block is the new-lineage statement. The transition beats (parity, old-build
  pointer, re-reporting rules) stay until parity with the pre-rebuild build is declared;
  when that happens, remove those beats AND the cat that reads this file. The "Setting
  up cover art and game metadata" section is evergreen -- keep it (here or moved back
  into the workflow) even after the transition beats go.
  Keep the voice: warm, direct, no corporate padding. Plain Markdown — no liquid/templating.
-->

**Before anything else — read this.**

This build is a **new lineage**. RiptOPL was rebuilt from official OPL after the previous
line engineered itself into a corner. It is **not yet at parity** with the build it
replaces — reaching at least the performance of the abandoned build comes before anything
else. New features come after parity, not before.

**This rolling build can be ahead of what has been verified on real hardware** — that is
what a rolling channel is for: it exists so testing can happen. If you want the most
trustworthy RiptOPL today, that is still the old build below.

**The old build is still available and still fine to use.** The final release of that
lineage is on the releases page (*Rolling Alpha*) for exactly this reason, and the archive
is on MEGA. If you are happy on it, stay on it — it is not going anywhere.

**Bug reports are now tracked only against this new lineage.** I will assume any report
refers to it unless you tell me otherwise. Reports filed during the transition are treated
as still-open and need re-verification against this build.

**Re-report anything you have seen fixed before.** Regressions are possible and every
setup is unique — an unreported one can sit unfixed indefinitely. A duplicate report costs
nothing; a silent one costs everyone.

**Every report is genuinely valued.** This does not improve without them: the last two
weeks of fixes came almost entirely from tester reports, not from reading code.

## Playing PS1 games — two cores, one list

RiptOPL plays PS1 titles through **two** cores, and both show up together in a single **PS1**
list. Press **L3** on a device page to swap between your PS2 discs and that PS1 list.

| Core | Format | Lives in |
| --- | --- | --- |
| **POPSTARTER** | `*.VCD` | `<device>:/POPS/` |
| **Ember** | `*.cue` / `*.bin` / `*.exe` | `<device>:/EMBER/games/<Game Name>/` |

They are interleaved and sorted as one library, so a game you keep for both cores simply appears
twice. Which core runs a title is decided by that row when you launch it — there is nothing to
configure and no extra toggle.

**Setting up Ember.** This package contains a ready-made **`EMBER/`** folder. Copy it to the root of
a device, right beside `POPS/`. Then add **your own PS1 BIOS** as `EMBER/bios.bin` — it must be
exactly 512 KB. A BIOS is copyrighted and is never distributed with RiptOPL, so the folder ships with
a placeholder file whose name reminds you. Give each game its own folder:

```
<device>:/EMBER/
    ember.elf
    bios.bin                       <- yours, 512 KB, not included
    games/
        Spyro 2 (Ripto's Rage)/
            whatever.cue
            whatever.bin
```

The **folder name** is what you see in the list, and it is also the key for cover art and per-game
settings — so `ART/Spyro 2 (Ripto's Rage)_COV.png` on that device just works, exactly as it does for
a `.VCD`. Files inside can be named anything; `.cue` is preferred over `.exe` over `.bin`. Ember
writes its per-game memory cards (`MC1.vmc`, `MC2.vmc`) into that game folder, so it has to be on
writable media.

Available on USB, MX4SIO, iLink, exFAT-ATA and MMCE. SMB and the APA internal HDD are
POPSTARTER-only for now.

**Ember display mode.** Ember can run at 240p or 480p, and there is a setting for it on the
**PS Emulation Settings** page. Ember's `settings.txt` is optional and stays that way: pick
240p or 480p and RiptOPL writes that choice to `<device>:/EMBER/settings.txt` when you launch
a game — creating the file if needed, on that device only. Go back to *Default* and the
setting is removed again, along with the file if that was all it contained. Any other lines
in that file are left alone, and *Default* never creates one.

### Ember credit and licence

**Ember is created by Gageformer.** Official release page: <https://github.com/Gageformer/Ember/releases>

Ember is an independent PS1 emulator written from scratch for the PS2 — it is **not** part of
RiptOPL and is not our work. It is bundled here unmodified, with the author's blessing, under
its **Ember Public Beta Testing Licence**. That licence ships in the package as
**`EMBER/LICENSE-BETA.txt`** and governs the Ember build it accompanies — please read it. In
short: free to use and to bundle non-commercially, no selling, no modifying or repackaging the
build, and no BIOS or game content is included or ever will be.

Please report Ember problems to *us* first, not to Gageformer: the launching is ours, and most
issues turn out to be on our side.

## Alternate POPSTARTER builds

`POPS/` now carries a **`POPSTARTER VERSIONS/`** folder holding five builds of POPSTARTER:

| Build | Use it when |
| --- | --- |
| **MAIN** | POPSTARTER without the SMB support the shipped default carries. Only if you know you do not need SMB. |
| **DEBUG** | **The shipped default.** SMB-capable, and prints diagnostics on screen. |
| **USBDELAY** | USB devices that need longer to settle before POPSTARTER reads them. |
| **USBDELAY_DEBUG** | USBDELAY, with the diagnostics. |
| **USBDELAY_LONGER_DEBUG** | A longer delay still, with diagnostics. |

To switch, copy the `POPSTARTER.ELF` you want over `POPS/POPSTARTER.ELF` on your device.
Nothing in RiptOPL selects these for you — it is a manual swap, and the folder is there so
you do not have to go hunting for the builds.

**The `POPSTARTER.ELF` shipped at `POPS/POPSTARTER.ELF` is the DEBUG build on purpose** — that
is the build with SMB support, so it is the right default. Seeing POPSTARTER diagnostics
during a VCD launch is expected, not a fault. Do not copy **MAIN** over it unless you are
sure you do not need SMB — the alternate builds are here mainly for the `USBDELAY` cases.

**The redundant `POPSTARTER/` folder is gone.** It used to sit at the root of this package
holding a second copy of nine files that `POPS/` already contains. It also carried a
`bdma_config.txt` that said `fat32` — that file is a *marker RiptOPL writes itself* to
record which BDMAssault variant it installed, so shipping a pre-filled copy told a fresh
install that a variant was equipped when it was not. Everything you need is in `POPS/`.

## Setting up cover art and game metadata

The easiest way to get art and metadata right is **OrbitOPL Toolbox** — open
**OrbitOPL-Toolbox.url** from the package, or visit
https://github.com/Luden02/OrbitOPL-Toolbox. It is a cross-platform (Windows / macOS /
Linux) desktop app for managing an OPL library — import games from disc images, fetch
cover art and screenshots, compress ISOs to ZSO, edit per-game settings, manage virtual
memory cards — and, the part that matters here: **it writes the ART folder and per-game
configs in exactly the layout RiptOPL looks for.** Most "looks wrong on my setup" reports
come down to files not being named the way the loader looks for them, and Toolbox gets
the naming right for you.

OrbitOPL Toolbox is third-party and independently maintained — **please report Toolbox
issues to its own repository, not here.**

<!-- The "which flavour to download" guidance deliberately does NOT live here: it belongs
     to the "## Get started" section below (in the workflow), which lists all four
     flavours. It used to sit at this block's tail, stranded above the detailed list --
     one home, one copy. -->

