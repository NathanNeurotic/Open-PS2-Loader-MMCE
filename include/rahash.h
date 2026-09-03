/* RA: the image hash, computed the way RetroAchievements does. See src/rahash.c */
#ifndef __RAHASH_H__
#define __RAHASH_H__

/* Where to report hashing steps. A hang partway through a 2 GB image on USB is
   otherwise completely silent, and the return code alone cannot tell "could not
   open" from "could not read". */
typedef void (*ra_step_fn)(const char *what);
void raHashSetStepLog(ra_step_fn fn);

/* Hashes the boot executable of an image WITHOUT mounting it: walks ISO9660
   with 64-bit offsets, because a mounted image on USB hangs when reading files
   beyond the 2 GB mark. out33 receives 32 hex digits plus the terminator.
   Returns 0 on success. */
int raHashIsoDirect(const char *isopath, const char *startup, char *out33);

#endif
