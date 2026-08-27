/*
  RA: the disc in the tray as a game source.

  OPL has four sources -- USB and friends (BDM), a share (ETH), the
  internal drive (HDD) and homebrew (APP) -- and none of them is the
  optical drive: an original disc is what the console boots by itself,
  so OPL never needed to. Our telemetry, though, lives inside OPL's
  launch: ee_core hooks the game that OPL loaded. A disc started from
  the console's own browser has no hook, no raudp and no achievements.

  What makes a disc work is not new code for reading it -- OPL already
  hands the game the path "cdrom0:\<NAME>;1" (see system.c) -- but
  leaving the drive alone. Before a game starts, OPL reboots the IOP
  with an IOPRP image that replaces the console's CDVDMAN with its own,
  image-reading one. Drop that replacement and cdrom0: is the real
  drive again, while everything else -- ee_core, its IOP modules, the
  patches -- stays exactly as it is. ioprp.c builds that second image.

  Checking the disc against RetroAchievements needs no launch at all:
  in the menu the IOP still runs the console's own CDVDMAN, so the
  hasher reads sectors straight off the drive (rahash.c). That is
  cheaper than an image file, which has to be walked around the 2 GB
  offset problem.

  What the disc mode gives up, all of it living inside OPL's own
  cdvdman: in-game reset (the IGR combo), virtual memory cards, and the
  per-image compatibility patches.
*/

#include <stdio.h>
#include <string.h>

#include "include/opl.h"
#include "include/ioman.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/rahash.h"
#include "include/rawatch.h"
#include "include/ranet.h"
#include "include/gui.h"
#include "include/discsupport.h"

/* Where a watch list received over the network is kept, and where the
   launch looks for one. raAskPC and sbLoadWatchList both fall back to
   the share; and a list that arrived during the check is already in
   memory, so a disc needs no writable device at all. */
#define DISC_WL_PATH "mass0:"

/* sceCdGetDiskType answers SCECdDETCT while the drive is still working
   out what it is holding. Bounded, so a drive that never settles
   reports a failure instead of hanging the menu. */
#define DISC_DETECT_TRIES 2000

static int discWaitReady(char *why, int whysz)
{
    int type, i;

    if (sceCdStatus() == SCECdErOPENS) {
        if (why != NULL)
            snprintf(why, whysz, "The disc tray is open");
        return -1;
    }

    for (i = 0; i < DISC_DETECT_TRIES; i++) {
        type = sceCdGetDiskType();

        if (type != SCECdDETCT && type != SCECdDETCTCD && type != SCECdDETCTDVDS &&
            type != SCECdDETCTDVDD)
            break;
    }

    type = sceCdGetDiskType();
    if (type == SCECdNODISC) {
        if (why != NULL)
            snprintf(why, whysz, "No disc in the drive");
        return -2;
    }

    sceCdDiskReady(0);
    type = sceCdGetDiskType();

    if (type != SCECdPS2DVD && type != SCECdPS2CD && type != SCECdPS2CDDA) {
        if (why != NULL)
            snprintf(why, whysz, "Not a PlayStation 2 disc (type %d)", type);
        return -3;
    }

    LOG("RA: disc ready, type %d\n", type);

    {
        /* Into the hash log: a disc that is not what we assumed is
           otherwise indistinguishable from one we cannot read. */
        char line[48];

        snprintf(line, sizeof(line), "1-disc-ready type=%d", type);
        raHashStep(line);
    }

    return 0;
}

int discGetStartup(char *out, int max, char *why, int whysz)
{
    int ret;

    out[0] = '\0';

    ret = discWaitReady(why, whysz);
    if (ret != 0)
        return ret;

    ret = raDiscBootFile(out, max);
    if (ret != 0) {
        if (why != NULL)
            snprintf(why, whysz, "Could not read SYSTEM.CNF from the disc (%d)", ret);
        return -4;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* "RA: check disc support"                                            */

/* One check at a time, the same rule the image check follows: the
   worker runs on the I/O thread and talks to the network. */
static volatile int disc_check_busy = 0;

static void discCheckWorker(void)
{
    char startup[16], hash[33], why[96], info[96], info2[96];
    int q;

    raHashLogOpen(DISC_WL_PATH);
    raHashSetStepLog(&raHashStep);

    why[0] = '\0';

    if (discGetStartup(startup, sizeof(startup), why, sizeof(why)) != 0) {
        raHashStep("1-no-disc");
        raHashLogAdd("(disc)", "-", why);
        guiShowRANotice(why[0] ? why : "The disc could not be read", NULL);
        goto done;
    }

    if (raHashDisc(startup, hash) != 0) {
        raHashStep("5-disc-hash-failed");
        raHashLogAdd("(disc)", startup, "NOT HASHED");
        guiShowRANotice("The disc could not be hashed", startup);
        goto done;
    }

    raHashLogAdd("(disc)", startup, hash);
    LOG("RA: disc hash %s = %s\n", startup, hash);

    raHashStep("6-asking-pc");
    q = raAskPC(hash, startup, DISC_WL_PATH, info, sizeof(info), info2, sizeof(info2));

    if (q == 0) {
        raHashStep("7-list-received");
        guiShowRANotice(info[0] ? info : "Supported by RetroAchievements",
                        info2[0] ? info2 : NULL);
    } else if (q == 1) {
        raHashStep("7-pc-does-not-know-image");
        guiShowRANotice("RetroAchievements does not know this disc", hash);
    } else if (q == -7) {
        raHashStep("7-pc-still-identifying");
        guiShowRANotice("The PC is still identifying the disc",
                        "Try again in a few seconds");
    } else if (q == -2) {
        raHashStep("7-no-socket-on-console");
        guiShowRANotice("The console could not open a network socket",
                        "Restart the console and try again");
    } else {
        raHashStep("7-pc-did-not-answer");
        guiShowRANotice("The PC client did not answer",
                        "Check that xerabora runs, or try 'RA: test PC connection'");
    }

done:
    raHashSetStepLog(NULL);
    raHashLogClose();
    disc_check_busy = 0;
}

int discCheckSupportDeferred(void)
{
    if (disc_check_busy)
        return 0;

    disc_check_busy = 1;
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &discCheckWorker);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */

void discLaunch(void)
{
    char startup[16], why[96];

    why[0] = '\0';

    if (discGetStartup(startup, sizeof(startup), why, sizeof(why)) != 0) {
        guiShowRANotice(why[0] ? why : "The disc could not be read", NULL);
        return;
    }

    /* The list is usually already in memory, put there by the check
       over the network; the file is only for a console restarted
       mid-game. */
    sbLoadWatchList(DISC_WL_PATH, startup);

    LOG("RA: launching %s from the disc\n", startup);

    /* No cdvdman and no mcemu: cdvdman_irx == NULL is what tells
       sendIrxKernelRAM to build the IOPRP that leaves the drive alone,
       and to ship a standalone DEV9 for the network stack.

       IGR stays ON. The first version passed COMPAT_MODE_6 to avoid the
       IGR shutdown RPC, whose server sits in OPL's cdvdman -- but the
       per-frame telemetry hook, RA_OnVblank, is called from the IGR
       VBLANK handler that Install_IGR() sets up, so that switch also
       silenced the telemetry. padhook.c now skips the RPC itself in
       this mode instead. */
    sysLaunchLoaderElf(startup, "DISC_MODE", 0, NULL, 0, NULL, 0, 0);
}
