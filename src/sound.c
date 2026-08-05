/*
 Copyright 2022, Thanks to SP193
 Licenced under Academic Free License version 3.0
 Review OpenPS2Loader README & LICENSE files for further details.
 */

#include <audsrv.h>
#include <timer.h>
#include <vorbis/vorbisfile.h>

#include "include/sound.h"
#include "include/opl.h"
#include "include/ioman.h"
#include "include/themes.h"
#include "include/pad.h" // padRumbleTap/padRumbleTapList -- menu rumble mirrors the cursor tick (#172)
#include "include/gui.h" // guiGetCurrentScreen -- the game list and the menus tap differently (#172)

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

#define CURSOR_SFX_CHANNEL_BASE  SFX_COUNT
#define CURSOR_SFX_CHANNEL_COUNT 6

static int cursorChannelIndex = 0;

static int sfxGetCursorChannel(int slot)
{
    return CURSOR_SFX_CHANNEL_BASE + slot;
}

static void sfxSetCursorChannelsVolume(int volume)
{
    for (int i = 0; i < CURSOR_SFX_CHANNEL_COUNT; i++)
        audsrv_adpcm_set_volume_and_pan(sfxGetCursorChannel(i), volume, 0);
}

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
    if (size <= 0) {
        LOG("SFX: lseek failed or empty file: %s\n", full_path);
        close(adpcm);
        return -EIO;
    }
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

    // Calculate duration based on number of samples. The sample count lives at
    // u32 offset 3 (bytes 12-15); guard against a SFX file shorter than that
    // header so we don't read past the buffer.
    if (sfxData->size >= (int)(4 * sizeof(u32)))
        sfxData->duration_ms = sfxCalculateSoundDuration(((u32 *)sfxData->buffer)[3]);
    else
        sfxData->duration_ms = 0;
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

    audsrv_adpcm_init();
    cursorChannelIndex = 0;
    sfxInitDefaults();
    audioSetVolume();

    // Check default theme is not current theme
    int themeID = thmGetGuiValue();
    char *thmPath = thmGetFilePath(themeID);
    if (thmPath != NULL) { // NULL for <OPL> + the built-in <Coverflow> -> use default sfx
        // Get theme path for sfx
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

// Debug-Colors instrumentation (#271/#272): per-press audsrv RPC wall time. audsrv_ch_play_adpcm
// is a SYNCHRONOUS SIF RPC on the GUI thread (one per nav press); under IOP contention it stalls
// the menu mid-navigation. Always measured (two tick reads), rendered only when Settings ->
// Debug Colors is on (gui.c / dia.c).
#define SFX_CLOCKS_PER_MS 147456 // EE cpu_ticks() rate, same constant as pad.c's CLOCKS_PER_MILISEC
static unsigned int sfxLastPlayMs = 0;
static unsigned int sfxMaxPlayMs = 0;

void sfxGetPlayDiag(unsigned int *lastMs, unsigned int *maxMs)
{
    if (lastMs)
        *lastMs = sfxLastPlayMs;
    if (maxMs)
        *maxMs = sfxMaxPlayMs;
}

void sfxPlay(int id)
{
    int channel;

    // Menu rumble (#172): mirror the GUI's own feedback on the pad. Deliberately ABOVE both gates
    // below -- rumble is haptic feedback, not sound, so it must survive a user who plays with SFX off
    // (or a build where audsrv never came up). Every cursor move / confirm / cancel in the whole GUI
    // already funnels through sfxPlay(), so these lines cover them all with no new call sites.
    // Both arms rate-limit themselves and never block.
    //
    // NOTE: SFX_CONFIRM is also the LAUNCH edge, and the pulse's decay clock is ticked by readPads(),
    // which stops running during the launch handoff -- so itemExecSelect() calls padRumbleFlush()
    // before that blocking work, or this bump would run for the whole loading screen. See pad.c.
    // The game list and the menus both move the cursor with sfxPlay(SFX_CURSOR) -- the SAME id, so an
    // id-only hook cannot tell them apart, and on hardware they do NOT feel alike at identical
    // settings (the reporter called one "too strong" and the other "very weak" in the same breath).
    // GUI_SCREEN_MAIN is exclusively the game list; the START menu is GUI_SCREEN_MENU. Reading the
    // screen here is safe: input is gated off while a screen transition is in flight, so no cursor sfx
    // can fire while the answer is ambiguous.
    if (id == SFX_CURSOR) {
        if (guiGetCurrentScreen() == GUI_SCREEN_MAIN)
            padRumbleTapList();
        else
            padRumbleTap();
    } else if (id == SFX_CONFIRM || id == SFX_CANCEL || id == SFX_MESSAGE)
        padRumbleBump();
    // SFX_MESSAGE (notifications / message boxes) is safe to arm from here: every one of its sites
    // renders from a loop that polls readPads() -- the main loop for guiShowNotifications, and
    // guiMsgBox's own modal loop -- so the pulse decays normally.
    //
    // SFX_BOOT is deliberately NOT armed here. It plays from inside guiIntroLoop(), whose loop never
    // polls readPads(), so the decay would be frozen for the whole intro (it runs for the length of
    // the boot jingle) -- seconds of buzz. The "ready" tap is armed in main() right after the intro
    // returns instead, which is the moment the user actually cares about and where the main loop is
    // about to start ticking the decay.

    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (gEnableSFX) {
        // NAV-PARITY TEST BUILD (#340): official's sfxPlay is exactly this one call -- no cursor
        // channel rotation, no 45 ms cursor rate limit, no per-call tick diagnostics.
        (void)channel;
        audsrv_ch_play_adpcm(id, &sfx[id]);
    }
}

/*--    Theme Background Music    -------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

#define BGM_RING_BUFFER_COUNT 16
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
        WaitSema(outSema);
        if (terminateFlag)
            break;

        audsrv_wait_audio(BGM_RING_BUFFER_SIZE);
        audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);
        rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;

        SignalSema(inSema);
    }

    audsrv_stop_audio();

    rdPtr = 0;
    wrPtr = 0;

    bgmThreadRunning = 0;
    bgmIsPlaying = 0;
}

static void bgmIoThread(void *arg)
{
    int decodeTotal, bitStream;

    bgmIoThreadRunning = 1;
    do {
        WaitSema(inSema);

        if (terminateFlag || !gEnableBGM)
            break;

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

        if (terminateFlag)
            break;

        wrPtr = (wrPtr + 1) % BGM_RING_BUFFER_COUNT;
        SignalSema(outSema);
    } while (!terminateFlag && gEnableBGM);

    bgmIoThreadRunning = 0;
    terminateFlag = 1;
    SignalSema(outSema);
}

static int bgmLoad(void)
{
    char bgmPath[256];
    int themeID;

    vorbisFile = malloc(sizeof(OggVorbis_File));
    if (vorbisFile == NULL)
        return -ENOMEM;

    memset(vorbisFile, 0, sizeof(OggVorbis_File));

    themeID = thmGetGuiValue();
    char *thmPath = thmGetFilePath(themeID);
    if (thmPath != NULL) { // NULL for <OPL> + the built-in <Coverflow> -> no theme BGM folder
        snprintf(bgmPath, sizeof(bgmPath), "%ssound/bgm.ogg", thmPath);
        FILE *bgmFile = fopen(bgmPath, "rb");
        if (bgmFile != NULL) {
            if (ov_open_callbacks(bgmFile, vorbisFile, NULL, 0, OV_CALLBACKS_DEFAULT) == 0) {
                LOG("BGM: Loaded theme BGM %s\n", bgmPath);
                return 0;
            }

            LOG("BGM: Theme BGM is not a valid Ogg bitstream: %s\n", bgmPath);
            fclose(bgmFile);
            memset(vorbisFile, 0, sizeof(OggVorbis_File));
        } else {
            LOG("BGM: Theme BGM not found: %s\n", bgmPath);
        }
    }

    if (gDefaultBGMPath[0] != '\0') {
        FILE *bgmFile;

        snprintf(bgmPath, sizeof(bgmPath), "%s", gDefaultBGMPath);
        bgmFile = fopen(bgmPath, "rb");
        if (bgmFile != NULL) {
            if (ov_open_callbacks(bgmFile, vorbisFile, NULL, 0, OV_CALLBACKS_DEFAULT) == 0) {
                LOG("BGM: Loaded configured BGM %s\n", bgmPath);
                return 0;
            }

            LOG("BGM: Configured BGM is not a valid Ogg bitstream: %s\n", bgmPath);
            fclose(bgmFile);
            memset(vorbisFile, 0, sizeof(OggVorbis_File));
        } else {
            LOG("BGM: Configured BGM not found: %s\n", bgmPath);
        }
    }

    // No embedded fallback BGM (removed to save ~324 KB); BGM plays only when a
    // theme provides sound/bgm.ogg or a BGM path is configured.
    LOG("BGM: No theme or configured BGM available.\n");
    free(vorbisFile);
    vorbisFile = NULL;

    return -ENOENT;
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

    if (vorbisFile != NULL) {
        // Vorbisfile takes care of fclose for file-backed sources.
        ov_clear(vorbisFile);
        free(vorbisFile);
        vorbisFile = NULL;
    }
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
        audsrv_set_volume(gBGMVolume);

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
    SignalSema(inSema);
    SignalSema(outSema);

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
        audio_initialized = 1;
    }
}

void audioEnd(void)
{
    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    /* Stop BGM if it is still running: bgmIsPlaying covers the normal case;
     * bgmIoThreadRunning/bgmThreadRunning cover the case where a settings-toggle
     * caused the IO thread to self-exit (setting terminateFlag=1/bgmIsPlaying=0)
     * but bgmDeinit() was never called, leaking the semaphores and thread handles. */
    if (isBgmPlaying() || bgmIoThreadRunning || bgmThreadRunning)
        bgmStop();

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

    sfxSetCursorChannelsVolume(gSFXVolume);
    audsrv_adpcm_set_volume(0, gBootSndVolume);
    audsrv_set_volume(gBGMVolume);
}
