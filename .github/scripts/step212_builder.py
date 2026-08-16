#!/usr/bin/env python3
import pathlib
import subprocess
import sys

REPO = pathlib.Path.cwd()
TARGET = "rebuild/step-212-apa-boot-and-bgm-resilience"
BASE = "5dcc6b0113cde82cf0487d0dfb0230813853c7ed"


def run(*args):
    print("+", " ".join(args), flush=True)
    subprocess.run(args, cwd=REPO, check=True)


def replace_once(path, old, new):
    p = REPO / path
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one match, found {count}\n--- needle ---\n{old}")
    p.write_text(text.replace(old, new, 1))


def insert_once(path, marker, insertion):
    replace_once(path, marker, insertion + marker)


def commit(title, body, files):
    run("git", "add", *files)
    run("git", "diff", "--cached", "--check")
    run("git", "commit", "-m", title, "-m", body)


run("git", "config", "user.name", "NathanNeurotic Step Builder")
run("git", "config", "user.email", "actions@users.noreply.github.com")
run("git", "fetch", "origin", TARGET)
run("git", "checkout", TARGET)
head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
if head != BASE:
    raise RuntimeError(f"Refusing to patch unexpected Step-212 head {head}; expected {BASE}")

# ---------------------------------------------------------------------------
# Commit 1: restore truthful HDD mount state and APA boot retry semantics.
# ---------------------------------------------------------------------------
replace_once(
    "src/opl.c",
    '    gOPLPart[0] = \'\\0\';\n    gHDDPrefix = "pfs0:";\n',
    '    gOPLPart[0] = \'\\0\';\n'
    '    // NULL is the only truthful pre-mount state. hddLoadSupportModules() uses non-NULL as the\n'
    '    // proof that the persistent pfs0: data home is already mounted; seeding this with "pfs0:"\n'
    '    // skipped discovery/mount entirely on a fresh APA boot.\n'
    '    gHDDPrefix = NULL;\n',
)

replace_once(
    "src/opl.c",
    '        LOG("BOOT APA boot dir %s could not resolve an HDD data home; refusing MC/raw fallback\\n", gBootDir);\n'
    '        snprintf(gBootDir, sizeof(gBootDir), "pfs0:OPL");\n'
    '        // The launch transport is still APA even though the persistent data home did not mount.\n'
    '        // Preserve that fact so the post-config repair keeps HDD enabled for a safe retry.\n'
    '        gBootHomeApa = 1;\n'
    '        gHDDStartMode = START_MODE_AUTO;\n'
    '        gBootHddCommonFallback = 1;\n'
    '        configEnd();\n'
    '        configInit(gBootDir);\n'
    '        return;\n',
    '        LOG("BOOT APA boot dir %s could not resolve an HDD data home; keeping launch identity for safe retry\\n", gBootDir);\n'
    '        // The launch transport is still APA even though the persistent data home did not mount.\n'
    '        // Keep the real launch identity rather than inventing an unmounted pfs0:OPL home. The raw\n'
    '        // hddN: config firewall makes the first read fail closed, and tryAlternateDevice retries\n'
    '        // only the existing-partition HDD ownership chain.\n'
    '        gBootHomeApa = 1;\n'
    '        gHDDStartMode = START_MODE_AUTO;\n'
    '        gBootHddCommonFallback = 0;\n'
    '        return;\n',
)

replace_once(
    "src/opl.c",
    '// Set only after an APA/PFS boot home has successfully mounted. Used after config read because\n'
    '// a stored HDD=Disabled value must not turn off the transport OPL itself was launched from.\n'
    'static int gBootHomeApa = 0;\n',
    '// Set for an APA/PFS launch identity, even when the first persistent-home mount attempt fails.\n'
    '// Used after config read because a stored HDD=Disabled value must not turn off the transport OPL\n'
    '// itself was launched from, and by discovery to prohibit an unrelated MC write fallback.\n'
    'static int gBootHomeApa = 0;\n',
)

insert_once(
    "src/opl.c",
    '    // If OPL was booted from a valid CWD/boot directory (gBootDir is set, e.g. "mc0:/OPL" or "mc0:"),\n',
    '    // APA/PFS boot identity is authoritative. If the first early mount missed because the disk was\n'
    '    // still settling, retry the existing-partition resolver here. Never turn an APA boot into an MC\n'
    '    // config home merely because that first PFS mount was not ready yet.\n'
    '    if (gBootHomeApa) {\n'
    '        value = checkLoadConfigHDD(types);\n'
    '        if (value & CONFIG_OPL)\n'
    '            return value;\n'
    '        configPrepareNotifications(gBootDir);\n'
    '        showCfgPopup = 0;\n'
    '        return 0;\n'
    '    }\n\n',
)

