/* RA: the disc in the tray as a game source. See src/discsupport.c */
#ifndef __DISCSUPPORT_H__
#define __DISCSUPPORT_H__

/* Reads the boot executable name off the disc, e.g. "SLUS_210.65".
   Returns 0 on success; why (may be NULL) receives a line for the
   notice popup when it fails. */
int discGetStartup(char *out, int max, char *why, int whysz);

/* Hashes the disc and asks the PC client whether RetroAchievements
   knows it, exactly as "RA: check game support" does for an image.
   Runs on the I/O thread. Returns 0 when a check is already running. */
int discCheckSupportDeferred(void);

/* Boots the disc under OPL's ee_core, with no CDVD emulation, so
   telemetry and achievements work off the real drive. Does not
   return when it succeeds. */
void discLaunch(void);

#endif
