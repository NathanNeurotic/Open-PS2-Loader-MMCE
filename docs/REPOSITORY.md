# Repository layout and maintenance

The root Makefile remains the build entry point. Run build and maintenance commands
from the repository root unless a tool explicitly documents another directory.

## Directory map

| Location | Purpose |
| --- | --- |
| `src/`, `include/` | Main EE application and headers |
| `ee_core/`, `elfldr/`, `modules/` | In-game EE core, ELF loader, and IOP modules |
| `gfx/`, `audio/`, `thirdparty/` | Build assets and third-party dependencies |
| `lng/`, `lng_fork/`, `lng_tmpl/` | Language outputs, fork translations, and base template |
| `POPS/`, `popsmb/`, `neutrino/`, `EMBER/` | Loader payloads and integration resources; paths can be part of the release layout |
| `pc/` | Host tools and companion-tool links |
| `labs/` | Standalone experiments and test programs |
| `misc/` | Sample configurations and icon resources |
| `docs/` | User and developer documentation |
| `docs/history/` | Dated reports and the legacy changelog |
| `tools/` | Repository dependency, language, and release helpers; see [commands](../tools/README.md) |
| `notes/`, `reference/` | Research notes and reference material |
| `.github/` | GitHub workflows, workflow helpers, and issue templates |

## Local working material

Use the ignored `tmp/` directory for disposable audit outputs and local reports.
Store durable backups and known-good hardware artifacts outside disposable build
directories, with the source SHA, toolchain, and file hashes. Do not treat an ELF
as disposable merely because Git ignores it.

Linked worktrees must be managed with `git worktree` commands. Inspect their
status, untracked files, and ownership before removing one. PR #596 removed the
broken tracked Git links under `forensic/`; local directories and worktree
registrations can still exist and must be inspected separately.

## Cleanup sequence and recovery

This cleanup starts at `9e7cc9c4089bd8cb849223437fcfbe1932d7914b` on
`codex/repository-cleanup`, targeting `rebuild/main`. Keep each step in a separate
commit so it can be reviewed or reverted independently:

1. Record the layout and maintenance rules.
2. Group dated historical reports and provide a documentation index. PR #596
   already removed the obsolete root handoffs; they remain available in Git history.
3. Group repository helper scripts and update Makefile, workflow, and documentation
   references together. Preserve their repository-root working-directory contract.
4. Validate the changed paths and generated language outputs; run the existing
   build-flavours workflow on the PR.

Before moving files, the branch incorporated `rebuild/main` at
`2e25cf3b181ca2ccaa751e532960e962043c8bdf`, including the already merged hygiene PR
#596. That merge is a separate breadcrumb; it is not part of the cleanup diff
against the updated base.

Source cleanup does not authorize bulk branch or tag deletion. A cleanup branch
isolates file edits, but refs and worktree registrations belong to the repository.
Before retiring refs, capture live names and full object IDs, check open PRs and
automation, and create and verify a Git bundle containing the objects being retired.
Back up dirty and untracked work separately; a bundle cannot preserve it.

Keep publishing branches, open-PR branches, active worktrees, published release
tags, and hardware checkpoints. Reachability from `rebuild/main` is only an initial
candidate filter: it does not establish that a branch name is unused. Squash merges
also require content/history review beyond an ancestry check. A list of SHAs alone
is an inventory, not a recoverable backup.

Do not rewrite history or merge the cleanup PR as part of preparing it. Restore
file changes with individual revert commits, preserving the review trail.
