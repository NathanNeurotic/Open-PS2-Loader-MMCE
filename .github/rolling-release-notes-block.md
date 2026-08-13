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

<!-- ONE-TIME NOTE, first new-lineage publish only: remove this paragraph (and this
     comment) from the block after the first correct rolling publish has gone out. -->
*A note on the rolling tag's history: before this publish, the `rolling` tag had been
stuck on a June 2026 commit, so the "source at this tag" link on earlier rolling releases
never matched the assets beside them. The downloadable builds were always built from the
right commit — only the tag was stale. It has been recreated for this publish. If you
ever built from that tag expecting to match a release, you didn't.*

**MMCE is still awaiting reimplementation.** If your games are on MMCE, this build will
not see them yet. This line stays in every release until MMCE lands, so nobody downloads
expecting it.

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

**Which one to download:** prefer a **PINNED** flavour — `APP_RIPTOPL-PS2DEVPINNED` or
`APP_RIPTOPL-OFFICIALPINNED` in the package zip. Pinned builds are digest-locked and
reproducible, so a report against one can be rebuilt and compared later. When you report,
**name the flavour you are running — Settings → About shows it** in the version string.
That one line is what makes a report actionable.
