<!--
  REUSABLE ROLLING-RELEASE NOTES BLOCK — read by .github/workflows/rolling-release.yml
  ("Build release notes" step cats this file into notes.md verbatim, right after the
  version/commit/built/run header, on every rolling publish). Edit it here, once; every
  rolling release then carries it without being re-drafted. Do NOT paste a copy into an
  individual release body — that fork would drift from this one.

  This block is the new-lineage statement. It stays until parity with the pre-rebuild
  build is declared; when that happens, remove the block AND the cat that reads it.
  Keep the voice: warm, direct, no corporate padding. Plain Markdown — no liquid/templating.
-->

**Before anything else — read this.**

This build is a **new lineage**. RiptOPL was rebuilt from official OPL after the previous
line engineered itself into a corner. It is **not yet at parity** with the build it
replaces — the goal before anything else is reaching at least the performance of the
abandoned build. New features come after parity, not before.

**MMCE is still awaiting reimplementation.** If your games are on MMCE, this build will
not see them yet. This line stays in every release until MMCE lands, so nobody downloads
expecting it.

**The old build is still available and still fine to use.** The final release of that
lineage is on the releases page (*Rolling Alpha*), and the archive is on MEGA. If you are
happy on it, stay on it — it is not going anywhere.

**Bug reports are now tracked only against this new lineage.** I will assume any report
refers to it unless you tell me otherwise. Reports filed during the transition are treated
as still-open and need re-verification against this build.

**Re-report anything you have seen fixed before.** Regressions are possible and every
setup is different — an unreported one can sit unfixed indefinitely. A duplicate report
costs nothing; a silent one costs everyone.

**Every report is genuinely valued.** This does not improve without them: the last two
weeks of fixes came almost entirely from tester reports, not from reading code.

**Which one to download:** prefer a **PINNED** flavour — `APP_RIPTOPL-PS2DEVPINNED` or
`APP_RIPTOPL-OFFICIALPINNED` in the package zip. Pinned builds are digest-locked and
reproducible, so a report against one can be rebuilt and compared later. When you report,
**name the flavour you are running — Settings → About shows it** in the version string.
That one line is what makes a report actionable.
