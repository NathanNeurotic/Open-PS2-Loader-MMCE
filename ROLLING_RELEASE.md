# Rolling Release

This fork publishes a continuously-updated **`rolling`** pre-release so the current
`master` build can be pulled straight from GitHub as development progresses, without
touching the curated `v*` tagged releases. `rolling` is the **only** channel updated on
each `master` push.

## What the rolling release contains

Every push to `rebuild/main` (or `master`) rebuilds and republishes the `rolling` pre-release. The headline
asset is a **full installable package** (built with all four toolchain flavours); the bare loader
ELFs, DualSense loaders, and supporting files are published alongside it:

| Asset | What it is |
|---|---|
| `RIPTOPL-<rel>-<sha>.zip` | **The installable package.** Contains four loader folders that differ ONLY by the SDK toolchain they were built with (the RiptOPL code in each is identical), each explicitly labeled: `APP_RIPTOPL-PS2DEVPINNED/RIPTOPL.ELF` (#1, recommended pinned ps2dev), `APP_RIPTOPL-OFFICIALPINNED/RIPTOPL.ELF` (#2, recommended pinned ps2homebrew), `APP_RIPTOPL-PS2DEVROLLING/RIPTOPL.ELF` (#3, rolling canary), and `APP_RIPTOPL-OFFICIALROLLING/RIPTOPL.ELF` (#4, rolling canary). Also includes a `POPS/` folder for PS1 support via POPSTARTER (including all loose BDMA pairs—none are embedded in the loader—the new `usbd.irx.ilink` + `usbhdfsd.irx.ilink` pair for iLink VCD launches, and
`POPS/POPSTARTER VERSIONS/`, the alternate POPSTARTER builds) and an `EMBER/` folder for PS1 support
via **[Ember](https://github.com/Gageformer/Ember)** by **[Gageformer](https://github.com/Gageformer)**, the second PS1 core; the bundled **[Neutrino](https://github.com/rickgaiser/neutrino)** core by **[rickgaiser](https://github.com/rickgaiser)** as a ready-to-use `neutrino/` folder (drag-and-drop to `mc?:/`), plus `PS2-Servers.url`, `udpfs-server.url`, `OrbitPS2-Manager.url`, `OPL-PS1-AIO-Converter-GUI.url`, and `PS2RD-CHT-Manager.url`. Extract it, pick a folder and copy its `RIPTOPL.ELF` — see [Which build should I use?](#which-build-should-i-use) below. |
| `RIPTOPL-<version>-PS2DEVPINNED.ELF` | Bare loader, digest-pinned `ps2dev/ps2dev` toolchain (**recommended / primary**; in-app version ends `-PS2DEVPINNED`). |
| `RIPTOPL-<version>-OFFICIALPINNED.ELF` | Bare loader, digest-pinned `ps2homebrew/ps2homebrew` toolchain (**recommended / alternative pin**; in-app version ends `-OFFICIALPINNED`). |
| `RIPTOPL-<version>-PS2DEVROLLING.ELF` | Bare loader, `ps2dev/ps2dev:latest` toolchain (canary / early warning; in-app version ends `-PS2DEVROLLING`). |
| `RIPTOPL-<version>-OFFICIALROLLING.ELF` | Bare loader, `ps2homebrew:main` toolchain (canary / early warning; in-app version ends `-OFFICIALROLLING`). |
| `RIPTOPL-<version>-<SDK>-ds5.ELF` | Same as the bare loader for each of the four SDK flavours, **with DualSense (DS5 USB) pad support** compiled in (`DUALSENSE=1`). One per flavour (`-PS2DEVPINNED-ds5`, `-OFFICIALPINNED-ds5`, `-PS2DEVROLLING-ds5`, `-OFFICIALROLLING-ds5`). The default builds keep DualSense OFF. |
| `RIPTOPL-<version>-src.zip` | Source snapshot to rebuild this exact commit. |
| `SHA256SUMS.txt` | SHA256 of every published binary + the source snapshot. |
| `BUILD-MANIFEST.txt` | Detailed toolchain metadata, image digests, commit hashes, and compiler versions for each build. |
| `RIPTOPL-LANGS-*.zip` | Extra UI language files (`.lng` + non-Latin fonts) — copy into your OPL folder. |
| `RIPTOPL-VARIANTS-*.zip` / `RIPTOPL-DEBUG-*.zip` | Alternate build configs and debug builds across all toolchains — for testing/diagnostics. |

The current UI presents the same **PS2/PS1 Game Display** setting on Interface and PS Emulation:
**Both (L3)** switches separate device libraries; **Mixed** combines them and L3 cycles
Mixed → PS2 → PS1; **PS2** and **PS1** lock one library and make L3 fully inert. APPS is independent:
it can remain one Mixed ELF list or split Apps / `[PS1]`-titled ELFs across L3. Favorites is also
independent and always cycles **All in One → PS2 → PS1 → ELF**. Returning from Start/Settings retains
the page the user paused on. UDPFS and UDPBD PS1 views publish Ember titles only because POPSTARTER
cannot restore those network transports after its IOP reset. Confirming Network Settings now applies
the current values and reconnects immediately; Select / Refresh retries a failed network page.

`<version>` is the `ps2dev:latest` build's `git describe` (e.g. `v1.2.0-Beta-2562-c553567`); each
flavour carries the same version with its flavour suffix (`-PS2DEVPINNED`, `-OFFICIALPINNED`, `-PS2DEVROLLING`, `-OFFICIALROLLING`).

## Which build should I use?

All loaders contain **the same RiptOPL code** — they differ only by the SDK toolchain that built them.
Recommended in this order, by reliability:

1. **`APP_RIPTOPL-PS2DEVPINNED/` (`-PS2DEVPINNED`) — recommended primary download.** Built on the
   `ps2dev/ps2dev` SDK pinned by image digest to a known-good configuration so it provides reproducible,
   stable behavior.
2. **`APP_RIPTOPL-OFFICIALPINNED/` (`-OFFICIALPINNED`) — recommended official pin.** Built on the
   `ps2homebrew/ps2homebrew` official SDK, pinned by image digest.
3. **`APP_RIPTOPL-PS2DEVROLLING/` (`-PS2DEVROLLING`) — bleeding-edge canary.** Tracks `ps2dev/ps2dev:latest`.
4. **`APP_RIPTOPL-OFFICIALROLLING/` (`-OFFICIALROLLING`) — bleeding-edge official canary.** Tracks `ps2homebrew:main`.

When something misbehaves on hardware, please say **which flavour you ran** — the in-app version string's
flavour suffix tells you. Those details greatly help pin down any issues between application code and toolchain updates.

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
