/*
 Copyright 2022, Thanks to SP193
 Licenced under Academic Free License version 3.0
 Review OpenPS2Loader README & LICENSE files for further details.
 */

#include <audsrv.h>
#include <delaythread.h> // DelayThread -- bounded drain in sfxDispatchQuiesce
#include <timer.h>       // cpu_ticks -- SFX RPC wall-time measurement
#include <vorbis/vorbisfile.h>

#include "include/sound.h"
#include "include/opl.h"
#include "include/ioman.h"
#include "include/themes.h"
#include "include/pad.h" // padRumble -- menu rumble rides the SFX events

// Silence unused variable warnings from vorbisfile.h
static ov_callbacks OV_CALLBACKS_NOCLOSE __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY_NOCLOSE __attribute__((unused));

/*--    Theme Sound Effects    ----------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

extern unsigned char boot_adp[];
extern unsigned int size_boot_adp;

extern unsigned char cancel_adp[];
extern unsigned int size_cancel_adp;

extern unsigned char confirm_adp[];
extern unsigned int size_confirm_adp;

extern unsigned char cursor_adp[];
extern unsigned int size_cursor_adp;

extern unsigned char message_adp[];
extern unsigned int size_message_adp;

extern unsigned char transition_adp[];
extern unsigned int size_transition_adp;

extern unsigned char bd_connect_adp[];
extern unsigned int size_bd_connect_adp;

extern unsigned char bd_disconnect_adp[];
extern unsigned int size_bd_disconnect_adp;

struct sfxEffect
{
    const char *name;
    void *buffer;
    int size;
    int builtin;
    int duration_ms;
};

static struct sfxEffect sfx_files[SFX_COUNT] = {
    {"boot.adp"},
    {"cancel.adp"},
    {"confirm.adp"},
    {"cursor.adp"},
    {"message.adp"},
    {"transition.adp"},
    {"bd_connect.adp"},
    {"bd_disconnect.adp"},
};

static struct audsrv_adpcm_t sfx[SFX_COUNT];
static int audio_initialized = 0;

// Returns 0 if the specified file was read. The sfxEffect structure will not be updated unless the file is successfully read.
static int sfxRead(const char *full_path, struct sfxEffect *sfx)
{
    int adpcm;
    void *buffer;
    int ret, size;

    LOG("SFX: sfxRead('%s')\n", full_path);

    adpcm = open(full_path, O_RDONLY);
    if (adpcm < 0) {
        LOG("SFX: %s: Failed to open adpcm file %s\n", __FUNCTION__, full_path);
        return -ENOENT;
    }

    size = lseek(adpcm, 0, SEEK_END);
    lseek(adpcm, 0L, SEEK_SET);

    buffer = memalign(64, size);
    if (buffer == NULL) {
        LOG("SFX: Failed to allocate memory for SFX\n");
        close(adpcm);
        return -ENOMEM;
    }

    ret = read(adpcm, buffer, size);
    close(adpcm);

    if (ret != size) {
        LOG("SFX: Failed to read SFX: %d (expected %d)\n", ret, size);
        free(buffer);
        return -EIO;
    }

    sfx->buffer = buffer;
    sfx->size = size;
    sfx->builtin = 0;

    return 0;
}

static int sfxCalculateSoundDuration(int nSamples)
{
    float sampleRate = 44100; // 44.1kHz

    // Return duration in milliseconds
    return (nSamples / sampleRate) * 1000;
}

static void sfxInitDefaults(void)
{
    int i;

    for (i = 0; i < SFX_COUNT; i++)
        sfx_files[i].builtin = 1;

    sfx_files[SFX_BOOT].buffer = boot_adp;
    sfx_files[SFX_BOOT].size = size_boot_adp;
    sfx_files[SFX_CANCEL].buffer = cancel_adp;
    sfx_files[SFX_CANCEL].size = size_cancel_adp;
    sfx_files[SFX_CONFIRM].buffer = confirm_adp;
    sfx_files[SFX_CONFIRM].size = size_confirm_adp;
    sfx_files[SFX_CURSOR].buffer = cursor_adp;
    sfx_files[SFX_CURSOR].size = size_cursor_adp;
    sfx_files[SFX_MESSAGE].buffer = message_adp;
    sfx_files[SFX_MESSAGE].size = size_message_adp;
    sfx_files[SFX_TRANSITION].buffer = transition_adp;
    sfx_files[SFX_TRANSITION].size = size_transition_adp;
    sfx_files[SFX_BD_CONNECT].buffer = bd_connect_adp;
    sfx_files[SFX_BD_CONNECT].size = size_bd_connect_adp;
    sfx_files[SFX_BD_DISCONNECT].buffer = bd_disconnect_adp;
    sfx_files[SFX_BD_DISCONNECT].size = size_bd_disconnect_adp;
}

// Returns 0 (AUDSRV_ERR_NOERROR) if the sound was loaded successfully.
static int sfxLoad(struct sfxEffect *sfxData, audsrv_adpcm_t *sfx)
{
    int ret;

    // Calculate duration based on number of samples
    sfxData->duration_ms = sfxCalculateSoundDuration(((u32 *)sfxData->buffer)[3]);
    // Estimate duration based on filesize, if the ADPCM header was 0
    if (sfxData->duration_ms == 0)
        sfxData->duration_ms = sfxData->size / 47;

    ret = audsrv_load_adpcm(sfx, sfxData->buffer, sfxData->size);
    if (sfxData->builtin == 0) {
        free(sfxData->buffer);
        sfxData->buffer = NULL; // Mark the buffer as freed.
    }

    return ret;
}

// SFX dispatch thread plumbing (defined below sfxInit; see the block comment there).
static void sfxDispatchQuiesce(void);
static volatile int sfxDispatchPaused;

// Returns number of audio files successfully loaded, < 0 if an unrecoverable error occurred.
int sfxInit(int bootSnd)
{
    char sound_path[256];
    char full_path[256];
    int ret, loaded;
    int thmSfxEnabled = 0;
    int i = 1;

    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return -1;
    }

    // This function rewrites sfx[] and resets the IOP ADPCM bank (theme reload path): park the
    // dispatch thread first so no queued entry can hand audsrv a half-reloaded sample (#340).
    sfxDispatchQuiesce();

    audsrv_adpcm_init();
    sfxInitDefaults();
    audioSetVolume();

    // Check default theme is not current theme
    int themeID = thmGetGuiValue();
    if (themeID != 0) {
        // Get theme path for sfx
        char *thmPath = thmGetFilePath(themeID);
        snprintf(sound_path, sizeof(sound_path), "%ssound", thmPath);

        // Check for custom sfx folder
        DIR *dir = opendir(sound_path);
        if (dir != NULL) {
            thmSfxEnabled = 1;
            closedir(dir);
        }
    }

    loaded = 0;
    i = bootSnd ? 0 : 1;
    for (; i < SFX_COUNT; i++) {
        if (thmSfxEnabled) {
            snprintf(full_path, sizeof(full_path), "%s/%s", sound_path, sfx_files[i].name);
            ret = sfxRead(full_path, &sfx_files[i]);
            if (ret != 0) {
                LOG("SFX: %s could not be loaded. Using default sound %d.\n", full_path, ret);
            }
        } else
            snprintf(full_path, sizeof(full_path), "builtin/%s", sfx_files[i].name);

        ret = sfxLoad(&sfx_files[i], &sfx[i]);
        if (ret == 0) {
            LOG("SFX: Loaded %s, size=%d, duration=%dms\n", full_path, sfx_files[i].size, sfx_files[i].duration_ms);
            loaded++;
        } else {
            LOG("SFX: failed to load %s, error %d\n", full_path, ret);
        }
    }

    sfxDispatchPaused = 0; // sfx[] rewrite complete: re-arm the dispatch path

    return loaded;
}

int sfxGetSoundDuration(int id)
{
    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return 0;
    }

    return sfx_files[id].duration_ms;
}

// Per-press audsrv RPC wall time. audsrv_ch_play_adpcm is a SYNCHRONOUS SIF RPC; under IOP
// contention it stalled the menu mid-navigation on real hardware (#340: 352 ms measured).
// Always measured (two tick reads); surfaced later by the Debug HUD (checklist item 46).
#define SFX_CLOCKS_PER_MS 147456 // EE cpu_ticks() rate
static unsigned int sfxLastPlayMs = 0;
static unsigned int sfxMaxPlayMs = 0;

void sfxGetPlayDiag(unsigned int *lastMs, unsigned int *maxMs)
{
    if (lastMs)
        *lastMs = sfxLastPlayMs;
    if (maxMs)
        *maxMs = sfxMaxPlayMs;
}

// Silent-drop counters (#364). A dropped sound is invisible to the user and was invisible to every
// instrument until now. stale = cursor ticks aged out at dispatch (climbs while scrolling under
// load; harmless by design). full = ANY sound discarded because the 8-deep ring was saturated --
// that is the path that eats deliberate presses. Read against SP (the per-RPC wall time above) the
// pair splits the two possible causes of cut sound: big SP with flat SX says the IOP side is slow,
// small SP with climbing SX/full says the dispatcher is being starved of CPU time.
static unsigned int sfxDroppedStale = 0;
static unsigned int sfxDroppedFull = 0;

void sfxGetDropDiag(unsigned int *staleDrops, unsigned int *fullDrops)
{
    if (staleDrops)
        *staleDrops = sfxDroppedStale;
    if (fullDrops)
        *fullDrops = sfxDroppedFull;
}

// ---- SFX dispatch thread (#340) -------------------------------------------------------------------
// audsrv_ch_play_adpcm is a synchronous SIF RPC, and hardware photos measured a single cursor-tick
// RPC blocking the GUI thread for 352 ms under IOP contention. Sounds are queued to a small
// dedicated thread instead, so no caller ever waits on the IOP for a sound effect; the RPC wall
// time is still measured (on the dispatch thread) into the same diag fields.
//
// TWO producers exist: the GUI thread (prio 31) and the ioman worker (prio 30 -- e.g. device
// connect/disconnect sounds from the deferred menu update), and the worker PREEMPTS the GUI, so
// the enqueue's slot claim is bracketed with DIntr/EIntr (EE thread preemption requires an
// interrupt, making that a complete guard). The thread and semaphore are created ONCE from
// audioInit, which happens-before any producer (both producers gate on audio_initialized).
// A full ring DROPS the sound -- a skipped tick beats a stalled menu. Concurrent audsrv RPCs
// from a second thread are nothing new: bgmThread has always streamed alongside sfx.
#define SFX_QUEUE_LEN   8
// Entries older than this play no more -- CURSOR TICKS ONLY. A tick is a side effect of movement,
// not a press: after an IOP stall clears, replaying the stacked backlog would chirp back-to-back
// describing where the cursor USED to be, so an aged tick is still dropped.
// Deliberate presses are NOT aged out. #364 (zackcage6, confirmed on run 31710438197): "the bgm/
// sound effects of opening/closing or confirming/denying actions will be cut when there's no
// window of cooldown (~5 seconds)". dia.c fires SFX_CONFIRM on BOTH focus-gain and focus-loss, so
// a menu open/close is two CONFIRMs; whenever the dispatcher fell >500 ms behind (a slow audsrv
// RPC under load, or a saturated ring ahead of it), the stale test silently ate real presses --
// the exact fault the enqueue comment below calls worse than a late chirp. The fork has no queue
// at all and every press sounds (synchronously: the menu waits on the RPC). We keep the queue
// that fixed the measured 352 ms #340 menu stall, but stop discarding what the user asked for.
// Drops are counted (sfxDroppedStale/sfxDroppedFull) and surfaced on the debug HUD as SX, next to
// the RPC wall time SP, so the next report about cut sound comes with numbers attached.
#define SFX_STALE_TICKS (100 * SFX_CLOCKS_PER_MS)
static struct
{
    int channel;
    int id;
    u32 ticks;
} sfxQueue[SFX_QUEUE_LEN];
static volatile int sfxQHead = 0;        // advanced by producers (GUI + ioman worker), DIntr-guarded
static volatile int sfxQTail = 0;        // advanced by the consumer only
static volatile int sfxDispatchBusy = 0; // consumer is inside the audsrv RPC
static int sfxDispatchSema = -1;
static int sfxDispatchTid = -1;
static u8 sfxDispatchStack[8 * 1024] __attribute__((aligned(16)));
extern void *_gp;

static void sfxDispatchThread(void *arg)
{
    (void)arg;

    while (1) {
        WaitSema(sfxDispatchSema);
        while (sfxQTail != sfxQHead) {
            int channel = sfxQueue[sfxQTail].channel;
            int id = sfxQueue[sfxQTail].id;
            u32 ticks = sfxQueue[sfxQTail].ticks;
            sfxQTail = (sfxQTail + 1) % SFX_QUEUE_LEN;

            // busy goes up BEFORE the gates: the quiescers set paused and then wait for
            // (empty && !busy), so an entry that passed the gates is always covered by busy.
            sfxDispatchBusy = 1;
            // Only CURSOR ages out (see the define above). CONFIRM/CANCEL/TRANSITION/MESSAGE are
            // deliberate presses and play even when the dispatcher fell behind (#364).
            int stale = (id == SFX_CURSOR) && (u32)(cpu_ticks() - ticks) > SFX_STALE_TICKS;
            if (sfxDispatchPaused || !audio_initialized || stale) {
                if (stale)
                    sfxDroppedStale++;
                sfxDispatchBusy = 0;
                continue;
            }

            u32 start = cpu_ticks();
            audsrv_ch_play_adpcm(channel, &sfx[id]);
            unsigned int costMs = (unsigned int)((cpu_ticks() - start) / SFX_CLOCKS_PER_MS);
            sfxLastPlayMs = costMs;
            if (costMs > sfxMaxPlayMs)
                sfxMaxPlayMs = costMs;
            sfxDispatchBusy = 0;
        }
    }
}

// Called from audioInit BEFORE audio_initialized flips on -- creation happens-before any
// producer, so a lazy double-create race cannot exist. On failure sfxPlay falls back to the
// old synchronous RPC.
static void sfxDispatchStart(void)
{
    if (sfxDispatchTid >= 0)
        return;

    ee_sema_t sema;
    sema.init_count = 0;
    sema.max_count = SFX_QUEUE_LEN;
    sema.attr = 0;
    sema.option = 0;
    sfxDispatchSema = CreateSema(&sema);
    if (sfxDispatchSema < 0)
        return;

    ee_thread_t thread;
    thread.func = &sfxDispatchThread;
    thread.stack = sfxDispatchStack;
    thread.stack_size = sizeof(sfxDispatchStack);
    thread.gp_reg = &_gp;
    thread.initial_priority = 45; // below the GUI thread (31) and IO worker (30): sound yields to input
    thread.attr = 0;
    thread.option = 0;
    sfxDispatchTid = CreateThread(&thread);
    if (sfxDispatchTid < 0) {
        DeleteSema(sfxDispatchSema);
        sfxDispatchSema = -1;
        return;
    }
    StartThread(sfxDispatchTid, NULL);
}

// Park the consumer: no RPC is in flight when this returns. Required before sfx[] is rewritten
// (sfxInit theme reload -- a queued entry playing a half-reloaded sample would hand audsrv a torn
// descriptor) and before audsrv_quit (audioEnd -- an RPC crossing the quit wedges the thread
// forever). Bounded; the prio-45 thread drains during the yields. The caller re-arms with
// sfxDispatchPaused = 0 once its rewrite is complete.
static void sfxDispatchQuiesce(void)
{
    int spins;

    if (sfxDispatchTid < 0)
        return;
    sfxDispatchPaused = 1;
    SignalSema(sfxDispatchSema); // wake it so queued entries drain (unplayed)
    for (spins = 0; spins < 500 && (sfxQTail != sfxQHead || sfxDispatchBusy); spins++)
        DelayThread(1000);
}

static void sfxEnqueue(int channel, int id)
{
    if (sfxDispatchPaused)
        return;

    // Two producers on different priorities: claim the slot atomically.
    DIntr();

    // NOTHING IS COALESCED HERE. An earlier revision dropped a CONFIRM/CANCEL whenever an identical
    // one was still pending in the ring, reasoning that only one can be heard anyway (retriggering a
    // channel replaces the sample already playing). That is true of the AUDIO and false of the USER:
    // dia.c plays SFX_CONFIRM on BOTH focus-gain and focus-loss, so open/close/open/close is four
    // deliberate presses of the same id in quick succession, and the pending-entry test silenced
    // every other one. Reported from hardware as "I click open, it opens, I click close, it closes;
    // when I REOPEN it is completely silent, and when I reclose it sounds normally."
    // Dropping a sound the user asked for is a worse fault than a chirp arriving late.
    // The backlog problem that motivated coalescing is handled where it belongs -- at DISPATCH --
    // and ONLY for cursor ticks: aging CONFIRM/CANCEL out at dispatch was #364 (see the
    // SFX_STALE_TICKS comment), so deliberate presses carry no stale limit at all.

    // ...with ONE exception, and the reasoning above is exactly why it is safe: SFX_CURSOR is not a
    // press, it is a side effect of movement. Holding a direction fires it every step, so a queue
    // full of cursor ticks is a queue of sounds that describe where the cursor USED to be -- and
    // each one that survives to dispatch is another audsrv RPC to the same IOP the game device is
    // being read through. A cursor tick is worth playing NOW or not at all. So when one is already
    // pending, refresh its timestamp instead of adding another: the user still hears a tick for the
    // movement they just made (the pending entry plays, and it is no longer stale), and the ring
    // never fills with obsolete ones. CONFIRM/CANCEL keep their every-press guarantee untouched --
    // they are deliberate presses, and silencing an alternate one was the hardware fault that
    // removed blanket coalescing in the first place.
    if (id == SFX_CURSOR) {
        int scan = sfxQTail;
        while (scan != sfxQHead) {
            if (sfxQueue[scan].id == SFX_CURSOR) {
                sfxQueue[scan].ticks = cpu_ticks(); // keep it fresh; do not queue a second one
                EIntr();
                return;
            }
            scan = (scan + 1) % SFX_QUEUE_LEN;
        }
    }

    int next = (sfxQHead + 1) % SFX_QUEUE_LEN;
    if (next == sfxQTail) {
        // Ring full under IOP congestion: drop the sound, never the frame -- but COUNT it (#364),
        // because this is the path that discards deliberate presses when the IOP wedges for seconds.
        sfxDroppedFull++;
        EIntr();
        return;
    }
    sfxQueue[sfxQHead].channel = channel;
    sfxQueue[sfxQHead].id = id;
    sfxQueue[sfxQHead].ticks = cpu_ticks();
    sfxQHead = next;
    EIntr();
    SignalSema(sfxDispatchSema);
}

// Menu rumble rides the SFX events: one hook covers every call site in the GUI, and the events are
// already the ones worth feeding back on.
//
// SFX_CURSOR IS the menu rumble -- the fork's own note reads "menu rumble mirrors the cursor tick".
// Moving through the menu thumping is what a user means by "vibration"; without it the feature reads
// as broken, which is exactly how it was reported ("vibration doesn't work... it worked fine on our
// fork").
//
// It was left out of this table on the theory that a per-step actuator write would add SIO2 traffic
// to the very navigation path #340 was about. That premise was wrong twice over: padSetActDirect is
// a SIF RPC that latches six bytes on the IOP and adds ZERO SIO2 traffic (the fork's own analysis),
// and #340 turned out to be an SD-card driver left resident on SIO2 by the boot resolver
// (rebuild-135) -- nothing to do with rumble.
//
// SFX_BOOT and the SFX_BD_* device events stay out: nobody is holding the pad at boot, and a drive
// appearing is not something the user did.
//
// SFX_BOOT and the SFX_BD_* device events are also left out: nobody is holding the pad yet at boot,
// and a drive appearing is not something the user did.
static void sfxRumble(int id)
{
    if (!gEnableRumble)
        return;

    switch (id) {
        // The fork's tuned values, byte for byte: a 240 ms nav thump at 0x50 -- long enough for the
        // ERM to actually reach amplitude, which a shorter pulse never does -- and a firmer, shorter
        // 110 ms bump at 0x60 for a decision, so confirming feels more definite than scrolling.
        case SFX_CURSOR:
            padRumble(240, 0x50);
            break;
        case SFX_CONFIRM:
        case SFX_CANCEL:
        case SFX_MESSAGE:
            padRumble(110, 0x60);
            break;
        case SFX_TRANSITION:
            padRumble(150, 200);
            break;
        default:
            break;
    }
}

void sfxPlay(int id)
{
    // ABOVE the audio gates on purpose: someone running with sound off should still get the haptics
    // they asked for, and rumble does not need audsrv.
    sfxRumble(id);

    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (gEnableSFX) {
        // Issued from the dispatch thread so the caller never waits on the IOP for a sound
        // (#340). Synchronous fallback only if the thread could not be created at audioInit.
        if (sfxDispatchTid >= 0) {
            sfxEnqueue(id, id);
        } else {
            u32 start = cpu_ticks();
            audsrv_ch_play_adpcm(id, &sfx[id]);
            unsigned int costMs = (unsigned int)((cpu_ticks() - start) / SFX_CLOCKS_PER_MS);
            sfxLastPlayMs = costMs;
            if (costMs > sfxMaxPlayMs)
                sfxMaxPlayMs = costMs;
        }
    }
}

/*--    Theme Background Music    -------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

// Ring DEPTH is the only defence this decoder has against being preempted, and 16 slots was not
// enough (#364: "slight stutter every time I scroll through the game list one by one").
//
// The arithmetic: 4096-byte slots of 44.1 kHz 16-bit stereo = 176400 bytes/sec, so 16 slots is 64 KB
// ~= 372 ms of audio. bgmIoThread does a BLOCKING ov_read off the theme's sound/bgm.ogg on the game
// device, and both BGM threads run at 0x40/0x41 -- far below the ioman worker (30) and the GUI (31),
// since on the EE a LOWER number wins. Scrolling queues a cover per row onto that same worker, so
// each scroll step can hold the decoder off; any stall longer than the cushion is an audible gap,
// and on a slow device one cover read comfortably exceeds 372 ms. That is also why the reporter
// still heard it after re-encoding the music at a lower bitrate -- the cushion is measured in BYTES
// of decoded PCM, so a smaller source file buys nothing.
//
// 48 slots = 192 KB ~= 1.11 s, for +128 KB of BSS. Both semaphores already take their max_count and
// init_count from this constant, and rdPtr/wrPtr are unsigned char, so depth is the only knob to turn.
//
// ⛔ THE OTHER FIX IS FORBIDDEN: do NOT raise the BGM threads above the IO worker. That lets audio
// preempt game-list IO, which is the #340 input-starvation path -- trading a music glitch for
// swallowed controller input. Deepening the buffer costs memory and nothing else.
#define BGM_RING_BUFFER_COUNT 48
#define BGM_RING_BUFFER_SIZE  4096

// Slices bgmStop() will wait for each BGM thread to stop. One slice is the SetAlarm below, 200*16
// h-blanks, ~200 ms -- so 16 is a little over 3 seconds each. A healthy stop takes one slice.
#define BGM_STOP_WAIT_SLICES  16
#define BGM_THREAD_BASE_PRIO  0x40
#define BGM_THREAD_STACK_SIZE 0x1000

extern void *_gp;

static int bgmThreadID, bgmIoThreadID;
static int outSema, inSema;
// Shared between the BGM playback thread and the main thread; volatile so updates
// are observed across threads rather than cached in a register.
static volatile unsigned char terminateFlag, bgmIsPlaying;
static volatile unsigned char rdPtr, wrPtr;
static char bgmBuffer[BGM_RING_BUFFER_COUNT][BGM_RING_BUFFER_SIZE];
static volatile unsigned char bgmThreadRunning, bgmIoThreadRunning;

// Nonzero while bgmIoThread may be inside a device read. Read by the art worst-open latch.
volatile int gBgmInRead = 0;

static u8 bgmThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static u8 bgmIoThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));

static OggVorbis_File *vorbisFile;

static void bgmThread(void *arg)
{
    bgmThreadRunning = 1;

    while (!terminateFlag) {
        SleepThread();

        while (PollSema(outSema) == outSema) {
            audsrv_wait_audio(BGM_RING_BUFFER_SIZE);
            audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);
            rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;

            SignalSema(inSema);
        }
    }

    audsrv_stop_audio();

    rdPtr = 0;
    wrPtr = 0;

    bgmThreadRunning = 0;
    bgmIsPlaying = 0;
}

static void bgmIoThread(void *arg)
{
    int partsToRead, decodeTotal, bitStream, i;

    bgmIoThreadRunning = 1;
    do {
        WaitSema(inSema);

        // TERMINATE BEFORE DECODING, not after. Ours checked only at the BOTTOM of the do/while, so a
        // thread woken during teardown ran one more full decode first -- and by then hddCleanUp has
        // already closed every pfs descriptor with PDIOC_CLOSEALL (src/opl.c runs deinitAllSupport
        // BEFORE audioEnd). ov_read on the dead fd returns 0, the ret==0 arm calls ov_pcm_seek and
        // retries, decodeTotal never decreases, and the inner loop SPINS FOREVER. bgmIoThreadRunning
        // then never clears and bgmStop's unbounded wait below never returns: the black-screen exit
        // hang, reproducible only with ATA up because hddCleanUp is the only teardown in the tree
        // that closes file descriptors. master has this guard; we never ported it.
        if (terminateFlag || !gEnableBGM)
            break;

        partsToRead = 1;

        while ((wrPtr + partsToRead < BGM_RING_BUFFER_COUNT) && (PollSema(inSema) == inSema))
            partsToRead++;

        decodeTotal = BGM_RING_BUFFER_SIZE;
        int bufferPtr = 0;
        // The BGM decoder reads bgm.ogg through the SAME process-wide fileXio channel as art, and it
        // does NOT go through ioman -- so it is invisible to the HUD's IO column and to every
        // ioGetPending() sample. Sol's IOP audit flagged exactly that as the hole in rebuild-161's
        // instrument. This flag closes it: set while this thread can be inside a device read.
        gBgmInRead = 1;
        do {
            int ret = ov_read(vorbisFile, bgmBuffer[wrPtr] + bufferPtr, decodeTotal, 0, 2, 1, &bitStream);
            if (ret > 0) {
                bufferPtr += ret;
                decodeTotal -= ret;
            } else if (ret < 0) {
                LOG("BGM: I/O error while reading.\n");
                terminateFlag = 1;
                break;
            } else if (ret == 0) {
                // End of stream: loop the track. If the SEEK fails we cannot make progress -- the
                // next ov_read returns 0 again and decodeTotal never falls, so this inner loop is
                // unbounded. That is not hypothetical: a descriptor closed underneath us (pfs
                // PDIOC_CLOSEALL during teardown) presents exactly as permanent EOF. Treat it as the
                // I/O error it is.
                if (ov_pcm_seek(vorbisFile, 0) != 0) {
                    LOG("BGM: cannot rewind (descriptor lost?) -- stopping.\n");
                    terminateFlag = 1;
                    break;
                }
            }
        } while (decodeTotal > 0);
        gBgmInRead = 0;

        if (terminateFlag)
            break;

        wrPtr = (wrPtr + partsToRead) % BGM_RING_BUFFER_COUNT;
        for (i = 0; i < partsToRead; i++)
            SignalSema(outSema);
        WakeupThread(bgmThreadID);
    } while (!terminateFlag && gEnableBGM);

    bgmIoThreadRunning = 0;
    terminateFlag = 1;
    // Release the consumer too, not just the sleeper: bgmThread parks in SleepThread here (master's
    // parks on outSema), so it needs the wakeup -- but if it is instead inside its PollSema drain it
    // needs the count. Signalling a semaphore nothing is waiting on is harmless; bgmDeinit deletes
    // both a moment later.
    SignalSema(outSema);
    WakeupThread(bgmThreadID);
}

static int bgmLoad(void)
{
    FILE *bgmFile;
    char bgmPath[256];

    vorbisFile = malloc(sizeof(OggVorbis_File));
    memset(vorbisFile, 0, sizeof(OggVorbis_File));

    int themeID = thmGetGuiValue();
    if (themeID != 0) {
        char *thmPath = thmGetFilePath(themeID);
        snprintf(bgmPath, sizeof(bgmPath), "%ssound/bgm.ogg", thmPath);
    } else
        snprintf(bgmPath, sizeof(bgmPath), gDefaultBGMPath);

    bgmFile = fopen(bgmPath, "rb");
    if (bgmFile == NULL) {
        LOG("BGM: Failed to open Ogg file %s\n", bgmPath);
        return -ENOENT;
    }

    if (ov_open_callbacks(bgmFile, vorbisFile, NULL, 0, OV_CALLBACKS_DEFAULT) < 0) {
        LOG("BGM: Input does not appear to be an Ogg bitstream.\n");
        return -ENOENT;
    }

    return 0;
}

static int bgmInit(void)
{
    ee_thread_t thread;
    ee_sema_t sema;
    int result;

    terminateFlag = 0;
    rdPtr = 0;
    wrPtr = 0;
    bgmThreadRunning = 0;
    bgmIoThreadRunning = 0;

    sema.max_count = BGM_RING_BUFFER_COUNT;
    sema.init_count = BGM_RING_BUFFER_COUNT;
    sema.attr = 0;
    sema.option = (u32) "bgm-in-sema";
    inSema = CreateSema(&sema);

    if (inSema >= 0) {
        sema.max_count = BGM_RING_BUFFER_COUNT;
        sema.init_count = 0;
        sema.attr = 0;
        sema.option = (u32) "bgm-out-sema";
        outSema = CreateSema(&sema);

        if (outSema < 0) {
            DeleteSema(inSema);
            return outSema;
        }
    } else
        return inSema;

    thread.func = &bgmThread;
    thread.stack = bgmThreadStack;
    thread.stack_size = sizeof(bgmThreadStack);
    thread.gp_reg = &_gp;
    thread.initial_priority = BGM_THREAD_BASE_PRIO;
    thread.attr = 0;
    thread.option = 0;

    // BGM thread will start in DORMANT state.
    bgmThreadID = CreateThread(&thread);

    if (bgmThreadID >= 0) {
        thread.func = &bgmIoThread;
        thread.stack = bgmIoThreadStack;
        thread.stack_size = sizeof(bgmIoThreadStack);
        thread.gp_reg = &_gp;
        thread.initial_priority = BGM_THREAD_BASE_PRIO + 1;
        thread.attr = 0;
        thread.option = 0;

        // BGM I/O thread will start in DORMANT state.
        bgmIoThreadID = CreateThread(&thread);
        if (bgmIoThreadID >= 0) {
            result = 0;
        } else {
            DeleteSema(inSema);
            DeleteSema(outSema);
            DeleteThread(bgmThreadID);
            result = bgmIoThreadID;
        }
    } else {
        result = bgmThreadID;
        DeleteSema(inSema);
        DeleteSema(outSema);
    }

    return result;
}

static void bgmDeinit(void)
{
    DeleteSema(inSema);
    DeleteSema(outSema);
    DeleteThread(bgmThreadID);
    DeleteThread(bgmIoThreadID);

    // Vorbisfile takes care of fclose.
    ov_clear(vorbisFile);
    free(vorbisFile);
    vorbisFile = NULL;
}

static void bgmShutdownDelayCallback(s32 alarm_id, u16 time, void *common)
{
    iWakeupThread((int)common);
}

void bgmStart(void)
{
    struct audsrv_fmt_t audsrvFmt;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    int ret = bgmInit();
    if (ret >= 0) {
        if (bgmLoad() != 0) {
            bgmDeinit();
            return;
        }

        vorbis_info *vi = ov_info(vorbisFile, -1);
        ov_pcm_seek(vorbisFile, 0);

        audsrvFmt.channels = vi->channels;
        audsrvFmt.freq = vi->rate;
        audsrvFmt.bits = 16;

        audsrv_set_format(&audsrvFmt);

        bgmIsPlaying = 1;

        StartThread(bgmIoThreadID, NULL);
        StartThread(bgmThreadID, NULL);
    }
}

// Tell the BGM threads to stop and return IMMEDIATELY. No join, no wait, no bgmDeinit.
//
// This exists because of two hardware reports that pull in opposite directions. rebuild-163 moved
// the full bgmStop() to the top of deinit so the music would stop before the device it streams from
// is destroyed -- Vass327 confirmed that fixed issue #382 on all three flavours. Nathan then found
// it froze his exit from a UDPFS boot, so 164 backed the whole thing out, which also gave up a
// confirmed fix.
//
// The ORDER was right; the WAIT was the problem. bgmStop's join sleeps on SetAlarm/SleepThread, and
// doing that at the very top of deinit put a multi-second blocking wait on a path that had never
// blocked there before. So: signal here, join later. The decoder stops touching the device
// immediately, which is all issue #382 needs, and the actual thread join stays where it always was,
// in audioEnd(), after the device teardown -- exactly where it has always been safe.
void bgmQuiesce(void)
{
    if (!audio_initialized)
        return;

    terminateFlag = 1;

    // Both semaphores, because the io thread parks on inSema and a WakeupThread cannot release one
    // -- the deadlock rebuild-154 fixed. Signalling one nothing waits on is harmless.
    SignalSema(inSema);
    SignalSema(outSema);
    WakeupThread(bgmThreadID);
}

void bgmStop(void)
{
    int threadId;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    LOG("BGM: terminating threads...\n");

    terminateFlag = 1;

    // WAKE THE PRODUCER, WHICH THIS NEVER DID. bgmIoThread parks in WaitSema(inSema), and a
    // WakeupThread does not release a semaphore -- nor was it even aimed at that thread; bgmThreadID
    // is the PLAYBACK thread. The only SignalSema(inSema) in the whole file is inside bgmThread's
    // `while (PollSema(outSema))` drain, so once playback sees terminateFlag and exits, nothing can
    // ever signal inSema again and the io thread is parked for good -- with the first wait below
    // spinning on it forever.
    //
    // It survived because it is a RACE, and one the BGM stutter fix lost: with the ring at 48 buffers
    // (was 16) the producer runs far enough ahead that "parked on inSema with nothing pending for
    // playback to drain" became the normal steady state rather than a rare one.
    SignalSema(inSema);
    SignalSema(outSema);
    WakeupThread(bgmThreadID);

    // BOUNDED. These were unbounded `while (running)` spins, i.e. the last step of the exit hang:
    // whatever wedges the decoder also wedges the thread trying to shut it down, and the screen is
    // already gone by the time audioEnd runs. A thread that will not stop is left alone rather than
    // terminated (same rule as the art worker: never kill a thread that may hold an IOP RPC), and
    // audsrv_quit below is what actually silences it. ~3 s is far past any healthy stop.
    threadId = GetThreadId();
    int waits = BGM_STOP_WAIT_SLICES;
    while (bgmIoThreadRunning && waits-- > 0) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }
    waits = BGM_STOP_WAIT_SLICES;
    while (bgmThreadRunning && waits-- > 0) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }

    if (bgmIoThreadRunning || bgmThreadRunning) {
        LOG("BGM: threads did not stop (io=%d play=%d) -- abandoning\n",
            (int)bgmIoThreadRunning, (int)bgmThreadRunning);
        return; // do NOT bgmDeinit(): a live thread still holds these semaphores and the vorbis file
    }

    bgmDeinit();

    LOG("BGM: stopped.\n");
}

int isBgmPlaying(void)
{
    int ret = (int)bgmIsPlaying;

    return ret;
}

// HACK: BGM stutters while perfroming certain tasks, mute during these operations and unmute once completed.
void bgmMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(0);
}

void bgmUnMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(gBGMVolume);
}

/*--    General Audio    ------------------------------------------------------------------------------------------------------
-----------------------------------------------------------------------------------------------------------------------------*/

