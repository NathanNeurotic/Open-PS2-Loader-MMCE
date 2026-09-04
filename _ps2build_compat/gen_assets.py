#!/usr/bin/env python3
"""Generate embedded-asset .c files for the ps2build build of OPL.

Replicates the root Makefile's bin2c rules (asm/*.c), which ps2build cannot
express (no codegen steps). Every output goes to _ps2build_compat/gen/<sym>.c
and exports exactly `unsigned char <sym>[]` / `unsigned int size_<sym>` like
PS2SDK bin2c, so the EE sources link unchanged.

Real blobs are embedded where they exist (in-repo assets, prebuilt package
IRXs under $PS2DEV/packages, and the ps2build-built ee_core/elfldr ELFs when
present). Everything whose source is absent from the new SDK package layout
(or that belongs to an omitted multi-variant module) is emitted as an EMPTY
stub array so the EE compile/link can proceed; every stub is logged to
_ps2build_compat/gen/STUBS.txt and mirrored into GAPS.md by hand.

Usage (from the Open-PS2-Loader dir, with the build env on PATH):
    python3 _ps2build_compat/gen_assets.py
Re-run after `ps2build build` once ee_core.elf/elfldr.elf exist to swap the
ELF stubs for the real binaries (two-pass bootstrap).
"""

import gzip
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "_ps2build_compat", "gen")
PS2DEV = os.environ.get("PS2DEV", "C:/Users/natha/AppData/Local/ps2dev")
PACKAGES = os.path.join(PS2DEV, "packages")

PNG_ASSETS = """load0 load1 load2 load3 load4 load5 load6 load7 usb usb_bd ilk_bd
m4s_bd hdd_bd mmce hdd eth udp_bd udp_fs app cross triangle circle square select start left right
cover disc screen incebtion ip coverapp missing screens
ELF HDL ISO VCD ZSO UL APPS CD DVD Aspect_s Aspect_w Aspect_w1
Aspect_w2 Rating_0
Rating_1 Rating_2 Rating_3 Rating_4 Rating_5 Scan_240p Scan_240p1 Scan_480i Scan_480p
Scan_480p1 Scan_480p2 Scan_480p3 Scan_480p4 Scan_480p5 Scan_576i Scan_576p Scan_720p
Scan_1080i Scan_1080i2 Scan_1080p Vmode_multi Vmode_ntsc Vmode_pal logo
logo0 logo1 logo2 logo3 case
Index_0 Index_1 Index_2 Index_3 Index_4 fav fav_mark R3 L3 L1 R1 PS1 PS2 case_overlay""".split()

ADP_ASSETS = "boot cancel confirm cursor message transition bd_connect bd_disconnect".split()

