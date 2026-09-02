/*
  RetroAchievements unlock notice: a gold pulse over the running game. The
  PC sends RAU1, raudp DMAs a struct ra_event into the buffer below, the
  VBLANK handler plays it with PMODE and BGCOLOR writes only. Why a flash
  and not a picture: lab/overlay/README.md in the control repo. Licenced
  under Academic Free License version 3.0, like the rest of ee_core.
*/

#include "ee_core.h"
#include "coreconfig.h"
#include "ra_overlay.h"
#include "../../modules/network/common/ra_snap.h"

/* GS privileged registers, mapped by the TLB entry in tlb.c. Write-only,
   so nothing here reads one back. */
#define GS_PMODE (*(volatile u64 *)0x12000000)

#define GS_SET_PMODE(en1, en2, mmod, amod, slbg, alp) \
    ((u64)(en1) | ((u64)(en2) << 1) | ((u64)1 << 2) | ((u64)(mmod) << 5) | ((u64)(amod) << 6) | ((u64)(slbg) << 7) | ((u64)(alp) << 8))

/* Circuit 1 blended against BGCOLOR (SLBG 1) with the weight given,
   circuit 2 off. ALP 0xFF is the plain game. */
#define RA_OVL_PMODE(alp) GS_SET_PMODE(1, 0, 1, 0, 1, (alp))

/* 0xBBGGRR, the way the rest of ee_core writes BGCOLOR. */
#define RA_OVL_GOLD  0x20A0FF
#define RA_OVL_BLACK 0x000000

/* The pulse the owner picked: 12 frames down to the game at half weight,
   48 frames back. About a second at 60 Hz. */
#define RA_OVL_DOWN  12
#define RA_OVL_UP    48
#define RA_OVL_FLOOR 0x80

/* Where raudp's events land. A 64-byte line of its own, read through
   UNCACHED_SEG so the DMA'd data is seen and not a stale cache line. */
static struct ra_event ra_ovl_event __attribute__((aligned(64)));

static unsigned int ra_ovl_seen = 0;  /* last event seq acted on */
static unsigned int ra_ovl_start = 0; /* frame the running flash began */
static int ra_ovl_running = 0;

void *RA_OverlayEventBuffer(void)
{
    struct ra_event *e = (struct ra_event *)UNCACHED_SEG(&ra_ovl_event);

    e->magic = 0;
    e->seq = 0;
    e->kind = 0;
    e->arg = 0;

    return &ra_ovl_event;
}

/* No badge buffer in the flash build: raudp sees the zero address and
   keeps the chunks to itself. The badge road -- delivery proven, the
   upload's cost not yet worth it -- is parked in the lab. */
void *RA_OverlayBadgeBuffer(void)
{
    return NULL;
}

/* The GS paints BGCOLOR in the border too, so the colour follows the blend
   curve: full at the deepest point, black at either end. k is 0..256. */
static unsigned int ra_ovl_scale(unsigned int colour, int k)
{
    unsigned int r = (colour & 0xFF) * k >> 8;
    unsigned int g = ((colour >> 8) & 0xFF) * k >> 8;
    unsigned int b = ((colour >> 16) & 0xFF) * k >> 8;

    return r | (g << 8) | (b << 16);
}

void RA_OverlayOnVblank(unsigned int frames)
{
    volatile struct ra_event *e = (volatile struct ra_event *)UNCACHED_SEG(&ra_ovl_event);
    unsigned int phase;
    int alp, k;

    /* A new event restarts the flash, even mid-flash: two unlocks close
       together read as two pulses, not one long one. */
    if (e->magic == RA_EVENT_MAGIC && e->seq != ra_ovl_seen) {
        ra_ovl_seen = e->seq;
        ra_ovl_start = frames;
        ra_ovl_running = 1;
    }

    if (!ra_ovl_running)
        return;

    phase = frames - ra_ovl_start;

    if (phase < RA_OVL_DOWN) {
        alp = 0xFF - (0xFF - RA_OVL_FLOOR) * (int)(phase + 1) / RA_OVL_DOWN;
    } else if (phase < RA_OVL_DOWN + RA_OVL_UP) {
        alp = RA_OVL_FLOOR + (0xFF - RA_OVL_FLOOR) * (int)(phase - RA_OVL_DOWN + 1) / RA_OVL_UP;
    } else {
        /* Last frame: plain game, background back to black. The game
           rewrites PMODE itself on its next flip, so nothing else needs
           restoring. */
        GS_PMODE = RA_OVL_PMODE(0xFF);
        BGCOLND(RA_OVL_BLACK);
        ra_ovl_running = 0;
        return;
    }

    k = (0xFF - alp) * 256 / (0xFF - RA_OVL_FLOOR);
    BGCOLND(ra_ovl_scale(RA_OVL_GOLD, k));
    GS_PMODE = RA_OVL_PMODE(alp);
}
