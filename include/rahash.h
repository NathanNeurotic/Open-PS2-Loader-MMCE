/* RA: the image hash, computed the way RetroAchievements does. See src/rahash.c */
#ifndef __RAHASH_H__
#define __RAHASH_H__

/* Where to report hashing steps: a hang on USB is otherwise silent */
typedef void (*ra_step_fn)(const char *what);
void raHashSetStepLog(ra_step_fn fn);

/* Wrapper over the static GetStartupExecName in supportbase.c. */
int raGetStartupName(const char *cnfpath, char *out, int max);

/* Hashes the boot executable of an image without mounting it: walks
   ISO9660 with 64-bit offsets, because a mounted image on USB hangs
   when reading files beyond the 2 GB mark. out33 receives 32 hex
   digits plus the terminator. Returns 0 on success. */
int raHashIsoDirect(const char *isopath, const char *startup, char *out33);

/* RA: the same hash, taken off the disc in the tray instead of an
   image file. Only valid from the menu, where the IOP still runs the
   console's own CDVDMAN and cdrom0: is the real drive. */
int raHashDisc(const char *startup, char *out33);

/* RA: reads BOOT2 out of the disc's own SYSTEM.CNF and returns the
   boot executable name, e.g. "SLUS_210.65". Returns 0 on success. */
int raDiscBootFile(char *out, int max);

#endif