commit(
    "rebuild-212: restore APA boot mount-state invariant",
    "Fix the HDD-only boot regression exposed by hardware after Step 211.\n\n"
    "Step 211 made gHDDPrefix double as the persistent-PFS-mount state, but setDefaults still seeded it to pfs0:. "
    "That made hddLoadSupportModules return before discovery or fileXioMount on a fresh APA launch. The early resolver "
    "then treated an unmounted namespace as if it were merely a missing config home.\n\n"
    "Start gHDDPrefix at NULL, retain the real APA launch identity if no existing PFS home mounts, and retry only the "
    "existing-partition HDD resolver from alternate discovery. The raw hddN: config firewall and the no-create/no-format "
    "APA policy remain intact.",
    ["src/opl.c"],
)

# ---------------------------------------------------------------------------
# Commit 2: BGM CPU priority + I/O-stall headroom.
# ---------------------------------------------------------------------------
replace_once(
    "src/sound.c",
    '#define BGM_RING_BUFFER_COUNT 128 // 512 KB buffer (~2.9s of audio): glides through USB 1.1 background art reads (#364)\n'
    '#define BGM_RING_BUFFER_SIZE  4096\n'
    '#define BGM_STOP_WAIT_SLICES  16\n'
    '#define BGM_THREAD_BASE_PRIO  0x3E\n',
    '#define BGM_RING_BUFFER_COUNT 192 // 768 KB buffer (~4.35s): extra headroom for long device/list bursts (#364)\n'
    '#define BGM_RING_BUFFER_SIZE  4096\n'
    '#define BGM_STOP_WAIT_SLICES  16\n'
    '// EE priorities are strict (lower number wins): playback=30, Vorbis I/O/decode=31. Playback is\n'
    '// tiny and normally blocked in audsrv; decode now outranks the background I/O worker (32) while\n'
    '// sharing the GUI/pad tier (31), whose vsync wait yields naturally. This prevents a long runnable\n'
    '// background queue from starving refills; the larger ring also covers stalls that are IOP-bound,\n'
    '// where EE thread priority cannot help.\n'
    '#define BGM_THREAD_BASE_PRIO 0x1E\n',
)

replace_once(
    "src/texcache.c",
    '// Art thread priority. LOWER NUMBER WINS on the EE. GUI/pad thread is 31 (opl.c), the shared io\n'
    '// worker 32 (ioman.c), sound 62 (0x3E). 72 (0x48) puts art below every one of them: it can never take\n'
    '// the CPU from input, from a config save, from a list rebuild, or from audio decoding.\n',
    '// Art thread priority. LOWER NUMBER WINS on the EE. BGM playback/decode are 30/31 (sound.c), the\n'
    '// GUI/pad thread is 31 (opl.c), and the shared io worker is 32 (ioman.c). 72 (0x48) puts art below\n'
    '// every one of them: it can never take the CPU from input, config work, list rebuilds, or audio.\n',
)

commit(
    "rebuild-212: give BGM refill scheduling hard priority",
    "Hardware remains clean functionally from MC but BGM can stutter when background load becomes severe. "
    "The previous 62/63 audio priorities were below the priority-32 shared I/O worker, so a long runnable queue could "
    "starve both playback and Vorbis refills even though art itself had already been lowered to 72.\n\n"
    "Move playback/decode to 30/31 and expand the PCM ring from 512 KiB to 768 KiB (~4.35 s). The priority change covers "
    "EE scheduling starvation; the extra 256 KiB covers longer device/IOP stalls that priority cannot shorten. Art remains 72.",
    ["src/sound.c", "src/texcache.c"],
)

