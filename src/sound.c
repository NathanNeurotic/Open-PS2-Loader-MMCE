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
#define SFX_QUEUE_LEN        8
// Entries older than this play no more: after an IOP stall clears, replaying the backlog of
// stacked cursor ticks would chirp back-to-back.
#define SFX_STALE_TICKS      (100 * SFX_CLOCKS_PER_MS)
// Confirm (847 ms) and cancel (522 ms) are far longer than a cursor tick and are fired by deliberate
// presses, not by scrolling, so they get a much looser bound: long enough that a real press always
// sounds even when the dispatcher is briefly behind, short enough that a backlog which surfaces
// after an IOP stall is still discarded instead of chirping long after the fact.
#define SFX_STALE_TICKS_LONG (500 * SFX_CLOCKS_PER_MS)
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
            u32 staleLimit = (id == SFX_CONFIRM || id == SFX_CANCEL) ? SFX_STALE_TICKS_LONG : SFX_STALE_TICKS;
            int stale = (id == SFX_CURSOR || id == SFX_CONFIRM || id == SFX_CANCEL) &&
                        (u32)(cpu_ticks() - ticks) > staleLimit;
            if (sfxDispatchPaused || !audio_initialized || stale) {
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
    // The backlog problem that motivated it is handled where it belongs -- at DISPATCH, by the
    // staleness test, which now covers these ids too (see sfxDispatchThread).

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
        EIntr();
        return; // ring full under IOP congestion: drop the sound, never the frame
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
        partsToRead = 1;

        while ((wrPtr + partsToRead < BGM_RING_BUFFER_COUNT) && (PollSema(inSema) == inSema))
            partsToRead++;

        decodeTotal = BGM_RING_BUFFER_SIZE;
        int bufferPtr = 0;
        do {
            int ret = ov_read(vorbisFile, bgmBuffer[wrPtr] + bufferPtr, decodeTotal, 0, 2, 1, &bitStream);
            if (ret > 0) {
                bufferPtr += ret;
                decodeTotal -= ret;
            } else if (ret < 0) {
                LOG("BGM: I/O error while reading.\n");
                terminateFlag = 1;
                break;
            } else if (ret == 0)
                ov_pcm_seek(vorbisFile, 0);
        } while (decodeTotal > 0);

        wrPtr = (wrPtr + partsToRead) % BGM_RING_BUFFER_COUNT;
        for (i = 0; i < partsToRead; i++)
            SignalSema(outSema);
        WakeupThread(bgmThreadID);
    } while (!terminateFlag && gEnableBGM);

    bgmIoThreadRunning = 0;
    terminateFlag = 1;
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

void bgmStop(void)
{
    int threadId;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    LOG("BGM: terminating threads...\n");

    terminateFlag = 1;
    WakeupThread(bgmThreadID);

    threadId = GetThreadId();
    while (bgmIoThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }
    while (bgmThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
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

    if (gEnableBGM && isBgmPlaying())
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
