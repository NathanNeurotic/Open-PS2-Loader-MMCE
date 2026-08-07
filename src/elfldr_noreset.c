/*
  No-IOP-reset ELF handoff (NHDDL parity) for the Neutrino launch path.

  ps2sdk's LoadELFFromFileWithPartition SifLoadElf()s the target through the live IOP but then
  SifIopReset()s and reloads ONLY rom0:SIO2MAN/MCMAN/MCSERV before jumping in
  (ee/elf-loader/src/loader/src/loader.c). Neutrino must read its config/modules (-cwd) and open
  the game ISO through the LAUNCHER's mounts before doing its own IOP reset, so that handoff
  black-screens every setup whose neutrino/ folder or game lives on a BDM device.

  ps2sdk gained a NoReset entry point in 8890d8b5 (2026-06-30), but neither build container ships
  it yet (ps2dev:latest probed 2026-07-04 carries a 2026-06-21 SDK; the PS2MAXSDK pin is 2025-07-25),
  so we vendor the child loader instead (elfldr/loader.c, the NHDDL approach): copy it into
  BIOS-unused memory at 0x84000, ExecPS2 into it with argv[0] = target path, and it SifLoadElf()s
  the target through OPL's still-live mounts -- no IOP reset. Works identically on both SDKs.
*/

#include <kernel.h>
#include "include/ioman.h" // LOG (kernel argv-budget refusal trace)
#include <sifrpc.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

extern unsigned char elfldr_elf[];
extern unsigned int size_elfldr_elf;

#define ELFLDR_ELF_MAGIC 0x464c457f
#define ELFLDR_PT_LOAD   1

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
} elfldr_header_t;

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
} elfldr_pheader_t;

// Wipe the BIOS-unused region the child loader is linked into (0x84000 - 0x100000,
// below OPL itself -- see elfldr/linkfile).
static void wipeBramMem(void)
{
    int i;
    for (i = 0x00084000; i < 0x100000; i += 64) {
        __asm__ __volatile__(
            "\tsq $0, 0(%0) \n"
            "\tsq $0, 16(%0) \n"
            "\tsq $0, 32(%0) \n"
            "\tsq $0, 48(%0) \n" ::"r"(i));
    }
}

int sysLoadELFKeepIOP(const char *filename, const char *partition, int argc, char *argv[])
{
    elfldr_header_t *eh;
    elfldr_pheader_t *eph;
    void *pdata;
    int i, fd;

    (void)partition; // callers pass "" -- no APA-partition context needed on this path

    // argv here is the target's FULL argv -- argv[0] INCLUDED and caller-controlled (Neutrino:
    // its own path; POPSTARTER: the XX./SB. selector it string-parses). At least argv[0] must
    // be supplied: the child forwards &argv[1] verbatim, never synthesizing a replacement.
    if (argc < 1 || argv == NULL || argv[0] == NULL)
        return -1;

    // Probe the target through our still-live mounts so a bad path fails fast in OPL
    // instead of inside the child loader (which can only fall through to OSDSYS).
    if (filename == NULL || (fd = open(filename, O_RDONLY)) < 0)
        return -1;
    close(fd);

    // Kernel args-area budget, the last line of defense for EVERY keep-IOP handoff (Neutrino and
    // POPSTARTER): SetArg copies at most 15 strings into ONE 256-byte pool (NULs included) and
    // ExecPS2 forwards the UNCLAMPED count -- exceeding either limit corrupts rather than
    // truncates. Callers are expected to fit (sysLaunchNeutrino budgets itself); refuse loudly
    // here rather than hand the kernel a mangled argv.
    {
        int pool = (int)strlen(filename) + 1;
        int j;
        for (j = 0; j < argc; j++) {
            if (argv[j] == NULL)
                return -1; // a NULL mid-argv would crash SetArg's copy inside ExecPS2 -- refuse here
            pool += (int)strlen(argv[j]) + 1;
        }
        if (argc + 1 > 15 || pool > 256) {
            LOG("[ELFLDR] argv over the kernel budget (args=%d/15, pool=%d/256) -- refusing handoff\n", argc + 1, pool);
            return -1;
        }
    }

    // Child contract: argv[0] = load path (SifLoadElf'd), argv[1..] = the target's full argv;
    // the ExecPS2 syscall marshals the strings across the jump.
    char *new_argv[argc + 1];
    new_argv[0] = (char *)filename;
    for (i = 0; i < argc; i++)
        new_argv[i + 1] = argv[i];

    wipeBramMem();

    eh = (elfldr_header_t *)elfldr_elf;
    if (_lw((u32)&eh->ident) != ELFLDR_ELF_MAGIC)
        return -1;

    eph = (elfldr_pheader_t *)(elfldr_elf + eh->phoff);
    for (i = 0; i < eh->phnum; i++) {
        if (eph[i].type != ELFLDR_PT_LOAD)
            continue;

        pdata = (void *)(elfldr_elf + eph[i].offset);
        memcpy(eph[i].vaddr, pdata, eph[i].filesz);

        if (eph[i].memsz > eph[i].filesz)
            memset((void *)((u8 *)(eph[i].vaddr) + eph[i].filesz), 0, eph[i].memsz - eph[i].filesz);
    }

    sceSifExitRpc();
    FlushCache(0);
    FlushCache(2);

    return ExecPS2((void *)eh->entry, NULL, argc + 1, new_argv);
}
