/*
  RetroAchievements support inside the running game: a snapshot of the
  watched memory addresses is taken every frame and pushed to the IOP
  over SIF DMA, where the raudp module sends it to the PC client.

  ee_core owns no thread once the game is running. Its only periodic code
  is the VBLANK_END interrupt handler in padhook.c (in-game reset), so
  RA_OnVblank() is called from there. Apart from RA_SetupWatchList(),
  which runs during init, everything here runs in interrupt context: no
  blocking, no waiting on DMA.

  Licenced under Academic Free License version 3.0, like the rest of ee_core.
*/

#include "ee_core.h"
#include "coreconfig.h"
#include "ra.h"
#include <sifdma.h>
#include "../../modules/network/common/ra_snap.h"
#include "../../modules/network/common/ra_watch.h"

/* Interrupt-safe SIF DMA entry points. libkernel.a exports them, but
   sifdma.h does not declare them. */
extern int isceSifSetDma(SifDmaTransfer_t *dmat, int count);
extern int isceSifDmaStat(int trid);

/* Filled in by iopmgr.c while loading the IOP modules. */
extern unsigned int ra_snap_iop; /* snapshot buffer in IOP RAM, 0 if none */
extern int ra_raudp_result;      /* LoadOPLModule() result for raudp */
extern int ra_smap_result;       /* LoadOPLModule() result for SMAP */

/* Frames to wait after the game starts before touching its memory: the
   game must load its own ELF first. About 10 seconds at 60 frames/s. */
#define RA_START_DELAY 600

/* The snapshot is assembled here and DMA'd from here. SIF DMA requires
   64-byte alignment; all writes go through UNCACHED_SEG so the data
   reaches RAM instead of staying in the EE cache. */
static u8 ra_snap_buf[RA_SNAP_TOTAL] __attribute__((aligned(64)));
static int ra_snap_dma_id = 0;
static unsigned int ra_snap_seq = 0;
static unsigned int ra_snap_skip = 0;
static unsigned int ra_snap_fail = 0;
static unsigned int ra_frames = 0;

/* Own copy of the watch list. config->raWatchList points into loader
   memory, which the game overwrites, so the list is copied during init
   the same way SetupCheats() copies the cheat list. */
static u32 ra_watch[RA_WATCH_MAX];
static int ra_watch_count = 0;
static int ra_watch_bytes = 0;

void RA_SetupWatchList(void)
{
    USE_LOCAL_EECORE_CONFIG;
    int i;

    ra_watch_count = 0;
    ra_watch_bytes = 0;

    if (config->raWatchList == NULL || config->raWatchCount <= 0)
        return;
    if (config->raWatchCount > RA_WATCH_MAX)
        return;
    if (config->raSnapBytes <= 0 || config->raSnapBytes > RA_SNAP_MAX_BYTES)
        return;

    for (i = 0; i < config->raWatchCount; i++)
        ra_watch[i] = config->raWatchList[i];

    ra_watch_count = config->raWatchCount;
    ra_watch_bytes = config->raSnapBytes;
}

/* One snapshot per frame.

   If the previous DMA is still in flight the frame is skipped, not
   waited for. The SIF channel is shared with the game (audio, disc,
   pad), and waiting inside an interrupt handler stalls it. */
