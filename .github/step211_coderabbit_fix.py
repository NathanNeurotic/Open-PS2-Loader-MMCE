from pathlib import Path

opl = Path("src/opl.c")
s = opl.read_text()

old = (
    '        snprintf(gBootDir, sizeof(gBootDir), "pfs0:OPL");\n'
    '        gHDDStartMode = START_MODE_AUTO;\n'
    '        gBootHddCommonFallback = 1;\n'
)
new = (
    '        snprintf(gBootDir, sizeof(gBootDir), "pfs0:OPL");\n'
    '        // The launch transport is still APA even though the persistent data home did not mount.\n'
    '        // Preserve that fact so the post-config repair keeps HDD enabled for a safe retry.\n'
    '        gBootHomeApa = 1;\n'
    '        gHDDStartMode = START_MODE_AUTO;\n'
    '        gBootHddCommonFallback = 1;\n'
)
count = s.count(old)
if count != 1:
    raise SystemExit(f"APA fallback block: expected 1 match, found {count}")
s = s.replace(old, new, 1)

old = '            if (gHddSettingsFallbackNotice || gBootHddCommonFallback) {\n'
new = (
    '            // Boot fallback state only describes an HDD save when this save actually landed on PFS.\n'
    '            if (gHddSettingsFallbackNotice ||\n'
    '                (gBootHddCommonFallback && path != NULL && !strncmp(path, "pfs", 3))) {\n'
)
count = s.count(old)
if count != 1:
    raise SystemExit(f"fallback notification condition: expected 1 match, found {count}")
opl.write_text(s.replace(old, new, 1))

handoff = Path("HANDOFF.md")
s = handoff.read_text()
old = (
    '**Invariant: RiptOPL never creates an APA partition implicitly.** HDD configuration/data discovery is\n'
    'existing-partitions-only: use a valid partition named by `__common/OPL/conf_hdd.cfg`, otherwise an\n'
    'already-existing `+OPL`, otherwise the already-existing `__common` partition with `pfs0:OPL/` as the\n'
    'data home. If none mount, fail closed. Folder creation inside an already-mounted PFS partition remains\n'
    'ordinary filesystem I/O and is allowed; APA partition creation/formatting is not.\n'
)
new = (
    '**Invariant: RiptOPL never creates an APA partition implicitly.** HDD configuration/data discovery is\n'
    'existing-partitions-only: use a valid partition named by `__common/OPL/conf_hdd.cfg`, otherwise the\n'
    'already-existing `__common` partition with `pfs0:OPL/` as the data home. An unreferenced `+OPL` is\n'
    'never selected. If neither target mounts, fail closed. Folder creation inside an already-mounted PFS\n'
    'partition remains ordinary filesystem I/O and is allowed; APA partition creation/formatting is not.\n'
)
count = s.count(old)
if count != 1:
    raise SystemExit(f"HANDOFF Step-209 block: expected 1 match, found {count}")
handoff.write_text(s.replace(old, new, 1))
