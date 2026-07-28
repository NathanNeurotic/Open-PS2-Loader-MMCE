#ifndef __SYSTEM_H
#define __SYSTEM_H

#include "include/mcemu.h"

#define NEUTRINO_PATH     "mc0:NEUTRINO/neutrino.elf"
#define NEUTRINO_ALT_PATH "mc1:NEUTRINO/neutrino.elf"

#define POPS_FOLDER "POPS" // per-device PS1/POPSTARTER folder: <dev>/POPS/POPSTARTER.ELF + *.VCD

#define SYS_LOAD_MC_MODULES   0x01
#define SYS_LOAD_USB_MODULES  0x02
#define SYS_LOAD_ISOFS_MODULE 0x04

unsigned int USBA_crc32(const char *string);
int sysGetDiscID(char *discID);
void sysInitDev9(void);
void sysShutdownDev9(void);
void sysReset();
void sysExecExit(void);
int sysLaunchDisc(void); // boot the physical PS2 disc in the drive; <0 (stays in OPL) on failure
void sysPowerOff(void);
#ifdef __DECI2_DEBUG
int sysInitDECI2(void);
#endif

void sysLaunchLoaderElf(const char *filename, const char *mode_str, int size_cdvdman_irx, void **cdvdman_irx, int size_mcemu_irx, void **mcemu_irx, int EnablePS2Logo, unsigned int compatflags);

#define NEUTRINO_VMC_SLOTS 2

// Fully-formed Neutrino "-mcN=<prefix>VMC/<name>.bin" args, one per VMC slot, resolved by the
// device support layer (sbBuildVmcNeutrinoArgs) BEFORE deinit frees the per-game config. Carried
// into sysLaunchNeutrino and emitted as DISCRETE argv[] entries -- never whitespace-tokenized -- so
// a VMC whose name contains a space survives intact (issue #47: VMC not mounting under Neutrino).
typedef struct neutrino_vmc_args
{
    // "-mcN=<prefix>VMC/<name>.bin"; "" => slot unconfigured / skipped. Sized for the worst case:
    // "-mcN=" + bdmPrefix (BDM_PREFIX_MAX=96) + "VMC/" + 31-char VMC name + ".bin" + NUL = ~140.
    char arg[NEUTRINO_VMC_SLOTS][160];
} neutrino_vmc_args_t;

// neutrinoBsdfs: per-game -bsdfs override (parity-audit #11), 0=Auto (per-device default),
// 1=exfat, 2=hdl (-dvd=hdl:), 3=bd (-dvd=bdfs:). Block-backed devices only; ignored (never
// emitted) for mmce/udpfs, and inert on APA which is always -bsdfs=hdl.
void sysLaunchNeutrino(const char *driver, const char *path, const char *startup, int compatmask, int EnablePS2Logo, const char *neutrinoPath, const char *extraArgs, int neutrinoVideo, int neutrinoGsmComp, int neutrinoBsdfs, const neutrino_vmc_args_t *vmcArgs);

// D6 pre-deinit launch pre-flight: validates the driver token and (network transports) syncs the
// bsd toml ip= while every mount is up and the GUI can still toast. Call BEFORE deinitEx in every
// Neutrino leg. Returns 0 = proceed; <0 = abort the launch (a toast has already been shown).
int sysNeutrinoPreflight(const char *driver, const char *neutrinoPath);

// Launch an external POPSTARTER.ELF for a PS1 VCD. selector = the target's argv[0]
// "<POPS>/<prefix><name>.ELF" token. Caller deinit()s with UNMOUNT_EXCEPTION first (see system.c).
void sysLaunchPopstarter(const char *popstarterElf, const char *selector);

// ELF handoff that KEEPS the IOP (drivers + mounts) alive -- NHDDL parity: the vendored elfldr/
// child loader SifLoadElf()s the target through OPL's live mounts and never SifIopReset()s (the
// target resets the IOP itself). argv is the target's FULL argv, argv[0] INCLUDED and
// caller-controlled -- unlike LoadELFFromFileWithPartition, which clobbers the target's argv[0]
// with "<partition><filename>" (that clobber is what broke POPSTARTER's XX./SB. selector).
// Container-independent: works the same on ps2dev:latest and the PS2DEVPINNEDSDK pin. Returns only on
// failure (bad path/ELF). Implemented in elfldr_noreset.c.
int sysLoadELFKeepIOP(const char *filename, const char *partition, int argc, char *argv[]);

int sysExecElf(const char *path);
int sysLoadModuleBuffer(void *buffer, int size, int argc, char *argv);
int sysCheckMC(void);
int sysCheckVMC(const char *prefix, const char *sep, char *name, int createSize, vmc_superblock_t *vmc_superblock);

#endif