static void ra_snap_send(void)
{
    USE_LOCAL_EECORE_CONFIG;
    struct ra_snap *s = (struct ra_snap *)UNCACHED_SEG(&ra_snap_buf);
    u8 *vals = (u8 *)UNCACHED_SEG(&ra_snap_buf[RA_SNAP_HDR]);
    SifDmaTransfer_t dmat;
    int i, off = 0;

    if (ra_snap_iop == 0 || ra_watch_count == 0)
        return;

    if (ra_snap_dma_id != 0 && isceSifDmaStat(ra_snap_dma_id) >= 0) {
        ra_snap_skip++;
        return;
    }

    ra_snap_seq++;

    s->magic = RA_SNAP_MAGIC;
    s->seq = ra_snap_seq;
    s->frames = ra_frames;
    s->dma_skip = ra_snap_skip;
    s->dma_fail = ra_snap_fail;
    s->count = (unsigned int)ra_watch_count;
    s->bytes = (unsigned int)ra_watch_bytes;

    for (i = 0; i < (int)sizeof(s->game_id); i++)
        s->game_id[i] = config->GameID[i];

    /* Values are read in list order and packed back to back, little
       endian. Reads go through UNCACHED_SEG, which returns RAM: a value
       the game wrote moments ago may still sit dirty in the EE data
       cache, so a reading can lag by the time it takes that line to be
       written back, usually well under a frame in a running game.
       Cached reads would see it at once but would pull up to a thousand
       cache lines per frame through the game's 8 KB data cache. */
    for (i = 0; i < ra_watch_count && off < ra_watch_bytes; i++) {
        u32 e = ra_watch[i];
        u32 addr = RA_WATCH_ADDR(e);
        u32 size = RA_WATCH_SIZE(e);

        if (size == 4) {
            u32 v = *(volatile u32 *)UNCACHED_SEG(addr);

            vals[off++] = (u8)v;
            vals[off++] = (u8)(v >> 8);
            vals[off++] = (u8)(v >> 16);
            vals[off++] = (u8)(v >> 24);
        } else if (size == 2) {
            u16 v = *(volatile u16 *)UNCACHED_SEG(addr);

            vals[off++] = (u8)v;
            vals[off++] = (u8)(v >> 8);
        } else {
            vals[off++] = *(volatile u8 *)UNCACHED_SEG(addr);
        }
    }

    s->seq_end = ra_snap_seq;

    /* Trailer after the values: the DMA copies front to back, so this is
       the last word to land on the IOP. raudp compares it with the
       header's seq to detect a snapshot overwritten mid-copy. */
    *(volatile u32 *)UNCACHED_SEG(&ra_snap_buf[RA_SNAP_TRAILER_OFF(ra_watch_bytes)]) = ra_snap_seq;

    dmat.src = (void *)&ra_snap_buf;
    dmat.dest = (void *)ra_snap_iop;
    dmat.size = RA_SNAP_DMA_SIZE(ra_watch_bytes);
    dmat.attr = 0;

    ra_snap_dma_id = isceSifSetDma(&dmat, 1);
    if (ra_snap_dma_id == 0)
        ra_snap_fail++;
}

#ifdef RA_DEBUG
/* Debug HUD, built only with RA_DEBUG=1.

   Inside the game there is no console and no network until raudp is up,
   so the only way to see whether the IOP modules loaded is to write a
   number into memory the game already displays. This writes into the
   money counter of Need for Speed: Underground 2 (SLUS_210.65), the
   title used during development; on any other game it writes into an
   unrelated address and shows nothing. The cheat engine must be off,
   otherwise it overwrites the same field.

   Reading the number, alternating every ~3 s:
     7 0xx xxx  SMAP loaded, xxx = return code
     8 0xx xxx  SMAP failed, xxx = error code
     5 0xx xxx  raudp loaded
     6 0xx xxx  raudp failed
     x 000 999  the loader never reached that module */
#define RA_PROBE_ADDR      0x004F9A64
#define RA_PROBE_SWAP      180
#define RA_PROBE_MOD_OK    5000000
#define RA_PROBE_MOD_FAIL  6000000
#define RA_PROBE_SMAP_OK   7000000
#define RA_PROBE_SMAP_FAIL 8000000

static void ra_debug_hud(void)
{
    u32 out;

    if (((ra_frames / RA_PROBE_SWAP) & 1) == 0) {
        out = (ra_smap_result >= 1) ? (RA_PROBE_SMAP_OK + (u32)ra_smap_result) : (RA_PROBE_SMAP_FAIL + (u32)(-ra_smap_result));
    } else {
        out = (ra_raudp_result >= 1) ? (RA_PROBE_MOD_OK + (u32)ra_raudp_result) : (RA_PROBE_MOD_FAIL + (u32)(-ra_raudp_result));
    }

    *(volatile u32 *)UNCACHED_SEG(RA_PROBE_ADDR) = out;
}
#endif

void RA_OnVblank(void)
{
    ra_frames++;
    if (ra_frames <= RA_START_DELAY)
        return;

#ifdef RA_DEBUG
    ra_debug_hud();
#endif

    ra_snap_send();
}