# ---------------------------------------------------------------------------
# Commit 3: missing/stale config.path read-only recovery + handoff docs.
# ---------------------------------------------------------------------------
helper_marker = '\n\nstatic int tryAlternateDevice(int types)\n'
helper_code = r'''

// Last-resort READ-ONLY discovery for a missing/stale config.path. This exists to recover the
// chicken-and-egg custom-settings value from an already-existing config; it never creates a config,
// never changes APA metadata, and never makes an arbitrary discovered device the permanent save home.
// A known local boot self-migrates the loaded in-memory sets back to its normal boot home; if the
// recovered config contains Custom Settings Path, _saveConfig will honor it and regenerate config.path
// on the user's next explicit Save Changes.
static int tryReadRecoveryConfigHome(int types, const char *home)
{
    DIR *dir = opendir(home);
    if (dir == NULL)
        return 0;
    closedir(dir);

    configEnd();
    configInit((char *)home);
    int value = configReadMulti(types);
    if (value & CONFIG_OPL)
        LOG("CONFIG recovery found existing settings at %s\n", home);
    return value;
}

static int tryMissingConfigPathRecovery(int types)
{
    static const char *const mcHomes[] = {
        "mc0:/OPL",
        "mc0:/",
        "mc1:/OPL",
        "mc1:/",
    };
    int value;

    // Do not trust sysCheckMC() for recovery: the report that drove this path is specifically a card
    // that served the ELF but was not selected by the normal card probe. Directly test both slots.
    for (unsigned int i = 0; i < sizeof(mcHomes) / sizeof(mcHomes[0]); i++) {
        value = tryReadRecoveryConfigHome(types, mcHomes[i]);
        if (value & CONFIG_OPL) {
            if (gBootDir[0] != '\0' && !gBootHomeDeferred)
                configSetMove(gBootDir); // read-old, write-normal; no silent MC save hijack
            return value;
        }
    }

    // USB is third choice. Force only the USB transport, then inspect only slots whose live driver
    // identity is USB; ATA/MX4SIO/iLink are not pulled into this recovery scan. Check both the device
    // root and the conventional /OPL directory. configRead() itself handles the legacy filename.
    if (bdmEnsureSourceModules(BDM_TYPE_USB, 1500)) {
        int slots[MAX_BDM_DEVICES];
        int count = bdmGetDeviceSlotsByType(BDM_TYPE_USB, slots, MAX_BDM_DEVICES);
        for (int i = 0; i < count; i++) {
            char home[32];

            snprintf(home, sizeof(home), "mass%d:/", slots[i]);
            value = tryReadRecoveryConfigHome(types, home);
            if (value & CONFIG_OPL) {
                if (gBootDir[0] != '\0' && !gBootHomeDeferred)
                    configSetMove(gBootDir);
                return value;
            }

            snprintf(home, sizeof(home), "mass%d:/OPL", slots[i]);
            value = tryReadRecoveryConfigHome(types, home);
            if (value & CONFIG_OPL) {
                if (gBootDir[0] != '\0' && !gBootHomeDeferred)
                    configSetMove(gBootDir);
                return value;
            }
        }
    }

    // Every failed probe re-homed the config_set filenames. Put them back exactly where normal boot
    // discovery expects them before the existing fallback policy continues.
    configEnd();
    configInit(gBootDir[0] != '\0' ? gBootDir : NULL);
    return 0;
}
'''
insert_once("src/opl.c", helper_marker, helper_code)

replace_once(
    "src/opl.c",
    '    // APA/PFS boot identity is authoritative. If the first early mount missed because the disk was\n'
    '    // still settling, retry the existing-partition resolver here. Never turn an APA boot into an MC\n'
    '    // config home merely because that first PFS mount was not ready yet.\n'
    '    if (gBootHomeApa) {\n'
    '        value = checkLoadConfigHDD(types);\n'
    '        if (value & CONFIG_OPL)\n'
    '            return value;\n'
    '        configPrepareNotifications(gBootDir);\n'
    '        showCfgPopup = 0;\n'
    '        return 0;\n'
    '    }\n\n',
    '    // APA/PFS boot identity is authoritative. If the first early mount missed because the disk was\n'
    '    // still settling, retry the existing-partition resolver before broad read-only recovery.\n'
    '    if (gBootHomeApa) {\n'
    '        value = checkLoadConfigHDD(types);\n'
    '        if (value & CONFIG_OPL)\n'
    '            return value;\n'
    '    }\n\n'
    '    // No redirect (or a stale redirect) must not strand an otherwise valid install. Search existing\n'
    '    // conventional homes read-only: both MC slots first, then USB. A known boot home is restored as\n'
    '    // the save destination after the read, so discovery cannot silently scatter future writes.\n'
    '    value = tryMissingConfigPathRecovery(types);\n'
    '    if (value & CONFIG_OPL)\n'
    '        return value;\n\n'
    '    // An APA launch that still has no mountable existing PFS home fails closed here. Do not fall\n'
    '    // through into the legacy MC write-home logic merely because recovery also found nothing.\n'
    '    if (gBootHomeApa) {\n'
    '        configPrepareNotifications(gBootDir);\n'
    '        showCfgPopup = 0;\n'
    '        return 0;\n'
    '    }\n\n',
)

