/* RetroAchievements: talking to the PC client from the OPL menu. See src/ranet.c */
#ifndef __RANET_H__
#define __RANET_H__

/* Hashes are known; ask the PC for the watch list and store it next to
   the game. Returns 0 when the list was received, 1 when RetroAchievements
   does not know the image, -7 when the PC was still identifying it after
   the waiting rounds, -8 when the NIC is owned by a UDP transport and the
   ask was refused without touching the network, another negative value when
   the PC did not answer or the transfer broke.
   info (may be NULL) receives the game title, info2 (may be NULL) the
   achievement counts as the PC reported them, e.g. info "Need for Speed:
   Underground 2" and info2 "76 achievements: 3 unlocked, 10 unsupported".
   Older PC clients report only the total, so info2 is then just
   "76 achievements". */
int raAskPC(const char *hash, const char *serial, const char *savepath,
            char *info, int infosz, char *info2, int info2sz);

/* 1 when a UDP transport (UDPBD, or the UDPFS filesystem chain) owns the
   SMAP NIC. RA must not raise the SMB stack on top of it; the menu actions
   refuse with a notice instead. */
int raNetNicBusy(void);

/* Broadcasts a discovery request and reports the outcome as two lines
   of text for the notice popup. Returns 1 when a PC client answered. */
int raNetTestLink(char *line1, int sz1, char *line2, int sz2);

#endif
