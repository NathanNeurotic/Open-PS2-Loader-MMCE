/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# (c) 2020 Francisco Javier Trujillo Mata <fjtrujy@gmail.com>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
#
# Modified for RiptOPL: NO IOP reset (same approach as NHDDL's neutrino
# handoff). This child loader runs from BIOS-unused memory (0x84000), loads
# the target ELF through the STILL-LIVE IOP -- OPL's drivers and mounts stay
# up -- and jumps into it. The target (Neutrino/POPSTARTER) reads its
# config/modules and the game through those mounts, then performs its own
# IOP reset.
#
# The target is read via SifLoadElf (rom0:LOADFILE) FIRST, with fileXio
# (iomanX) as the rescue path. Both are needed:
#   * LOADFILE cannot see iomanX-ONLY filesystems -- mmceman registers its
#     mmceN: device with iomanX and never with ioman (binary import tables:
#     mmceman imports iomanx, bdmfs_fatfs imports ioman) -- so an MMCE-hosted
#     neutrino.elf probed fine from OPL (fileXio) but SifLoadElf() returned
#     -ENOENT here. LOADFILE fails CLEANLY on those, and fileXio takes over.
#   * The SDK's EE fileXio client is a moving target: the 2026-05 snapshot
#     (the -WOPLSDK flavour's container) sits between the 2026-06-03 "SIF
#     binding handling" and 2026-06-17 "cleaner (re-)initialization" fixes,
#     and its client misloads from a fresh child program -- VCD/POPSTARTER
#     launches on that flavour bounced to OSDSYS when fileXio was primary.
#     Keeping the battle-tested LOADFILE first means ioman-visible devices
#     (mass/mc, i.e. every POPSTARTER launch) never touch the fragile client.
*/

#include <kernel.h>
#include <loadfile.h>
#include <ps2sdkapi.h>
#include <sifrpc.h>
#include <fileXio_rpc.h>
#include <fileio.h>
#include <errno.h>
#include <string.h>

//--------------------------------------------------------------
// Redefinition of init/deinit libc:
//--------------------------------------------------------------
// DON'T REMOVE is for reducing binary size.
// These functions are defined as weak in /libc/src/init.c
//--------------------------------------------------------------
void _libcglue_init() {}
void _libcglue_deinit() {}
void _libcglue_args_parse(int argc, char **argv) {}

DISABLE_PATCHED_FUNCTIONS();
DISABLE_EXTRA_TIMERS_FUNCTIONS();
PS2_DISABLE_AUTOSTART_PTHREAD();

#define ELF_MAGIC   0x464c457f
#define ELF_PT_LOAD 1

