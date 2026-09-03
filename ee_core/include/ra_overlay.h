/*
  RetroAchievements unlock notice over the running game; see
  src/ra_overlay.c.
*/
#ifndef RA_OVERLAY_H
#define RA_OVERLAY_H

/* Where raudp DMAs events to (struct ra_event, 16 bytes, in a 64-byte
   line of its own). iopmgr.c passes this address to the module. */
void *RA_OverlayEventBuffer(void);

/* Where raudp DMAs the badge to (struct ra_badge: a 16-byte header and
   8 KB of PSMCT16 pixels). Passed to the module the same way. */
void *RA_OverlayBadgeBuffer(void);

/* Per-frame hook, called from RA_OnVblank() with the frame counter that
   ra.c keeps. Picks up new events and runs the notice. Register writes
   only. */
void RA_OverlayOnVblank(unsigned int frames);

#endif
