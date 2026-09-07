# Rolling Release

This fork publishes a continuously-updated **`rolling`** development release so the current
publishing-branch build can be pulled straight from GitHub as development progresses, without
changing a preserved snapshot such as `current-fan-favorite`. No stable `v*` release is currently published here. `rolling` is a full (non-pre-release) release and
intentionally owns GitHub's **Latest** marker. The publishing branch is `rebuild/main` during the
rebuild. `master` is the OLD LINEAGE and is no longer wired to the release workflow at all: a
push to it builds nothing and publishes nothing.

## What the rolling release contains

Every push to the publishing branch rebuilds and republishes the `rolling` release. The headline
asset is a **full installable package** (built across four toolchain flavours, with best-effort
flavours called out if omitted). A post-publish normalizer consolidates the public download set into
archives by purpose (RA, DEBUG and language packs are listed when produced):

| Asset | What it is |
|---|---|
| `RIPTOPL-<rel>-<sha>.zip` | **The installable package.** Normally contains four loader folders that differ ONLY by the SDK toolchain they were built with (the RiptOPL code in each is identical), each explicitly labeled: `APP_RIPTOPL-PS2DEVPINNED/RIPTOPL.ELF` (#1, recommended pinned ps2dev), `APP_RIPTOPL-OFFICIALPINNED/RIPTOPL.ELF` (#2, recommended pinned ps2homebrew), `APP_RIPTOPL-PS2DEVROLLING/RIPTOPL.ELF` (#3, rolling canary), and `APP_RIPTOPL-OFFICIALROLLING/RIPTOPL.ELF` (#4, rolling canary). A best-effort flavour can be absent when its build fails, and the release notes identify it. Also includes a `POPS/` folder for PS1 support via POPSTARTER (including all loose BDMA pairs—none are embedded in the loader—the new `usbd.irx.ilink` + `usbhdfsd.irx.ilink` pair for iLink VCD launches, and `POPS/POPSTARTER VERSIONS/`, the alternate POPSTARTER builds) and an `EMBER/` folder for PS1 support via **[Ember](https://github.com/Gageformer/Ember)** by **[Gageformer](https://github.com/Gageformer)**, the second PS1 core; the normally bundled **[Neutrino](https://github.com/rickgaiser/neutrino)** core by **[rickgaiser](https://github.com/rickgaiser)** as a ready-to-use `neutrino/` folder (drag-and-drop to `mc?:/`), plus `PS2-Servers.url`, `udpfs-server.url`, `OrbitPS2-Manager.url`, `OPL-PS1-AIO-Converter-GUI.url`, and `PS2RD-CHT-Manager.url`. Extract it, pick a folder and copy its `RIPTOPL.ELF` — see [Which build should I use?](#which-build-should-i-use) below. |
| `RIPTOPL-<version>-src.zip` | Source snapshot to rebuild this exact commit. |
| `RIPTOPL-LANGS-*.zip` | Extra UI language files (`.lng` + non-Latin fonts) — copy into your OPL folder. |
| `RIPTOPL-VARIANTS-*.zip` | Alternate build configs across the moving and pinned ps2dev flavours; the normalizer also adds one ready-made DualSense (`DUALSENSE=1`) loader for each available official flavour. |
| `RIPTOPL-RA-*.zip` | The **RetroAchievements** loader as a complete, installable package — same shape as the main archive (`POPS/`, `EMBER/`, `neutrino/`, the PC-tool shortcuts), with `APP_RIPTOPL-RA-*` loader folders in place of the standard ones, plus `xeRAbora.url` for the PC client the feature needs. Its own archive rather than an entry in VARIANTS so it can be picked deliberately, and because VARIANTS is excluded from the permanent MEGA archive as a diagnostic bundle. **A development build, not a finished feature**: both halves are written, none of it has been tested on hardware. See [docs/RETROACHIEVEMENTS.md](docs/RETROACHIEVEMENTS.md). |
| `RIPTOPL-DEBUG-*.zip` | Diagnostic builds across the moving and pinned ps2dev flavours, when produced. |

The final GitHub release intentionally has no floating `.ELF`, `SHA256SUMS.txt`, detailed changelog,
or SDK/IRX manifest assets. Standard loaders live in the unified package, DualSense loaders live in
VARIANTS, and build checksums remain in the workflow log/immutable MEGA archive rather than beside the
public archives.

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
Start with a pinned SDK for reproducible comparisons:

1. **`APP_RIPTOPL-PS2DEVPINNED/` (`-PS2DEVPINNED`) — recommended primary download.** Built on the
   `ps2dev/ps2dev` SDK pinned by image digest for reproducible toolchain selection. Pinning does not itself prove hardware compatibility.
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

# For just the normal installable package, copy its exact filename from the release page.
# Broad RIPTOPL-* globs also match RA, VARIANTS, DEBUG, LANGS and source archives.
gh release download rolling --repo NathanNeurotic/Open-PS2-Loader \
  --pattern "<exact-normal-package-filename>.zip" --clobber
```

Or download from the release page:
<https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/rolling>

Every prior run's assets are wiped before the new ones are uploaded, so nothing stale
accumulates (GitHub's auto "Source code" archives are added separately). The release
notes show the source commit, version, build time, the CI run that produced it, and
whether the bleeding-edge build succeeded.

## How it updates

[`.github/workflows/rolling-release.yml`](.github/workflows/rolling-release.yml):

- Triggers on every push to `rebuild/main` (updates `rolling`), on every `v*` **tag** push (cuts a
  curated per-version release with identical packaging), and on manual **Run workflow** (workflow_dispatch).
  `master` is deliberately absent from both the trigger list and the publish gate.
- Builds four labelled flavours across two toolchain lineages: moving and digest-pinned
  `ps2dev/ps2dev`, plus moving and digest-pinned official `ps2homebrew/ps2homebrew`.
- The `ps2dev/ps2dev:latest` build is **required to compile**: if it fails to build, the publish
  fails loudly. Note this guards against *build* breakage only — because `ps2dev:latest` tracks a
  moving SDK tag, a green build can still produce a binary that does not boot on hardware (see
  [Which build should I use?](#which-build-should-i-use)), which is why the pinned
  `-PS2DEVPINNED` flavour is kept as the safe fallback. The other three flavours are best-effort;
  when one fails, the package ships without that folder and the notes say so.
- Publishes/updates the single `rolling` release from the host runner as GitHub Latest and explicitly
  clears the pre-release flag.
- `concurrency` cancels superseded in-flight runs, so the release reflects the newest push.
- Refreshes the rolling release's **date**. GitHub silently ignores `published_at` on the update
  API, so the only mechanism that moves it is a draft → published flip; the workflow performs that
  flip, guarded so the channel cannot be left sitting as an invisible draft, and
  `promote-rolling-latest.yml` re-asserts the published state afterwards. Curated `v*` releases are
  never re-dated — they keep the date they were actually published.

## One pipeline, two channels

This workflow is the **single** place release packaging lives. The pushed ref picks the target:

- **`rebuild/main` push** → updates the `rolling` Latest release (the development channel).
- **`v*` tag push** → cuts the **curated per-version release** for that tag (a full release; an
  `-rc*` tag stays a pre-release). Curated tags do not displace `rolling` from GitHub's Latest marker.
  It runs through the **same normalized asset pipeline** as rolling — the same installable bundle,
  source snapshot, VARIANTS archive, and DEBUG/language archives when produced, with the same
  best-effort flavour rules — so the two channels cannot drift into different package formats.

`compilation.yml` no longer cuts releases (its release step is retired); it only runs CI +
uploads run artifacts. Per-ref `concurrency` keeps a `rebuild/main` push and a tag release from
cancelling each other.

## Not a stable release

`rolling` is a development build and may be unstable. The [Current Fan Favorite Build](https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/current-fan-favorite) is a preserved development snapshot selected after positive user feedback, not a universal compatibility guarantee.

Neutrino download/extraction is best-effort: a failed fetch can omit `neutrino/`; read the release notes before installing that core. MEGA uploads depend on successful publishing and configured credentials. Superseded or failed runs may have no archived package.