typedef struct
{
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf_header_t;

typedef struct
{
    u32 type;
    u32 offset;
    void *vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} elf_pheader_t;

//--------------------------------------------------------------
// Clear user memory
// PS2Link (C) 2003 Tord Lindstrom (pukko@home.se)
//         (C) 2003 adresd (adresd_ps2dev@yahoo.com)
//--------------------------------------------------------------
static void wipeUserMem(void)
{
    int i;
    for (i = 0x100000; i < GetMemorySize(); i += 64) {
        __asm__ __volatile__(
            "\tsq $0, 0(%0) \n"
            "\tsq $0, 16(%0) \n"
            "\tsq $0, 32(%0) \n"
            "\tsq $0, 48(%0) \n" ::"r"(i));
    }
}

static int readAll(int fd, void *buf, int size)
{
    u8 *p = (u8 *)buf;
    while (size > 0) {
        int got = fileXioRead(fd, p, size);
        if (got <= 0)
            return -1;
        p += got;
        size -= got;
    }
    return 0;
}

// Manual ELF load through the resident fileXio server (iomanX-aware). The program segments load
// into user memory (>= 0x100000, already wiped) well above this loader's bram home, so reading
// straight to each vaddr is safe. Returns 0 and sets *entry on success. gp stays 0 at ExecPS2:
// standard PS2 crt0s load $gp themselves (POPSLoader's embedded loader ships the same way).
static int loadElfViaFileXio(const char *path, u32 *entry)
{
    elf_header_t eh;
    elf_pheader_t ph;
    int fd, i, loaded = 0;

    if (fileXioInit() < 0)
        return -1;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return -1;

    // phentsize must match our struct or the per-header stride below reads garbage.
    if (readAll(fd, &eh, sizeof(eh)) != 0 || _lw((u32)&eh.ident) != ELF_MAGIC || eh.phnum == 0 ||
        eh.phentsize != sizeof(elf_pheader_t)) {
        fileXioClose(fd);
        return -1;
    }

    for (i = 0; i < eh.phnum; i++) {
        if (fileXioLseek(fd, eh.phoff + i * sizeof(elf_pheader_t), SEEK_SET) < 0 ||
            readAll(fd, &ph, sizeof(ph)) != 0) {
            fileXioClose(fd);
            return -1;
        }
        if (ph.type != ELF_PT_LOAD || ph.memsz == 0)
            continue;
        // Defensive bounds: a truncated/half-copied ELF (a real hazard on flaky cards) must fail
        // CLEANLY here -- into the SifLoadElf fallback / -ENOENT -- not scribble over the kernel
        // or this loader (below 0x100000) or wrap past the end of RAM. The memsz-vs-RAM test runs
        // FIRST so the subtraction in the last clause cannot underflow.
        if (ph.filesz > ph.memsz || (u32)ph.vaddr < 0x100000 ||
            ph.memsz > (u32)GetMemorySize() || (u32)ph.vaddr > (u32)GetMemorySize() - ph.memsz) {
            fileXioClose(fd);
            return -1;
        }
        if (ph.filesz > 0) {
            if (fileXioLseek(fd, ph.offset, SEEK_SET) < 0 || readAll(fd, ph.vaddr, ph.filesz) != 0) {
                fileXioClose(fd);
                return -1;
            }
        }
        if (ph.memsz > ph.filesz)
            memset((u8 *)ph.vaddr + ph.filesz, 0, ph.memsz - ph.filesz);
        loaded++;
    }
    fileXioClose(fd);

    if (!loaded)
        return -1;
    *entry = eh.entry;
    return 0;
}

// argv[0] = path of the ELF to LOAD; argv[1..] = the target's FULL argv, forwarded verbatim
// (argv[1] becomes the target's argv[0]). The caller CONTROLS the target's argv[0]: Neutrino
// gets its own path (NHDDL convention), POPSTARTER gets the "XX./SB." selector it string-parses
// to pick its backend -- the stock SDK loader clobbers argv[0] with the load path, which is
// exactly what sent POPSTARTER down its HDD "__common" route on every non-HDD VCD launch.
// The ExecPS2 syscall marshals the strings, so wiping user memory is safe.
int main(int argc, char *argv[])
{
    static t_ExecData elfdata;
    u32 entry;
    int ret;

    if (argc < 2)
        return -EINVAL;

    SifInitRpc(0);
    wipeUserMem();

    // Writeback data cache before loading ELF.
    FlushCache(0);

    // Primary: the classic LOADFILE path (see header -- every ioman-visible device stays on it).
    elfdata.epc = 0;
    SifLoadFileInit();
    ret = SifLoadElf(argv[0], &elfdata);
    SifLoadFileExit();
    if (ret == 0 && elfdata.epc != 0) {
        FlushCache(0);
        FlushCache(2);
        return ExecPS2((void *)elfdata.epc, (void *)elfdata.gp, argc - 1, &argv[1]);
    }

    // Rescue: fileXio (iomanX) for the devices LOADFILE cannot see (mmceN:, pfs, ...).
    if (loadElfViaFileXio(argv[0], &entry) == 0) {
        FlushCache(0);
        FlushCache(2);
        return ExecPS2((void *)entry, NULL, argc - 1, &argv[1]);
    }

    SifExitRpc();
    return -ENOENT;
}
