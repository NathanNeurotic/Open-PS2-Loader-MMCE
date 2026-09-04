#!/usr/bin/env python3
"""Append the IOP module targets to ps2.yaml (keeps the shared package
include list in one place instead of hand-repeating it 38 times).

Run from the Open-PS2-Loader dir:  python3 _ps2build_compat/gen_iop_yaml.py
Idempotent: strips everything below the "# IOP MODULES" marker first.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAML = os.path.join(ROOT, "ps2.yaml")
MARKER = "  # ======================= IOP MODULES =======================\n"

# Every IOP-relevant core package, resolved via ps2build's own headers:
# field (config.Target - see resolveTargetHeaders in internal/buildgraph/
# build.go) instead of a vendored copy or a hand-expanded absolute
# include_dirs:. Works for both kind: library (cdvd, netman, ps2ip,
# ioprpgen) and kind: driver (everything else here - real IOP kernel
# modules like loadcore/sysclib, never linked, only imported via SIF at
# runtime) since ps2build's resolvePackageLibs now falls back to a
# package's DriverArtifact when it has no library artifact, pulling its
# include_dirs/headers only - see that function's own comment. common/
# and startup/ are auto-added by ps2build.
#
# loadcore alone covers irx.h/types.h/defs.h/fcntl.h/sys/{fcntl,time,
# types,unistd}.h - all real, already shipped there (confirmed: they're
# ps2sdk's own iop/kernel/include/ files, just unreachable by package
# name before ps2build's driver-artifact fallback existed). atad/dev9/
# ps2ip-nm replace what used to be vendored under _ps2build_compat/ -
# now real, declared package headers too (ps2ip-nm's package.yaml
# didn't declare include_dirs at all until this was found).
BASE_HEADERS = [
    "loadcore", "sysclib", "stdio", "threadman", "intrman", "sysmem",
    "ioman", "iomanx", "sifman", "sifcmd", "modload", "dmacman",
    "timrman", "cdvd", "libsd", "bdm", "usbd", "mcman", "secrman",
    "excepman", "heaplib", "mtapman", "padman", "sio2man", "rmman",
    "vblank", "smap", "netman", "ps2ip", "ioprpgen", "ssbusc",
    "atad", "dev9", "ps2ip-nm",
]

D = "modules"  # shorthand


def mod(name, dirpath, sources, imports="imports.lst", exports=None,
        incs=(), defines=(), cflags=(), libs=(), opt=None):
    return dict(name=name, dir=dirpath, sources=sources, imports=imports,
                exports=exports, incs=list(incs), defines=list(defines),
                cflags=list(cflags), libs=list(libs), opt=opt)


MODULES = [
    mod("imgdrv", "modules/iopcore/imgdrv", ["imgdrv.c"]),
    mod("cdvdfsv", "modules/iopcore/cdvdfsv",
        ["cdvdfsv.c", "searchfile.c", "ncmd.c", "scmd.c"],
        exports="exports.tab", incs=["modules/iopcore/common"]),
    mod("resetspu", "modules/iopcore/resetspu", ["resetspu.c"]),
    mod("iremsndpatch", "modules/iopcore/patches/iremsndpatch",
        ["main.c", "asm.S"]),
    mod("apemodpatch", "modules/iopcore/patches/apemodpatch", ["main.c"]),
    mod("f2techioppatch", "modules/iopcore/patches/f2techioppatch", ["main.c"]),
    mod("cleareffects", "modules/iopcore/patches/cleareffects", ["main.c"]),
    mod("isofs", "modules/isofs", ["isofs.c", "zso.c", "lz4.c"]),
    mod("bdmevent", "modules/bdmevent", ["main.c"], incs=["include"]),
    # SMSUTILS: pure asm, no imports.lst/exports.tab at all.
    mod("smsutils", "modules/network/SMSUTILS", ["SMSUTILS.S"],
        imports=None, incs=["modules/iopcore/common"]),
    mod("ingame_smstcpip", "modules/network/SMSTCPIP",
        ["ps2ip.c", "inet.S", "ip.c", "ip_addr.c", "ip_frag.c", "etharp.c",
         "tcp_in.c", "tcp_out.c", "tcp.c", "tcpip.c", "mem.c", "api_lib.c",
         "api_msg.c", "sockets.c", "netif.c", "udp.c", "memp.c", "icmp.c",
         "pbuf.c", "timers.c", "loopif.c"],
        exports="exports.tab",
        incs=["modules/network/SMSTCPIP/include", "modules/iopcore/common"],
        defines=["LWIP_NOASSERT", "INGAME_DRIVER", "INTERRUPT_CTX_INPKT"],
        cflags=["-mno-memcpy", "-Wno-attributes", "-Wno-array-bounds",
                "-Wno-unused-but-set-variable"]),
    mod("smap_ingame", "modules/network/smap-ingame",
        ["main.c", "smap.c", "xfer.c"],
        incs=["modules/iopcore/common", "modules/network/common"]),
    mod("smap_udpbd", "modules/network/smap_udpbd",
        ["src/main.c", "src/smap.c", "src/xfer.c", "src/ministack.c",
         "src/udpbd.c", "src/udptty.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/network/smap_udpbd/src/include"],
        cflags=["-mno-check-zero-division"]),
    mod("udpfs_smap", "modules/network/udpfs_smap",
        ["src/main.c", "src/smap.c", "src/xfer.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/network/udpfs_smap/include",
              "modules/network/udpfs_smap/src"],
        cflags=["-mno-check-zero-division"]),
    mod("udpfs_ministack", "modules/network/udpfs_ministack",
        ["src/main.c", "src/ministack_arp.c", "src/ministack_eth.c",
         "src/ministack_ip.c", "src/ministack_udp.c", "src/udptty.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/network/udpfs_ministack/include",
              "modules/network/udpfs_ministack/src",
              "modules/network/udpfs_smap/include"],
        cflags=["-mno-check-zero-division"]),
    mod("udpfs_bd", "modules/network/udpfs_bd",
        ["src/main.c", "src/udprdma.c", "src/udpfs_core.c", "src/udpfs_bd.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/network/udpfs_bd/src",
              "modules/network/udpfs_smap/include",
              "modules/network/udpfs_ministack/include"],
        defines=["FEATURE_UDPFS_BD", "UDPRDMA_MAX_SOCKETS=1"],
        cflags=["-mno-check-zero-division"]),
    mod("udpfs_ioman", "modules/network/udpfs_ioman",
        ["src/main.c", "src/udprdma.c", "src/udpfs_core.c",
         "src/udpfs_ioman.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/network/udpfs_ioman/src",
              "modules/network/udpfs_smap/include",
              "modules/network/udpfs_ministack/include"],
        defines=["FEATURE_UDPFS_IOMAN", "UDPRDMA_MAX_SOCKETS=1"],
        cflags=["-mno-check-zero-division"]),
    mod("smbinit", "modules/network/smbinit",
        ["main.c", "smbauth.c", "des.c", "md4.c"],
        incs=["modules/iopcore/common"]),
    mod("nbns", "modules/network/nbns", ["main.c", "nbns.c"],
        exports="exports.tab", incs=["include"]),
    mod("httpclient", "modules/network/httpclient", ["main.c", "httpclient.c"],
        exports="exports.tab", incs=["modules/network/common"]),
    # Fork-vendored atad (ata_bd.irx); target named ps2atad so embed_irx
    # yields the ps2atad_irx symbol the EE side expects.
    mod("ps2atad", "modules/hdd/atad", ["src/ps2atad.c"],
        imports="src/imports.lst", exports="src/exports.tab",
        incs=["modules/hdd/atad/src"],
        defines=["ATA_USE_DEV9=1", "ATA_GAMESTAR_WORKAROUND=1",
                 "ATA_ENABLE_BDM=1"]),
    mod("xhdd", "modules/hdd/xhdd", ["xhdd.c", "xatad.c", "hdpro.c"],
        incs=["modules/hdd/common"]),
    mod("genvmc", "modules/vmc/genvmc", ["genvmc.c"]),
    mod("ds34usb", "modules/ds34usb/iop", ["ds34usb.c", "smsutils.S"],
        incs=["include"]),
    mod("ds34bt", "modules/ds34bt/iop", ["ds34bt.c", "smsutils.S"],
        incs=["include"]),
    # pademu variants (one source dir, per-variant defines).
    mod("usb_pademu", "modules/pademu",
        ["pademu.c", "sys_utils.c", "padmacro.c", "ds34common.c", "ds34usb.c"],
        exports="exports.tab", incs=["include"],
        defines=["USE_SMSUTILS", "USB"]),
    mod("bt_pademu", "modules/pademu",
        ["pademu.c", "sys_utils.c", "padmacro.c", "ds34common.c", "ds34bt.c"],
        exports="exports.tab", incs=["include"],
        defines=["USE_SMSUTILS", "BT"]),
    # lwNBD (downloaded by download_lwNBD.sh): ships pre-generated
    # imports.c/exports.c instead of imports.lst/exports.tab, listed as
    # plain sources in the Makefile's link order.
    mod("lwnbdsvr", "modules/network/lwNBD",
        ["ports/playstation2/lwnbd_irx.c", "ports/playstation2/exports.c",
         "ports/playstation2/imports.c", "src/servers.c", "src/plugins.c",
         "src/contexts.c", "servers/nbd/protocol-handshake.c",
         "servers/nbd/protocol.c", "servers/nbd/nbd.c", "servers/nbd/tcp.c",
         "plugins/atad/atad_d.c", "plugins/memory/memory.c",
         "plugins/mcman/mcman_d.c"],
        imports=None,
        incs=["modules/network/lwNBD/include",
              "modules/network/lwNBD/ports/playstation2", "include"],
        defines=['APP_NAME=\\"lwnbdsvr\\"'],
        cflags=["-include", ROOT.replace("\\", "/") +
                "/modules/network/lwNBD/ports/playstation2/ps2sdk-compat.h"],
        libs=["gcc"]),
]

# cdvdman variants (one dir, 6 flavour builds in the classic Makefile).
CDVDMAN_BASE = ["cdvdman.c", "ioops.c", "ncmd.c", "scmd.c", "searchfile.c",
                "streaming.c", "ioplib_util.c", "smsutils.S",
                "../../isofs/zso.c", "../../isofs/lz4.c"]
# The classic build links -lbdm (ps2sdk's IOP libbdm.a: bd_defrag.o +
# bd_cache.o) into the two BDM cdvdman variants. The new bdm package now
# ships that same archive for real (core/bdm/package.yaml's second
# artifact, lib/libbdm.a) - libs: ["bdm"] pulls it in directly, no
# vendored/recompiled copy needed.
for name, extra, defines, incs, libs in [
    ("bdm_cdvdman", ["device-bdm.c"], ["BDM_DRIVER"], [], ["bdm"]),
    ("bdm_ata_cdvdman", ["device-bdm.c", "atad.c", "dev9.c"],
     ["BDM_DRIVER", "USE_BDM_ATA", "__USE_DEV9"], [], ["bdm"]),
    ("mmce_cdvdman", ["device-mmce.c"], ["MMCE_DRIVER"], [], []),
    ("smb_cdvdman", ["device-smb.c", "smb.c", "dev9.c"],
     ["SMB_DRIVER", "__USE_DEV9"], ["modules/network/common"], []),
    ("hdd_cdvdman", ["device-hdd.c", "atad.c", "dev9.c"],
     ["HDD_DRIVER", "__USE_DEV9"], [], []),
    ("hdd_hdpro_cdvdman", ["device-hdd.c", "hdpro_atad.c"],
     ["HDD_DRIVER", "HD_PRO"], [], []),
]:
    MODULES.append(mod(name, "modules/iopcore/cdvdman", CDVDMAN_BASE + extra,
                       exports="exports.tab",
                       incs=["modules/iopcore/common"] + incs,
                       defines=defines, libs=libs))

# mcemu variants.
MCEMU_BASE = ["mcemu.c", "mcemu_io.c", "mcemu_sys.c", "mcemu_var.c",
              "mcemu_rpc.c"]
for name, extra, defines in [
    ("bdm_mcemu", ["device-bdm.c"], ["PADEMU", "BDM_DRIVER"]),
    ("mmce_mcemu", ["device-mmce.c"], ["PADEMU", "MMCE_DRIVER"]),
    ("hdd_mcemu", ["device-hdd.c"], ["PADEMU", "HDD_DRIVER"]),
    ("smb_mcemu", ["device-smb.c"], ["PADEMU", "SMB_DRIVER"]),
]:
    MODULES.append(mod(name, "modules/mcemu", MCEMU_BASE + extra,
                       defines=defines))


def render(m):
    out = []
    out.append("  - name: %s" % m["name"])
    out.append("    type: iop")
    out.append("    sources:")
    for s in m["sources"]:
        out.append("      - %s/%s" % (m["dir"], s))
    out.append("    include_dirs:")
    out.append("      - %s" % m["dir"])  # irx_imports.h lives here
    for i in m["incs"]:
        out.append("      - %s" % i)
    out.append("    headers:")
    for h in BASE_HEADERS:
        out.append("      - %s" % h)
    if m["defines"]:
        out.append("    defines:")
        for d in m["defines"]:
            out.append("      - %s" % d)
    if m["cflags"]:
        out.append("    cflags:")
        for c in m["cflags"]:
            out.append("      - %s" % c)
    if m["opt"]:
        out.append("    opt: %s" % m["opt"])
    if m["imports"]:
        out.append("    imports_lst: %s/%s" % (m["dir"], m["imports"]))
    if m["exports"]:
        out.append("    exports_tab: %s/%s" % (m["dir"], m["exports"]))
    if m["libs"]:
        out.append("    libs:")
        for l in m["libs"]:
            out.append("      - %s" % l)
    return "\n".join(out) + "\n"


def main():
    with open(YAML, "r", newline="") as f:
        text = f.read()
    if MARKER in text:
        text = text[:text.index(MARKER)]
    if not text.endswith("\n"):
        text += "\n"
    text += "\n" + MARKER
    text += "  # Generated by _ps2build_compat/gen_iop_yaml.py -- edit the\n"
    text += "  # generator, not this section.\n"
    for m in MODULES:
        text += "\n" + render(m)
    with open(YAML, "w", newline="\n") as f:
        f.write(text)
    print("ps2.yaml: %d iop targets appended" % len(MODULES))


if __name__ == "__main__":
    main()
