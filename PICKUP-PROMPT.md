# Paste this to the next agent

You are taking over an in-flight PS2 homebrew project (**RiptOPL**, a fork of Open PS2 Loader) from
another agent that ran out of budget. I am the owner, Nathan — I test every build on real PlayStation 2
hardware, and other testers report through GitHub issues. **My side and my testers' side must not
change during this handover.** The rules below are how that stays true.

## START HERE — the exact directory

```
C:\Users\natha\Github\Open-PS2-Loader\.claude\worktrees\opl-issue-340-diagnosis-2cdb77
```

Work in **that** directory. It is a git worktree, and it is already correct in three ways that
matter:

- it is checked out on the current tip branch, **`rebuild/step-164-eth-bounded-teardown`**;
- **`HANDOFF.md` and this file are sitting in it** — read `HANDOFF.md` first, from section 0;
- ⚠ **it is the one directory the build container is mounted to.** `oplbuild92` exposes it as
  `/src`, so `docker exec oplbuild92 sh -c "cd /src && make -j8 opl.elf"` compiles *this*
  worktree and nothing else.

⚠ **Do not `git worktree add` a fresh directory to work in.** A new worktree is not what the
container builds, so you would be editing one tree and compiling another — your fixes would
appear to do nothing, which reads like a wrong diagnosis rather than a wrong setup. Make your
branches **inside** the directory above:

```bash
cd C:/Users/natha/Github/Open-PS2-Loader/.claude/worktrees/opl-issue-340-diagnosis-2cdb77
git fetch origin
git checkout -b rebuild/step-169-<slug>      # next number is 169
```

⚠ The **main checkout** at `C:/Users/natha/Github/Open-PS2-Loader` sits on `rebuild/main` and
has **no `HANDOFF.md`**. Do not start there and do not commit there.

---

# NON-NEGOTIABLE WORKING CONTRACT — read before anything else

## 1. Branch discipline

- **Never commit to `master`, `rebuild/main`, or any existing `rebuild/step-*` branch.**
- Every change goes on **a new branch you create**, named `rebuild/step-NNN-<short-slug>`.
- **The next number is 169.** Increment for each new step. One focused change per step.
- The current tip you branch FROM is **`rebuild/step-164-eth-bounded-teardown`** (it contains steps
  164–168). Always branch from the latest step branch, not from `master`.

```bash
cd C:/Users/natha/Github/Open-PS2-Loader/.claude/worktrees/opl-issue-340-diagnosis-2cdb77
git fetch origin
git checkout -b rebuild/step-169-<slug>
```
(Branch **inside** that worktree — see "START HERE" for why a new worktree will not build.)

- Write a **long, explanatory commit message** saying what changed, why, what evidence drove it, and
  what it does *not* fix. The commit messages are this project's real documentation — keep that up.
- Never force-push, never rewrite history, never merge anything to `master` without asking me.

## 2. The build → test loop, exactly as it is today

Do not change any part of this. My testers rely on it looking identical.

1. **Build locally first** (Docker container `oplbuild92` is running, repo mounted at `/src`):
   ```bash
   docker exec oplbuild92 sh -c "cd /src && rm -f obj/*.o opl.elf && make -j8 opl.elf"
   ```
   `expr: syntax error` lines are pre-existing noise — ignore them, check the ELF timestamp.
2. **Run clang-format 12** on every `.c`/`.h` you touched (CI enforces it and is not a required
   check, so a red format check can still merge):
   ```bash
   docker run --rm -v "//c/Users/natha/Github/Open-PS2-Loader/.claude/worktrees/opl-issue-340-diagnosis-2cdb77://work" \
     -w //work xianpengshen/clang-tools:12 clang-format --style=file -i <files>
   ```
3. **Push the branch, then trigger CI on it:**
   ```bash
   git push -u origin rebuild/step-169-<slug>
   gh workflow run flavours.yml --repo NathanNeurotic/Open-PS2-Loader --ref rebuild/step-169-<slug>
   gh run watch <runId> --repo NathanNeurotic/Open-PS2-Loader --exit-status
   ```
   ⚠ **Always pass `--repo NathanNeurotic/Open-PS2-Loader`** — `gh` otherwise targets upstream and
   fails with "No commits between".