void audioInit(void)
{
    if (!audio_initialized) {
        if (audsrv_init() != 0) {
            LOG("AUDIO: Failed to initialize audsrv\n");
            LOG("AUDIO: Audsrv returned error string: %s\n", audsrv_get_error_string());
            return;
        }
        // Create the SFX dispatch thread BEFORE audio_initialized flips on: both producers
        // gate on that flag, so creation happens-before any possible enqueue.
        sfxDispatchStart();
        audio_initialized = 1;
    }
}

void audioEnd(void)
{
    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    // Stop BGM if anything is still alive, not merely if it is still PLAYING. A settings toggle can
    // leave the io thread self-exited (terminateFlag=1, bgmIsPlaying=0) with bgmDeinit never called,
    // so the narrow predicate skipped bgmStop and ran audsrv_quit() underneath live threads. Master's
    // predicate; both flags are already file-static volatiles here.
    if (isBgmPlaying() || bgmIoThreadRunning || bgmThreadRunning)
        bgmStop();

    // Park the SFX dispatch thread: an audsrv RPC crossing audsrv_quit wedges it forever.
    sfxDispatchQuiesce();

    audsrv_quit();
    audio_initialized = 0;
}

void audioSetVolume(void)
{
    int i;

    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    for (i = 1; i < SFX_COUNT; i++)
        audsrv_adpcm_set_volume(i, gSFXVolume);

    audsrv_adpcm_set_volume(0, gBootSndVolume);
    audsrv_set_volume(gBGMVolume);
}