# Prebuilt IOP modules shipped as package .irx in the new SDK layout.
# symbol -> path relative to $PS2DEV/packages. Paths verified against the
# installed sdk-core v2026.09.03.1 / sdk-world v2026.09.03.2 / audsrv
# v2026.09.03.1 trees on 2026-09-03; note the recurring dir-name !=
# irx-name splits (dev9/ps2dev9, fs-osd/ps2fs-osd, hdpro_atad/hdproatad,
# ps2ips-iop/ps2ips) and case-sensitive file names (iomanX, fileXio).
PKG_IRX = {
    "iomanx_irx": "core/iomanx/bin/iomanX.irx",       # Makefile: iomanX.irx
    "filexio_irx": "core/filexio/bin/fileXio.irx",    # Makefile: fileXio.irx
    "usbd_irx": "core/usbd_mini/bin/usbd_mini.irx",   # Makefile: usbd_mini.irx
    "bdm_irx": "core/bdm/bin/bdm.irx",
    "bdmfs_fatfs_irx": "core/bdmfs_fatfs/bin/bdmfs_fatfs.irx",
    "smap_irx": "core/smap/bin/smap.irx",
    "netman_irx": "core/netman/bin/netman.irx",
    "sio2man_irx": "core/sio2man/bin/freesio2.irx",   # Makefile: freesio2.irx
    "padman_irx": "core/padman/bin/freepad.irx",      # Makefile: freepad.irx
    "mcman_irx": "core/mcman/bin/mcman.irx",
    "mcserv_irx": "core/mcserv/bin/mcserv.irx",
    "libsd_irx": "core/libsd/bin/libsd.irx",
    "audsrv_irx": "world/audsrv/bin/audsrv.irx",
    "eesync_irx": "core/eesync-nano/bin/eesync-nano.irx",  # Makefile: eesync-nano.irx
    "udnl_irx": "core/udnl/bin/udnl.irx",             # Makefile default; udnl-t300 is the PADEMU-less alt
    "ps2fs_irx": "core/fs-osd/bin/ps2fs-osd.irx",     # Makefile: ps2fs-osd.irx
    "ps2hdd_irx": "core/hdd-osd/bin/ps2hdd-osd.irx",  # Makefile: ps2hdd-osd.irx
    "ps2dev9_irx": "core/dev9/bin/ps2dev9.irx",       # Makefile: ps2dev9.irx
    "ps2ip_irx": "core/ps2ip-nm/bin/ps2ip-nm.irx",    # Makefile: ps2ip-nm.irx
    "ps2ips_irx": "core/ps2ips-iop/bin/ps2ips.irx",   # Makefile: ps2ips.irx
    "poweroff_irx": "core/poweroff/bin/poweroff.irx",
    "hdpro_atad_irx": "core/hdpro_atad/bin/hdproatad.irx",  # Makefile: hdproatad.irx
    "iLinkman_irx": "core/iLinkman/bin/iLinkman.irx",
    # Release build embeds the _mini (printf-less) variants; the Makefile's
    # DEBUG=1 branch uses the full versions (core/usbmass_bd etc.).
    "usbmass_bd_irx": "core/usbmass_bd_mini/bin/usbmass_bd_mini.irx",
    "IEEE1394_bd_irx": "core/IEEE1394_bd_mini/bin/IEEE1394_bd_mini.irx",
    "mx4sio_bd_irx": "core/mx4sio_bd_mini/bin/mx4sio_bd_mini.irx",
    "smbman_irx": "world/smbman/bin/smbman.irx",
    "mmceman_irx": "world/mmceman/bin/mmceman.irx",   # Makefile: MMCE_ASSETS_DIR/mmceman.irx
    "mmcedrv_irx": "world/mmcedrv/bin/mmcedrv.irx",
    "mmceigr_irx": "world/mmceigr/bin/mmceigr.irx",
}

# In-repo committed blobs, embedded raw. symbol -> path relative to repo root.
REPO_RAW = {
    "IOPRP_img": "modules/iopcore/IOPRP.img",
    "poeveticanew_raw": "thirdparty/PoeVeticaNew.ttf",
    "icon_sys": "gfx/icon.sys",
    "icon_icn": "gfx/opl.icn",
    "icon_sys_A": "misc/icon_A.sys",
    "icon_sys_J": "misc/icon_J.sys",
    "icon_sys_C": "misc/icon_C.sys",
    "conf_theme_OPL_cfg": "misc/conf_theme_OPL.cfg",
    "theme_coverflow_cfg": "misc/theme_coverflow.cfg",
}

# modules/bdmassault blobs: gzip -9 -n, then embed as <sym>_gz.
REPO_GZ = {
    "bdma_usbd_usb_gz": "modules/bdmassault/usbd.irx.usbexfat",
    "bdma_usbhdfsd_usbexfat_gz": "modules/bdmassault/usbhdfsd.irx.usbexfat",
    "bdma_usbhdfsd_mx4sio_gz": "modules/bdmassault/usbhdfsd.irx.mx4sio",
    "bdma_usbd_mmce_gz": "modules/bdmassault/usbd.irx.mmce",
    "bdma_usbhdfsd_mmce_gz": "modules/bdmassault/usbhdfsd.irx.mmce",
    "bdma_usbd_ata_gz": "modules/bdmassault/usbd.irx.ata",
    "bdma_usbhdfsd_ata_gz": "modules/bdmassault/usbhdfsd.irx.ata",
}