# Update the handoff pointer and append Step 212's three-commit scope.
replace_once(
    "HANDOFF.md",
    '**Next number: 212. Current tip: `rebuild/step-211-consolidated-apa-config-safety`.** One focused change\n',
    '**Next number: 213. Current tip: `rebuild/step-212-apa-boot-and-bgm-resilience`.** One focused change\n',
)

handoff = REPO / "HANDOFF.md"
text = handoff.read_text()
append = r'''

---

## Step 212 — APA boot recovery, BGM starvation protection, and config.path recovery

Three hardware-driven corrections are intentionally shipped together for one test round.

1. **APA boot mount-state invariant.** `gHDDPrefix` now starts at `NULL`; only a successful existing-PFS
   mount assigns `pfs0:` / `pfs0:OPL/`. Step 211 had made non-NULL mean "mounted" while `setDefaults()`
   still pre-seeded `"pfs0:"`, allowing a fresh HDD boot to skip discovery/mount entirely. A failed first
   APA mount keeps the real launch identity, retries only the existing-partition HDD resolver, never
   falls through to an MC write home, and never creates/formats APA metadata.
2. **BGM load resilience.** PCM buffering grows from 128 to 192 × 4096-byte slots (768 KiB, about 4.35 s).
   Playback/decode priorities move from 62/63 to 30/31: playback is tiny and normally blocked in audsrv;
   Vorbis refill now outranks the priority-32 background I/O worker while art remains priority 72. This
   addresses both EE scheduling starvation and longer IOP/device stalls.
3. **Missing/stale `config.path` recovery.** If normal home/redirect loading fails, RiptOPL performs a
   read-only search for an already-existing master config on `mc0` (OPL dir then root), `mc1` (same),
   then mounted USB devices (root then `/OPL`). Both MC slots are probed directly rather than relying on
   `sysCheckMC()`. USB recovery force-loads only the USB transport and filters by live USB device type.
   A known local boot restores its normal save home after the read, so this cannot silently scatter new
   config files. If the recovered config contains Custom Settings Path, the next explicit Save Changes
   honors it and regenerates `config.path` through the normal guarded writer.

The Step-209/210/211 data-integrity barriers remain non-negotiable: raw `hddN:` config I/O is blocked,
`conf_hdd.cfg` may select only an existing mountable PFS target, `__common/OPL/` is the canonical HDD
fallback, and no config/discovery path creates, formats, resizes, or repairs an APA partition.
'''
if "## Step 212 — APA boot recovery" in text:
    raise RuntimeError("HANDOFF already contains Step 212 section")
handoff.write_text(text.rstrip() + append + "\n")

commit(
    "rebuild-212: recover settings when config.path is missing",
    "A second hardware/user report shows valid existing settings can be stranded until Save Changes creates config.path: "
    "the normal MC selector can miss the card that actually booted OPL, and a custom USB settings home is otherwise a "
    "chicken-and-egg dependency.\n\n"
    "Add a read-only recovery pass after normal/redirect loading fails: mc0, mc1, then USB conventional homes. Direct MC "
    "slot probes avoid sysCheckMC selection ambiguity; USB recovery loads only USB and filters live USB slots. On a known "
    "local boot the config filenames are moved back to the normal boot home after reading, so recovery cannot become a "
    "silent alternate-device write policy. A recovered Custom Settings Path is honored on the next explicit save, which "
    "then writes config.path through the existing guarded writer.\n\n"
    "Also document all Step-212 hardware fixes and advance the handoff pointer to 213.",
    ["src/opl.c", "HANDOFF.md"],
)

run("git", "log", "--oneline", "--decorate", "-4")
run("git", "diff", BASE + "..HEAD", "--check")
run("git", "push", "origin", "HEAD:" + TARGET)
print("STEP212_FINAL=" + subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip())