4. **Confirm all four jobs are green before giving me links.** Never hand me a link from a run you
   have not watched to completion.
5. **Give me the links in exactly this format** — same headings, same order, every time:

```
## Test — run <runId> (`rebuild/step-NNN-<slug>`)

**For testers:**
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVPINNED.zip
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALPINNED.zip

**Early warning:**
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVROLLING.zip
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALROLLING.zip
```

   **PINNED are the ones testers get** — digest-locked and reproducible, so a report against one can
   be rebuilt and compared later. **ROLLING is early warning**: if a rolling build breaks while its
   pinned twin does not, that is the toolchain, not us. Every artifact carries `BUILD-MANIFEST.txt`.

6. **Always tell me what to look for** with a build, and **what the fallback build is** if it
   misbehaves. Name the specific HUD fields worth reading.
7. **Do not spam builds.** Stage work on branches and let me decide when a tester gets a link. My
   tester Zack asked for this explicitly: *"just dont build another, wait for my reports as usual."*

## 3. Talking to me

- I send **videos** of the hardware. There is an ffmpeg command in `HANDOFF.md` §3 for extracting the
  on-screen debug HUD from them, and a decoder for every field. **Read the numbers — they have beaten
  source reasoning repeatedly, including the previous agent's.**
- Tell me plainly when something is unproven. The previous agent shipped two builds on confident
  guesses that my hardware then corrected. I would rather have "here is what the number says and here
  is what it does not say."
- If you cause a regression, say so directly and give me the known-good build to fall back to.

---

# Now read the handoff

**`HANDOFF.md` is in the directory named at the top of this file. Start at section 0 (the working
contract), then section 14 (current state), then read the whole thing.** It has the repo layout, the HUD decoder, the full fix history, the
standing traps, and — most importantly — an explicit list of hypotheses already **disproven with code
citations**, so you do not spend a day re-deriving dead ends.


## The one-paragraph situation

The rebuild's cover art arrives in **batches rather than rolling**, on USB, SMB and VCD alike — so it
is not the device. The mature fork (`origin/master`) does not do this and is the working reference to
diff against. Several real bugs have been fixed on the way to that question (SMB art vanishing
permanently, a per-row config-file read on every settled row, a teardown deadlock, a background that
could only load as a side effect of another load finishing), but the batching itself is **not yet
explained**. Two 16-agent adversarial investigations both failed to find a cover-vs-cover publish gap
in either tree, which moves suspicion to an intermittent `open()` stall — a 140×200 cover opens in
3–8 ms normally and **2730 ms** occasionally.

## What I need from you first

1. **rebuild-164 through 168 have never run on hardware.** Give me test links in the format above and
   tell me exactly what to look for. That is the top priority; everything else waits behind real data.
2. Once I report back, the HUD field **`W<ms>@<simple>/<menu>/<bgm><h|m>`** is built specifically to
   settle the open-stall question in one reading. `HANDOFF.md` §14 explains how to interpret it.

## How I want you to work on the code

- **Diff the fork first.** Three separate bugs this week turned out to be "master does X, we do not,"
  each found only after a tester hit it. When a symptom appears, compare against
  `git show origin/master:src/FILE` before theorising.
- **Adapt, do not transplant.** A wholesale file swap was started and reverted — master's texcache is
  missing eight symbols this tree calls, plus hardware-won MX4SIO protection. Port mechanisms, not
  files.
- **Verify every bot/agent finding against the actual code before acting on it.** Reviews from other
  models produced genuinely correct findings *and* confident wrong ones in equal measure this week.
- **Every counter you add must distinguish "not used yet" from "broken."** A field reading `0/0` cost
  two entire build cycles because a dead subsystem looked identical to an idle one.

If anything in the handoff contradicts what you find in the code, **trust the code and tell me.**
