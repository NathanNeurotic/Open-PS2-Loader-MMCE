# Rolling Release

This fork publishes a continuously-updated **`rolling`** pre-release so the current
`master` build can be pulled straight from GitHub as development progresses, without
touching the curated `v*` tagged releases. `rolling` is the **only** channel updated on
each `master` push.

## What the rolling release contains

Every push to `master` rebuilds and republishes the `rolling` pre-release. The headline
asset is a **full installable package** (built with both toolchains); the bare loader
ELFs and supporting files are published alongside it:

| Asset | What it is |
|---|---|
| `RIPTOPL-<rel>-<sha>.zip` | **The installable package.** Contains two loader folders that differ ONLY by the SDK toolchain they were built with (the RiptOPL code in each is identical), each explicitly labeled and recommended in this order: `APP_RIPTOPL-PS2DEVLATESTSDK/RIPTOPL.ELF` (#1), built on the current `ps2dev:latest` SDK with stock drivers, then `APP_RIPTOPL-PS2DEVPINNEDSDK/RIPTOPL.ELF` (#2), the same ps2dev SDK **pinned by image digest** to a day it was known good, kept as the safe fallback. The pinned job is best-effort (`continue-on-error`), so if it fails on a given run its folder is simply omitted and the package ships the latest-SDK folder alone — the latest build is the one that gates the publish. There is no unlabeled default folder. Also includes the `POPSTARTER/` + `POPS/` folders for PS1 support, the bundled Neutrino core as a ready-to-use `neutrino/` folder (drag-and-drop to `mc?:/`), and `PS2-Servers.url`, a shortcut to the maintained UDPFS / SMBv1 / UDPBD all-in-one PC launcher. The old `PC-SMB-Server/` folder is no longer embedded. Extract it, pick a folder and copy its `RIPTOPL.ELF` — see [Which build should I use?](#which-build-should-i-use) below. |
| `RIPTOPL-<version>-PS2DEVPINNEDSDK.ELF` | Bare loader, digest-pinned `ps2dev/ps2dev` toolchain (**safe fallback**; in-app version ends `-PS2DEVPINNEDSDK`). |
| `RIPTOPL-<version>-PS2DEVLATESTSDK.ELF` | Bare loader, `ps2dev/ps2dev:latest` toolchain (**recommended**; in-app version ends `-PS2DEVLATESTSDK`). |
| `RIPTOPL-<version>-<SDK>-ds5.ELF` | Same as the bare loader for each SDK flavour, **with DualSense (DS5 USB) pad support** compiled in (`DUALSENSE=1`). One per flavour (`-PS2DEVLATESTSDK-ds5` / `-PS2DEVPINNEDSDK-ds5`); same reliability order applies. The default builds keep DualSense OFF. Best-effort — a flavour's DS5 build may be absent if it failed that run. |
| `RIPTOPL-PS2DEVLATESTSDK-1080p.ELF` | **Experimental** loader with the re-added forced-**1080p** GSM video mode compiled in (`GSM1080P=1`). **Latest-SDK flavour only** — the raster is hardware-unvalidated, so it is kept out of every other asset. Selecting 1080p in the per-game GSM picker requires clearing a **three-step confirmation**; the Triangle + Cross boot combo forces safe 480p if a display can't sync it. Best-effort. |
| `RIPTOPL-<version>-src.zip` | Source snapshot to rebuild this exact commit. |
| `SHA256SUMS.txt` | SHA256 of every published binary + the source snapshot. |
| `IRX-MANIFEST*.txt` | SHA256 of every SDK-prebuilt IOP module each toolchain consumed (provenance for silent SDK-side driver swaps). |
| `RIPTOPL-LANGS-*.zip` | Extra UI language files (`.lng` + non-Latin fonts) — copy into your OPL folder. |
| `RIPTOPL-VARIANTS-*.zip` / `RIPTOPL-DEBUG-*.zip` | Alternate build configs and debug builds, both toolchains — for testing/diagnostics. |

`<version>` is the `ps2dev:latest` build's `git describe` (e.g. `v1.2.0-Beta-2559-bb25a00`); each
flavour carries the same version with a `-PS2DEVLATESTSDK` / `-PS2DEVPINNEDSDK` suffix.

## Which build should I use?

Both loaders are **the same RiptOPL code** — they differ only by the SDK toolchain that built them.
Recommended in this order, by reliability:

1. **`APP_RIPTOPL-PS2DEVLATESTSDK/` (`-PS2DEVLATESTSDK`) — the recommended download.** Built on the
   current `ps2dev:latest` SDK with its stock drivers, which is what RiptOPL is developed and tested
   against. That Docker tag **moves constantly** (often several times a day), which makes this flavour the
   best early-warning signal for upstream SDK regressions — but it also means it can *intermittently fail
   to boot* on some consoles when the SDK underneath it changes. That is expected volatility of a moving
   tag, **not** a RiptOPL bug (see issue [#102](https://github.com/NathanNeurotic/Open-PS2-Loader/issues/102)).
2. **`APP_RIPTOPL-PS2DEVPINNEDSDK/` (`-PS2DEVPINNEDSDK`) is the safe fallback.** The same ps2dev SDK,
   pinned by image digest to a day it was known good, so it does not move underneath you.
   Use it when the recommended build misbehaves on your console —
   if the recommended build black-screens at startup or the cursor does not respond, use this one
   and please say which you ran in the report.

When something misbehaves on hardware, please say **which flavour you ran** — the in-app version string's
`-PS2DEVLATESTSDK` / `-PS2DEVPINNEDSDK` suffix tells you. A PS2DEVLATESTSDK-only failure points at an
SDK regression; a both-flavour failure points at RiptOPL code. Those bits triple the value of a report.

## Pull the latest build

Because the filenames change each build, pull by the `rolling` tag rather than a fixed
filename — the `gh` CLI grabs whatever is currently published:

```sh
# Everything in the current rolling release
gh release download rolling --repo NathanNeurotic/Open-PS2-Loader --clobber

# Just the installable package zip
gh release download rolling --repo NathanNeurotic/Open-PS2-Loader \
  --pattern 'RIPTOPL-*-*-*.zip' --clobber
```

Or download from the release page:
<https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/rolling>

Every prior run's assets are wiped before the new ones are uploaded, so nothing stale
accumulates (GitHub's auto "Source code" archives are added separately). The release
notes show the source commit, version, build time, the CI run that produced it, and
whether the bleeding-edge build succeeded.

## How it updates

[`.github/workflows/rolling-release.yml`](.github/workflows/rolling-release.yml):

- Triggers on every push to `master` (updates `rolling`), on every `v*` **tag** push (cuts a
  curated per-version release with identical packaging), and on manual **Run workflow** (workflow_dispatch).
- Builds with two toolchains — `ps2dev/ps2dev:latest` (the moving target/canary) and a
  digest pin of that same `ps2dev/ps2dev` image — the same images as the main CI build.
- The `ps2dev/ps2dev:latest` build is **required to compile**: if it fails to build, the publish
  fails loudly. Note this guards against *build* breakage only — because `ps2dev:latest` tracks a
  moving SDK tag, a green build can still produce a binary that does not boot on hardware (see
  [Which build should I use?](#which-build-should-i-use)), which is why the pinned
  `-PS2DEVPINNEDSDK` flavour is kept as the safe fallback. The pinned build is best-effort
  (`continue-on-error`); when one fails, the package ships without that folder and the notes say so.
- Publishes/updates the single `rolling` pre-release from the host runner.
- `concurrency` cancels superseded in-flight runs, so the release reflects the newest push.

## One pipeline, two channels

This workflow is the **single** place release packaging lives. The pushed ref picks the target:

- **`master` push** → updates the `rolling` pre-release (the development channel).
- **`v*` tag push** → cuts the **curated per-version release** for that tag (a full release; an
  `-rc*` tag stays a pre-release). It publishes the **identical** asset set as rolling — same `.zip`
  installable bundle, both toolchains, `src.zip`, `SHA256SUMS` — so the two channels can't drift.

`compilation.yml` no longer cuts releases (its release step is retired); on `master` it only runs
CI + uploads run artifacts. Per-ref `concurrency` keeps a `master` push and a tag release from
cancelling each other.

## Not a stable release

`rolling` is a development build and may be unstable. Use the tagged releases for
known-good versions.