# ps2build-built EE ELFs (second pass: present only after a successful build).
BUILD_ELF = {
    "eecore_elf": "build/bin/ee_core.elf",
    "elfldr_elf": "build/bin/elfldr.elf",
}

# No source exists anywhere today -> empty stub array. Each entry is a gap;
# keep in sync with GAPS.md. Empty since 2026-09-03: sdk-core v2026.09.03.1 +
# sdk-world v2026.09.03.2 + the audsrv standalone release cover every
# previously-stubbed IRX (see PKG_IRX). Keep the mechanism for future gaps.
STUBS = {}

STUB_HEADER = """/* STUB: {sym} -- {reason}
 * Source blob is unavailable in the new SDK package layout; see
 * _ps2build_compat/GAPS.md. Empty array so the EE compile/link can proceed.
 */
#ifndef __{sym}__
#define __{sym}__

unsigned int size_{sym} = 0;
unsigned char {sym}[] __attribute__((aligned(16))) = {{
    0x00,
}};

#endif
"""


def find_bin2c():
    for name in ("bin2c", "bin2c.exe"):
        p = shutil.which(name)
        if p:
            return p
    for cand in ("/c/Users/natha/bin/bin2c.exe",
                 os.path.join(PS2DEV, "bin", "bin2c.exe")):
        if os.path.isfile(cand):
            return cand
    sys.exit("bin2c not found on PATH")


def emit(bin2c, sym, data_path, stubs, reason=None):
    out = os.path.join(GEN, sym + ".c")
    if data_path and os.path.isfile(data_path):
        subprocess.run([bin2c, data_path, out, sym], check=True)
        return "real"
    with open(out, "w", newline="\n") as f:
        f.write(STUB_HEADER.format(sym=sym, reason=reason or "missing"))
    stubs.append((sym, reason or "missing"))
    return "stub"


def main():
    os.makedirs(GEN, exist_ok=True)
    bin2c = find_bin2c()
    stubs = []
    counts = {"real": 0, "stub": 0}

    def bump(r):
        counts[r] += 1

    for name in PNG_ASSETS:
        bump(emit(bin2c, name + "_png",
                  os.path.join(ROOT, "gfx", name + ".png"), stubs,
                  "gfx/%s.png missing" % name))
    for name in ADP_ASSETS:
        bump(emit(bin2c, name + "_adp",
                  os.path.join(ROOT, "audio", name + ".adp"), stubs,
                  "audio/%s.adp missing" % name))
    for sym, rel in sorted(PKG_IRX.items()):
        bump(emit(bin2c, sym, os.path.join(PACKAGES, rel), stubs,
                  "package irx %s missing" % rel))
    for sym, rel in sorted(REPO_RAW.items()):
        bump(emit(bin2c, sym, os.path.join(ROOT, rel), stubs,
                  "%s missing" % rel))
    for sym, rel in sorted(REPO_GZ.items()):
        src = os.path.join(ROOT, rel)
        if os.path.isfile(src):
            with open(src, "rb") as f:
                data = gzip.compress(f.read(), compresslevel=9, mtime=0)
            tmp = os.path.join(GEN, sym + ".gz")
            with open(tmp, "wb") as f:
                f.write(data)
            bump(emit(bin2c, sym, tmp, stubs))
            os.remove(tmp)
        else:
            bump(emit(bin2c, sym, None, stubs, "%s missing" % rel))
    for sym, rel in sorted(BUILD_ELF.items()):
        bump(emit(bin2c, sym, os.path.join(ROOT, rel), stubs,
                  "%s not built yet (or its link failed); re-run gen_assets.py "
                  "after ps2build build" % rel))
    for sym, reason in sorted(STUBS.items()):
        bump(emit(bin2c, sym, None, stubs, reason))

    with open(os.path.join(GEN, "STUBS.txt"), "w", newline="\n") as f:
        for sym, reason in stubs:
            f.write("%s: %s\n" % (sym, reason))
    print("gen_assets: %d real, %d stubs -> %s" % (counts["real"], counts["stub"], GEN))
    for sym, reason in stubs:
        print("  STUB %-28s %s" % (sym, reason))


if __name__ == "__main__":
    main()
