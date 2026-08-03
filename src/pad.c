/*
  Copyright 2009, Ifcaro, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/pad.h"
#include "include/ioman.h"
#include <delaythread.h>
#include <libpad.h>
#include <timer.h>
#include <time.h>

#ifdef PADEMU
#include <libds34bt.h>
#include <libds34usb.h>
#endif

#define MAX_PADS 4

// Cpu ticks per one milisecond
#define CLOCKS_PER_MILISEC 147456

// 200 ms per repeat
#define DEFAULT_PAD_DELAY 200

// ---- Menu rumble pulse shape (#172) ---------------------------------------------------------------
// Defined up here because readPad()'s ds34 leg needs the levels long before the rumble section further
// down. Engine roles: the small engine (act[0]) is on/off -- a crisp attack; the big engine (act[1],
// 0..255) is an offset-weight ERM that needs ~50-80ms just to start turning but is the one you
// actually FEEL. (History: the first cut drove the small engine ALONE and the reporter felt nothing;
// the second drove both; the RETRO retune below made taps big-only and kept both on bumps.)
//
// THE BIG ENGINE'S LEVEL IS THE ONLY REAL INTENSITY KNOB WE HAVE. The small engine is on/off IN
// HARDWARE, not by our choice -- libpad.h:252-255 is explicit: "act_align[0] = 0/1 turns off/on 'small'
// engine" vs "act_align[1] = 0-255 sets 'big' engine speed". So a "quieter" tap has a FLOOR: the attack
// never softens, only the body does. Do not expect 0x48 to feel like 3/4 of 0x60.
// Lowering a level also buys ZERO current headroom: freepad's 600mA guard (CheckAirDirectTotal,
// padMiscFuncs.c:265-292) tests each actuator for NONZERO, not for magnitude -- 0x01 costs it exactly
// what 0xFF does. Only switching an engine fully off counts.
// Pulse shape retuned to RETROLauncher's MEASURED model (Nathan, HW 2026-07-16: "theirs seems more
// well implemented from the feeling perspective"). Decoded from their Lua (funciones.lua capturar):
// a nav step rumbles ~260-283ms at big-engine 80 (0x50) inside a ~667ms step period (~40% duty), and
// fast scroll is SILENT -- their per-step input freeze IS the rate limiter. Two consequences:
//   - Our old 60-75ms taps ended right as the big ERM began turning, so we shipped mostly the small
//     engine's click, and the old list level (0x78) was compensating for a window too short to feel.
//     RETRO's thump is LONGER and SOFTER, not stronger. One tap now serves menu and list alike.
//   - On a native DS2 their small motor NEVER fires: its drive byte keys on the LSB and RETRO's
//     values (80/90) are even. The reference feel is big-ERM-only, so TAPS drop the small engine.
//     BUMPS keep it -- a confirm should have a crisp attack; RETRO doesn't rumble confirm at all,
//     but Blade explicitly asked for it and it is HW-validated.
#define RUMBLE_TAP_MS           240  // nav thump, first press: long enough for the ERM to reach amplitude
#define RUMBLE_TAP_LEVEL        0x50 // RETRO's level, byte for byte (80/255 ~ 31%)
#define RUMBLE_BUMP_MS          110  // confirm / cancel / notification / ready (unchanged, HW-validated)
#define RUMBLE_LEVEL_BUMP       0x60
// Held-repeat policy, replacing the old flat 120ms gap (which ALIASED against key-repeat: 300ms
// repeat -> sparse flutters; 100ms repeat -> every other pulse dropped at ragged 100/200ms gaps).
// RETRO's throb rides each step at ~40% duty, and steps too fast to fit a felt pulse are silent.
// We self-measure the event rate (inter-arm interval) instead of plumbing key identity into sfxPlay.
#define RUMBLE_REPEAT_WINDOW_MS 400 // arms closer together than this = a held-repeat train
#define RUMBLE_REPEAT_DUTY_PCT  45  // pulse = this % of the measured step period while held
#define RUMBLE_REPEAT_FLOOR_MS  80  // shorter can't be felt (ERM spin-up) -> stay silent (RETRO rule)

struct pad_data_t
{
    int port, slot;
    int state;
    u32 paddata;
    struct padButtonStatus buttons;

    // pad_dma_buf is provided by the user, one buf for each pad
    // contains the pad's current state
    char padBuf[256] __attribute__((aligned(64)));

    char actAlign[6];
    int actuators;
    int analogCapable; // -1 unknown/not ready, 0 digital-only (fully published table), 1 DualShock-capable
    int analogRetryDelay;

    unsigned char rumbleOn;    // 1 = an "on" was sent that still owes its matching "off"
    unsigned char rumbleLevel; // big-engine level for the pulse in flight (0 = none armed)
    unsigned char rumbleSmall; // 1 = drive the small engine too (bumps); taps are big-ERM-only
    int rumbleMsLeft;          // ms remaining; ticked down in readPads() (see the ms-vs-frames note there)
};

// Pad commands are asynchronous. Keep every wait bounded so a transient SIO2/pad error cannot hang the
// GUI thread.
#define PAD_WAIT_POLLS   25
#define PAD_WAIT_POLL_US 1000

// Successful-read polls between analog self-heal attempts -- a POLL COUNT, decremented once per
// good read, not milliseconds like its neighbours.
#define PAD_ANALOG_RETRY_DELAY 60
// Milliseconds without registered input before an inline initializePad() -- reconnect edge or
// analog self-heal -- may run (#271/#272). That init is 250-600 ms of polled waits ON THE GUI
// THREAD, and its triggers (reconnect flaps, freepad dropping to digital) are driven by the read
// errors that cluster under SIO2 load -- so ungated it fired precisely while the user was
// navigating and ate the taps in flight. A digital-mode pad still reads every d-pad press, so
// deferring costs nothing the user can feel; the heal fires within a second of the last
// REGISTERED press. Known residual: a hardware-stuck button starves the heal for the session.
#define PAD_SELF_HEAL_IDLE_MS  1000
// A padSetMainMode round-trip can NEVER finish under freepad's own minimum latency: the IOP main
// thread dispatches the task on the next vblank, SetMainModeThread needs three vblank-gated SIO2
// transfers, and the request only flips COMPLETE after the next good ReadData -- >= 5 vblanks,
// ~83-100 ms. The generic 25 ms budget above therefore ALWAYS timed out on this leg, so analog
// arming worked only when the IOP happened to finish in the background and the pressure/rumble
// setup below it was unreachable. Give request-completion waits a budget above the happy path.
#define PAD_REQ_WAIT_POLLS     150

#define PAD_INIT_RETRY       -1
#define PAD_INIT_UNSUPPORTED 0
#define PAD_INIT_OK          1

/// current time in miliseconds (last update time)
static u32 curtime = 0;
// Last readPads() poll (curtime ms) on which ANY button/stick input was down. Drives the PAD_SELF_HEAL_IDLE_MS gate.
static u32 lastInputActivityMs = 0;
static u32 time_since_last = 0;
// Raw-tick bookkeeping for the wrap-correct delta in readPads(). lastticks is the previous poll's
// cpu_ticks(); tickrem carries the sub-millisecond remainder so dividing a delta stays drift-free.
static u32 lastticks = 0;
static u32 tickrem = 0;
static int padTicksSeeded = 0;

static unsigned short pad_count;
static struct pad_data_t pad_data[MAX_PADS];

// gathered pad data
static u32 paddata;
static u32 oldpaddata;


// Debug-Colors instrumentation (#271/#272). All writers run on the EE main thread inside
// readPads/readPad/initializePad, and the HUD (gui.c/dia.c) reads on the same thread, so no
// locking is needed. Counters are cumulative and always maintained (integer bumps only); they
// are SHOWN only when Settings -> Debug Colors is on.
static pad_diag_t padDiag;

// Per-poll read outcome, reset by readPads() and filled in by each readPad(): how many pads were
// in a ready state, and how many produced a fresh sample. A poll where a ready pad produced no
// fresh sample is a read MISS (per-pad accounting: another pad reading fine does not hide it).
// Misses cluster under SIO2 load on real hardware; an emulator's pad never produces one.
static int pollPadsReady;
static int pollPadsRead;

void padGetDiag(pad_diag_t *out)
{
    if (out)
        *out = padDiag;
}

/*
 * Screen transitions keep polling pads but do not dispatch input. Freeze the
 * baseline so a different button held when the destination appears can be
 * consumed there. Seed it from the transition-triggering sample on entry: that
 * button is already consumed and must not toggle the destination back.
 *
 * A tap pressed and released entirely inside the fade remains intentionally
 * ignored. Polling continues for rumble timing, pad state, and reconnects.
 */
static u32 edgedata;
static u32 oldedgedata;
static int edgeBaselineFrozen = 0;

void padFreezeEdgeBaseline(int freeze)
{
    int next = freeze ? 1 : 0;

    if (next && !edgeBaselineFrozen) {
        oldpaddata = paddata;
        oldedgedata = edgedata;
    }

    edgeBaselineFrozen = next;
}

static int delaycnt[16];
static int paddelay[16];

// KEY_ to PAD_ conversion table
static const int keyToPad[17] = {
    -1,
    PAD_LEFT,
    PAD_DOWN,
    PAD_RIGHT,
    PAD_UP,
    PAD_START,
    PAD_R3,
    PAD_L3,
    PAD_SELECT,
    PAD_SQUARE,
    PAD_CROSS,
    PAD_CIRCLE,
    PAD_TRIANGLE,
    PAD_R1,
    PAD_L1,
    PAD_R2,
    PAD_L2};

static int isPadReadyState(int state)
{
    return (state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1);
}

/*
 * waitPadReady()
 */
static int waitPadReady(struct pad_data_t *pad)
{
    int state = PAD_STATE_DISCONN;
    int polls;

    for (polls = 0; polls < PAD_WAIT_POLLS; polls++) {
        state = padGetState(pad->port, pad->slot);
        if (isPadReadyState(state) || (state == PAD_STATE_DISCONN))
            return state;
        DelayThread(PAD_WAIT_POLL_US);
    }

    LOG("PAD pad %d,%d ready wait timed out in state %d\n", pad->port, pad->slot, state);

    return state;
}

static int waitPadRequestComplete(struct pad_data_t *pad)
{
    int reqState = PAD_RSTAT_BUSY;
    int polls;

    for (polls = 0; polls < PAD_REQ_WAIT_POLLS; polls++) {
        reqState = padGetReqState(pad->port, pad->slot);
        if (reqState != PAD_RSTAT_BUSY)
            return reqState == PAD_RSTAT_COMPLETE;
        DelayThread(PAD_WAIT_POLL_US);
    }

    LOG("PAD pad %d,%d request timed out\n", pad->port, pad->slot);
    return 0;
}

static int initializePadInner(struct pad_data_t *pad)
{
    int tmp;
    int modes;
    int i;
    int state;

    LOG("PAD initializing pad %d,%d\n", pad->port, pad->slot);

    // Menu rumble state belongs to the current connection.
    pad->rumbleOn = 0;
    pad->rumbleMsLeft = 0;

    // is there any device connected to that port?
    state = waitPadReady(pad);
    if (state == PAD_STATE_DISCONN) {
        LOG("PAD pad %d,%d not connected.\n", pad->port, pad->slot);
        return PAD_INIT_RETRY;
    }
    if (!isPadReadyState(state))
        return PAD_INIT_RETRY;

    // How many different modes can this device operate in?
    // i.e. get # entrys in the modetable
    modes = padInfoMode(pad->port, pad->slot, PAD_MODETABLE, -1);
    LOG("PAD The device has %d modes: ", modes);

    if (modes > 0) {
        LOG("( ");

        for (i = 0; i < modes; i++) {
            tmp = padInfoMode(pad->port, pad->slot, PAD_MODETABLE, i);
            LOG("%d ", tmp);
        }

        LOG(")\n");
    }

    tmp = padInfoMode(pad->port, pad->slot, PAD_MODECURID, 0);
    LOG("PAD It is currently using mode %d\n", tmp);

    // A not-yet-ready DualShock also reports an empty mode table, so keep it retryable. Once a
    // non-empty table proves that DualShock mode is absent, mark the controller unsupported.
    if (modes <= 0) {
        LOG("PAD mode table is not ready (or controller is digital-only)\n");
        return PAD_INIT_RETRY;
    }

    // Verify that the controller has a DUAL SHOCK mode. CRITICAL: freepad's query threads have a
    // 10-attempt budget per stage, and under sustained SIO2 contention (MMCE/MX4SIO boot traffic)
    // they can publish numModes > 0 while the mode TABLE entries were never written (BSS zero).
    // Legal CTP mode types are 2..7 and freepad only writes an entry on a validated controller
    // response, so a 0/invalid entry PROVES the table is half-built -- that must stay RETRYABLE.
    // Only a fully-published table lacking DualShock is treated as unsupported.
    int sawInvalid = 0;
    int hasDualshock = 0;
    for (i = 0; i < modes; i++) {
        tmp = padInfoMode(pad->port, pad->slot, PAD_MODETABLE, i);
        if (tmp == PAD_TYPE_DUALSHOCK)
            hasDualshock = 1;
        else if (tmp == 0)
            sawInvalid = 1; // freepad writes entries only from validated responses -> 0 = unwritten slot
    }
    if (!hasDualshock) {
        if (sawInvalid) {
            LOG("PAD mode table half-built (contention?) -- retrying, NOT latching digital-only\n");
            return PAD_INIT_RETRY;
        }
        LOG("PAD This is no Dual Shock controller\n");
        // Only a FULLY-published table lacking DualShock may latch digital-only -- latching off a
        // half-built one permanently disarms the self-heal until the next physical replug.
        pad->analogCapable = 0;
        padDiag.analogCapable = 0;
        return PAD_INIT_UNSUPPORTED;
    }
    pad->analogCapable = 1;
    padDiag.analogCapable = 1;

    // If ExId != 0x0 => This controller has actuator engines
    // This check should always pass if the Dual Shock test above passed
    tmp = padInfoMode(pad->port, pad->slot, PAD_MODECUREXID, 0);
    if (tmp == 0) {
        LOG("PAD This is no Dual Shock controller??\n");
        return PAD_INIT_RETRY;
    }

    LOG("PAD Enabling dual shock functions\n");

    // When using MMODE_LOCK, user cant change mode with Select button
    tmp = padSetMainMode(pad->port, pad->slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    if (tmp != 1 || !waitPadRequestComplete(pad)) {
        LOG("PAD padSetMainMode failed: accepted=%d req=%d\n", tmp, padGetReqState(pad->port, pad->slot));
        return PAD_INIT_RETRY;
    }
    if (!isPadReadyState(waitPadReady(pad)))
        return PAD_INIT_RETRY;
    tmp = padInfoPressMode(pad->port, pad->slot);
    LOG("PAD infoPressMode: %d\n", tmp);

    if (!isPadReadyState(waitPadReady(pad)))
        return PAD_INIT_RETRY;
    tmp = padEnterPressMode(pad->port, pad->slot);
    LOG("PAD enterPressMode: %d\n", tmp);
    if (tmp == 1 && !waitPadRequestComplete(pad))
        LOG("PAD enterPressMode request failed\n");

    if (!isPadReadyState(waitPadReady(pad)))
        return PAD_INIT_OK; // analog mode is restored; pressure/rumble setup can wait for reconnect
    pad->actuators = padInfoAct(pad->port, pad->slot, -1, 0);
    LOG("PAD # of actuators: %d\n", pad->actuators);

    if (pad->actuators != 0) {
        pad->actAlign[0] = 0; // Enable small engine
        pad->actAlign[1] = 1; // Enable big engine
        pad->actAlign[2] = 0xff;
        pad->actAlign[3] = 0xff;
        pad->actAlign[4] = 0xff;
        pad->actAlign[5] = 0xff;

        if (!isPadReadyState(waitPadReady(pad)))
            return PAD_INIT_OK;
        tmp = padSetActAlign(pad->port, pad->slot, pad->actAlign);
        LOG("PAD padSetActAlign: %d\n", tmp);
        if (tmp == 1 && !waitPadRequestComplete(pad))
            LOG("PAD padSetActAlign request failed\n");
    } else {
        LOG("PAD Did not find any actuators.\n");
    }

    waitPadReady(pad);
    return PAD_INIT_OK;
}

// Timed wrapper. initializePad is the single biggest GUI-thread blackout in the pad path
// (250-600 ms of bounded polled waits), and its triggers cluster under exactly the SIO2 load
// that causes read misses -- so every run and its cost go to the Debug-Colors HUD (#271/#272).
static int initializePad(struct pad_data_t *pad)
{
    u32 start = cpu_ticks();
    int rc = initializePadInner(pad);
    unsigned int costMs = (unsigned int)((cpu_ticks() - start) / CLOCKS_PER_MILISEC);

    padDiag.reinitRuns++;
    padDiag.reinitLastMs = costMs;
    if (costMs > padDiag.reinitMaxMs)
        padDiag.reinitMaxMs = costMs;
    return rc;
}

static void updatePadState(struct pad_data_t *pad, int state)
{ // To simplify processing, monitor only Disconnected, FindCTP1 & Stable states.
    if ((state == PAD_STATE_DISCONN) || (state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1))
        pad->state = state;
}

static u32 readLeftJoy(struct pad_data_t *pad, u32 pdata)
{
    u32 padData = pdata;
    int xDeadzone, yDeadzone;

    if ((pad->buttons.mode >> 4) == 0x07) {
        switch (gXSensitivity) {
            case 0:
                xDeadzone = 128;
                break;
            case 1:
                xDeadzone = 100;
                break;
            case 2:
                xDeadzone = 80;
                break;
            case 3:
                xDeadzone = 60;
                break;
            default:
                xDeadzone = 100;
        }

        switch (gYSensitivity) {
            case 0:
                yDeadzone = 128;
                break;
            case 1:
                yDeadzone = 100;
                break;
            case 2:
                yDeadzone = 80;
                break;
            case 3:
                yDeadzone = 60;
                break;
            default:
                yDeadzone = 100;
        }

        if (xDeadzone < 128) {
            if (pad->buttons.ljoy_h < 127 - xDeadzone)
                padData |= PAD_LEFT;
            else if (pad->buttons.ljoy_h > 127 + xDeadzone)
                padData |= PAD_RIGHT;
        }

        if (yDeadzone < 128) {
            if (pad->buttons.ljoy_v < 127 - yDeadzone)
                padData |= PAD_UP;
            else if (pad->buttons.ljoy_v > 127 + yDeadzone)
                padData |= PAD_DOWN;
        }
    }

    return padData;
}

static int readPad(struct pad_data_t *pad)
{
    int rcode = 0;
    int oldState;
    int newState;
    int ret;
    int padsRead = 0;
    u32 newpdata = 0;

    oldState = pad->state;
    newState = padGetState(pad->port, pad->slot);
    updatePadState(pad, newState);

    if (oldState == PAD_STATE_DISCONN && isPadReadyState(pad->state)) {
        // Pad just connected.
        LOG("PAD pad %d,%d connected\n", pad->port, pad->slot);
        pad->analogCapable = -1;
        pad->analogRetryDelay = 0;
        padDiag.stateFlaps++;
        // Idle-gated (#271): this init is the same 250-600 ms inline blackout as the self-heal
        // below, and a reconnect edge can be DRIVEN by the very read errors that cluster under
        // load (sustained misses drop freepad to DISCONN, the recovery reconnects). Defer while
        // input is active: a digital-mode pad still reads every d-pad press, and the self-heal
        // branch below picks the init up on the first poll after a second of quiet.
        if ((u32)(curtime - lastInputActivityMs) >= PAD_SELF_HEAL_IDLE_MS)
            initializePad(pad);
        else
            padDiag.reinitDefers++;
    } else if (oldState != PAD_STATE_DISCONN && pad->state == PAD_STATE_DISCONN) {
        // The pad may transit from any state to disconnected. A real unplug is not a transient
        // miss: drop the held sample at once so a re-plug starts from a clean slate.
        LOG("PAD pad %d,%d disconnected\n", pad->port, pad->slot);
        pad->analogCapable = -1;
        pad->analogRetryDelay = 0;
        pad->paddata = 0;
    }

    if (isPadReadyState(pad->state)) {
        pollPadsReady++;
        ret = padRead(pad->port, pad->slot, &pad->buttons); // port, slot, buttons

        if (ret != 0) {
            newpdata = 0xffff ^ pad->buttons.btns;
            padsRead++;

            if ((pad->buttons.mode >> 4) == 0x07) {
                pad->analogCapable = 1;
                padDiag.analogCapable = 1;
                pad->analogRetryDelay = 0;
            } else if (pad->analogCapable != 0) {
                // freepad can temporarily return a pad to digital mode while recovering from a
                // read error. Retry forever with a backoff; a real digital-only controller is
                // disabled once its fully-published mode table proves DualShock mode absent.
                if (pad->analogRetryDelay > 0) {
                    pad->analogRetryDelay--;
                } else if (newpdata != 0 || (u32)(curtime - lastInputActivityMs) < PAD_SELF_HEAL_IDLE_MS) {
                    // User is actively providing input: DEFER the heal (see PAD_SELF_HEAL_IDLE_MS)
                    // -- initializePad here would block this thread 250-600 ms and eat the taps in
                    // flight. newpdata (THIS poll's sample) closes the one-poll staleness hole
                    // where the first press after a quiet spell lands while the activity stamp is
                    // still one poll old. Leaving the retry budget at 0 makes the heal fire on
                    // the first poll after a second of quiet.
                } else {
                    initializePad(pad);
                    pad->analogRetryDelay = PAD_ANALOG_RETRY_DELAY;
                }
            }
        }
    }

#ifdef PADEMU
    // Menu rumble on a ds34 pad rides this existing every-poll re-send, so it needs no RPC discipline
    // of its own and stops for free when the countdown expires. Params are (port, lrum, rrum).
    // Drive BOTH motors with the same level, matching the known-working reference (Enceladus'
    // lua_rumble does `ds34*_set_rumble(port, actAlign[1], actAlign[1])`). The previous version drove
    // only the light motor and the reporter felt nothing -- across DS3/DS4/DS5 the two motors differ in
    // kind (a DS3's light motor is on/off in hardware; a DualSense has no classic ERM at all), so
    // picking one and hoping is exactly how you ship silence.
    u8 rum = (gEnableRumble && pad->rumbleMsLeft > 0) ? (u8)pad->rumbleLevel : 0;

    if (ds34bt_get_status(pad->port) & DS34BT_STATE_RUNNING) {
        ret = ds34bt_get_data(pad->port, (u8 *)&pad->buttons.btns);
        ds34bt_set_rumble(pad->port, rum, rum);
        if (ret != 0) {
            newpdata |= 0xffff ^ pad->buttons.btns;
            padsRead++;
        }
    }

    if (ds34usb_get_status(pad->port) & DS34USB_STATE_RUNNING) {
        ret = ds34usb_get_data(pad->port, (u8 *)&pad->buttons.btns);
        ds34usb_set_rumble(pad->port, rum, rum);
        if (ret != 0) {
            newpdata |= 0xffff ^ pad->buttons.btns;
            padsRead++;
        }
    }
#endif

    if (padsRead > 0) {
        pollPadsRead++;
        newpdata = readLeftJoy(pad, newpdata);
        pad->paddata = newpdata;

        // merge into the global vars
        paddata |= pad->paddata;

        if (newpdata != 0x0) // something
            rcode = 1;
    } else {
        // no successful read: baseline behavior (do not carry state)
    }

    edgedata |= pad->paddata;

    return rcode;
}

/** Returns delay (in miliseconds) specified for the given key.
 * @param id The button id
 * @param repeat Boolean value specifying if we want initial key delay (0) or the repeat key delay (1)
 * @return the delay to the next key event
 */
static int getKeyDelay(int id, int repeat)
{
    int delay = paddelay[id - 1];

    // Initial press delay before auto-repeat begins (3x repeat delay).
    if (!repeat)
        delay *= 3;

    return delay;
}

// ---- Menu rumble (#172) ---------------------------------------------------------------------------
//
// A short tap on the pad when the cursor moves. Everything expensive was already in place: the pad is
// locked into DualShock mode and padSetActAlign() has enabled both engines -- we simply never fired
// padSetActDirect().
//
// Engine choice: BOTH engines. (This comment used to say "the small engine only, the big one is the
// wrong tool for a click" -- that reasoning lost to hardware. The reporter felt NOTHING from the small
// engine alone: a DS3's light motor is on/off in hardware and a DualSense has no classic ERM at all, so
// the big engine is the one you actually feel. See the level table at the top of this file.)
//
// Cost: padSetActDirect adds ZERO SIO2 traffic -- it is a SIF RPC that latches 6 bytes on the IOP,
// which freepad folds into the READ_DATA poll it already sends every vblank (the SIO2 frame length
// comes from the pad's mode, not the actuator payload). It IS a BLOCKING EE->IOP RPC though, and the
// menu's pad path otherwise issues none, so only ever send it on a CHANGE: ~2 RPCs per tap, none while
// idle. Never per frame.

// Rumble stays INERT until the GUI main loop is live (padRumbleActivate, called from main()).
//
// WHY THIS GATE EXISTS -- it fixes a real boot hang (#172), do not remove it:
// guiIntroLoop() calls screenHandler->handleInput() every frame but NEVER calls readPads(). paddata is
// therefore frozen on the single pre-intro read (opl.c, before the intro) and oldpaddata is still 0
// from BSS, so getKeyOn() -- (paddata & key) && !(oldpaddata & key) -- reports ANY button held at
// power-on as newly-pressed on EVERY intro frame, and menuHandleInputMenu fires sfxPlay(SFX_CURSOR)
// continuously for the whole boot.
// That was harmless for years because sfxPlay early-returns on !audio_initialized and audsrv is not up
// until deferredAudioInit (late in the IO FIFO) -- boot-time sfxPlay was a NO-OP. The rumble hook sits
// ABOVE that gate (deliberately: haptics must survive SFX being off), which turned that dormant path
// into a BLOCKING EE->IOP libpad RPC every RUMBLE_MIN_GAP_MS for the entire boot, concurrent with the
// IO worker's SifLoadModuleBuffer of USBMASS_BD/dev9/smap -> intermittent hang at "Loading USB storage
// driver...". Holding a button during boot made it MUCH more likely -- which is why the reporter's
// "hold START" attempt made things worse rather than better.
static int rumbleLive = 0;

// Called once from main() when the boot is done and guiMainLoop is about to start polling pads.
void padRumbleActivate(void)
{
    rumbleLive = 1;
}

// Previous rumble-arm timestamp in RAW ticks (see padRumbleArm) -- raw, not ms, so the
// difference is wrap-correct on the full 32-bit counter.
static u32 rumbleLastTicks = 0;
static int rumbleLastTicksValid = 0;

// Match the known-working native-pad behavior: a ready pad gets the command;
// controllers without motors simply ignore it.
static int padRumbleCapable(struct pad_data_t *pad)
{
    return isPadReadyState(pad->state);
}

// Drive a native PS2 pad's actuators. Returns 1 when the IOP accepted it. NOTE: the IOP silently
// DROPS this (returns 0) unless it is in TASK_UPDATE_PAD -- e.g. while initializePad's mode/align
// threads run -- so a caller that must not lose the command has to retry (see padRumbleStopAll).
static int padRumbleSendNative(struct pad_data_t *pad, int on)
{
    char act[6] = {0, 0, 0, 0, 0, 0};
    act[0] = (on && pad->rumbleSmall) ? 1 : 0; // small engine: on/off in HW; taps leave it OFF (RETRO parity)
    act[1] = on ? (char)pad->rumbleLevel : 0;  // big engine: the part you actually feel, 0..255
    // act[2..5] stay 0 -- padSetActAlign mapped only slots 0/1 to real actuators (the rest are 0xff =
    // unused), and the reference implementation passes 0 here too (a zeroed function-static).
    return padSetActDirect(pad->port, pad->slot, act);
}

static void padRumbleArm(int durationMs, unsigned char level, int smallEngine)
{
    int i;

    // rumbleLive: never issue a libpad RPC before the main loop is polling pads -- see the boot-hang
    // note at the top of this section. This MUST stay ahead of the gEnableRumble check.
    if (!rumbleLive || !gEnableRumble)
        return;

    // Held-repeat duty policy (RETRO parity; constants above). Measured on the SAME clock readPads()
    // ticks with. The interval is measured across EVERY arm attempt -- including suppressed ones --
    // so a fast held scroll stays silent for its whole run instead of strobing at the window edge.
    // Bumps (smallEngine) are decisions, not repeats: they always land at full length.
    // Raw ticks, differenced BEFORE dividing -- same wrap correctness as readPads(). Dividing first
    // put both operands in 0..29127 (cpu_ticks() wraps every ~29.13 s), so once per wrap `interval`
    // came out enormous and this arm was treated as isolated rather than a repeat.
    u32 nowRumbleTicks = cpu_ticks();
    u32 interval = (rumbleLastTicksValid) ? ((nowRumbleTicks - rumbleLastTicks) / CLOCKS_PER_MILISEC) : 0xFFFFFFFFu;
    rumbleLastTicks = nowRumbleTicks;
    rumbleLastTicksValid = 1;
    if (!smallEngine && interval < RUMBLE_REPEAT_WINDOW_MS) {
        int duty = (int)interval * RUMBLE_REPEAT_DUTY_PCT / 100;
        if (duty < RUMBLE_REPEAT_FLOOR_MS)
            return; // steps too fast to fit a felt pulse: silent, exactly like RETRO's fast scroll
        if (duty < durationMs)
            durationMs = duty; // ride each step at ~45% duty instead of merging into a grind
    }

    for (i = 0; i < pad_count; ++i) {
        struct pad_data_t *pad = &pad_data[i];

        // Arm the countdown for EVERY pad: a ds34 (DS3/4/5 over USB/BT) pad is not a native PS2 pad --
        // it never goes through padInfoAct/padSetActAlign -- and instead reads this straight off
        // readPad()'s existing every-poll re-send. Harmless on a pad that ends up rumbling nothing.
        pad->rumbleMsLeft = durationMs;
        pad->rumbleLevel = level; // both must be set BEFORE the kick below: padRumbleSendNative reads them
        pad->rumbleSmall = (unsigned char)(smallEngine ? 1 : 0);

        // Kick the native actuator immediately so the tap has no perceptible latency; readPads()
        // then RE-SENDS it every frame for the life of the tap. Do NOT "optimise" that re-send away:
        // freepad silently DROPS padSetActDirect whenever the IOP is not in TASK_UPDATE_PAD
        // (ps2sdk padMiscFuncs.c:294-296), and a single drop used to lose the whole 60ms tap. The
        // known-working reference (RETROLauncher) re-sends every frame and ignores the return
        // entirely, which is exactly why drops are invisible to it.
        if (padRumbleCapable(pad))
            padRumbleSendNative(pad, 1);
        pad->rumbleOn = 1; // owed an "off" regardless: the re-send below may well be what lands
    }
}

/** Nav thump for a cursor move (RETRO parity: ~240ms @ 0x50, big engine only; held-repeat rides each
 *  step at ~45% duty and fast scroll is silent). Safe from the GUI thread; never blocks and silently
 *  no-ops when disabled, suppressed by the duty policy, or the pad can't rumble. */
void padRumbleTap(void)
{
    padRumbleArm(RUMBLE_TAP_MS, RUMBLE_TAP_LEVEL, 0);
}

/** Game-list variant of padRumbleTap(). The two are IDENTICAL since the RETRO retune -- the old
 *  firmer/longer list split existed to compensate for a 60ms pulse dying inside the list's slow
 *  art-loading frames, which a 240ms thump no longer suffers. The seam is kept (sound.c still picks
 *  per screen off guiGetCurrentScreen) so re-splitting after HW feedback is a one-constant change. */
void padRumbleTapList(void)
{
    padRumbleArm(RUMBLE_TAP_MS, RUMBLE_TAP_LEVEL, 0);
}

/** Firmer bump for a confirm / cancel -- a decision should feel more definite than a scroll, so the
 *  bump KEEPS the small engine's crisp attack (taps are big-only now) and always lands at full
 *  length (the duty policy only clamps taps). RETRO doesn't rumble confirm at all; Blade explicitly
 *  asked for it and it is HW-validated, so it stays. On the LAUNCH edge the caller must follow this
 *  with padRumbleFlush(); see there for why. */
void padRumbleBump(void)
{
    padRumbleArm(RUMBLE_BUMP_MS, RUMBLE_LEVEL_BUMP, 1);
}

/** Play out any in-flight pulse, then stop the motors.
 *
 *  Call this before anything that blocks the GUI thread for a long time, because readPads() -- the ONLY
 *  thing that ticks the decay countdown -- stops running while it does. The launch path is the case that
 *  matters: between the confirm and deinitEx()'s stop sit menuLoadConfigDirect(), guiShowGameID()'s
 *  frame hold, and the whole of itemLaunch (sbPrepare, VMC superblock checks, cheats, fragment counting,
 *  and mmceSendGameID's card-switch wait, which alone can take seconds). Without this the confirm bump
 *  would run for that entire window -- a multi-second buzz instead of a 90ms tap.
 *
 *  Bounded by RUMBLE_BUMP_MS, i.e. at worst it adds ~90ms to a launch that already takes seconds. */
void padRumbleFlush(void)
{
    int i, waitMs = 0;

    for (i = 0; i < pad_count; ++i) {
        if (pad_data[i].rumbleMsLeft > waitMs)
            waitMs = pad_data[i].rumbleMsLeft;
    }

    if (waitMs > 0) {
        if (waitMs > RUMBLE_BUMP_MS)
            waitMs = RUMBLE_BUMP_MS; // belt + launch latency: a 240ms nav thump in flight is CUT here, deliberately
        DelayThread(waitMs * 1000);  // ms -> us
    }

    padRumbleStopAll();
}

/** Stop every actuator NOW and make sure it sticks. Call before anything that stops polling the pad
 *  (game launch / exit): padPortClose and padEnd do NOT clear actuators, so a motor left on keeps
 *  spinning straight into the game. */
void padRumbleStopAll(void)
{
    int i, polls;

    for (i = 0; i < pad_count; ++i) {
        struct pad_data_t *pad = &pad_data[i];

#ifdef PADEMU
        // ds34 pads are re-sent their rumble state every poll, so zeroing the state is enough -- but
        // once polling stops nothing re-sends, so push an explicit off too.
        ds34bt_set_rumble(pad->port, 0, 0);
        ds34usb_set_rumble(pad->port, 0, 0);
#endif
        pad->rumbleMsLeft = 0;

        if (!pad->rumbleOn)
            continue;

        // The IOP drops padSetActDirect unless it is in TASK_UPDATE_PAD, and the latched ON value
        // survives to be re-asserted on the next poll -- so a fire-and-forget off can be silently
        // lost. Retry on the same bounded budget waitPadReady uses. (padSetActDirect never raises
        // PAD_RSTAT_BUSY, so there is no request to wait on -- only the return value tells us.)
        for (polls = 0; polls < PAD_WAIT_POLLS; polls++) {
            if (padRumbleSendNative(pad, 0) == 1)
                break;
            DelayThread(PAD_WAIT_POLL_US);
        }
        if (polls == PAD_WAIT_POLLS)
            LOG("PAD rumble off was dropped for pad %d,%d\n", pad->port, pad->slot);
        pad->rumbleOn = 0;
    }
}

/** polling method. Call every frame. */
int readPads()
{
    int i;
    int result = 0;

    if (!edgeBaselineFrozen) {
        oldpaddata = paddata;
        oldedgedata = edgedata;
    }
    paddata = 0;
    edgedata = 0;

    /*
     * Difference the raw 32-bit clock before converting to milliseconds, then
     * carry the sub-millisecond remainder. This keeps repeat and rumble timing
     * correct across the ~29 second cpu_ticks() rollover.
     */
    u32 nowticks = cpu_ticks();
    if (!padTicksSeeded) {
        lastticks = nowticks;
        padTicksSeeded = 1;
    }

    u32 dticks = (nowticks - lastticks) + tickrem;
    lastticks = nowticks;
    time_since_last = dticks / CLOCKS_PER_MILISEC;
    tickrem = dticks % CLOCKS_PER_MILISEC;
    curtime += time_since_last;
    // Debug-Colors diag (HUD poll): worst poll period this session -- the spike that accompanies
    // an SIO2 stall (~16/17 ms at a steady 60 Hz).
    if (time_since_last > padDiag.pollMaxMs)
        padDiag.pollMaxMs = time_since_last;

    pollPadsReady = 0;
    pollPadsRead = 0;
    for (i = 0; i < pad_count; ++i)
        result |= readPad(&pad_data[i]);

    // Debug-Colors diag: a read MISS is a poll where a ready pad produced no fresh sample --
    // counted per-pad, so another pad (or a PADEMU ds34) reading fine does not hide it. These
    // counters feed the HUD "PAD miss:" line; they had no writers between the PR #328 revert
    // and this change, so the HUD showed miss:0 regardless of what the hardware did.
    if (pollPadsRead < pollPadsReady) {
        padDiag.readMisses++;
        padDiag.missBurst++;
        if (padDiag.missBurst > padDiag.missBurstMax)
            padDiag.missBurstMax = padDiag.missBurst;
    } else {
        padDiag.missBurst = 0;
    }

    // Stamp input activity AFTER the merge: any held button/stick re-arms the PAD_SELF_HEAL_IDLE_MS gate.
    if (paddata != 0)
        lastInputActivityMs = curtime;

    // Rumble duration is millisecond-based because some paths poll twice in one frame.
    for (i = 0; i < pad_count; ++i) {
        struct pad_data_t *pad = &pad_data[i];

        if (!isPadReadyState(pad->state)) {
            pad->rumbleOn = 0;
            pad->rumbleMsLeft = 0;
            continue;
        }

        if (pad->rumbleMsLeft > 0) {
            pad->rumbleMsLeft -= (int)time_since_last;
            if (pad->rumbleMsLeft > 0) {
                padRumbleSendNative(pad, 1);
                continue;
            }
            pad->rumbleMsLeft = 0;
        }
        if (!pad->rumbleOn)
            continue;

        if (padRumbleSendNative(pad, 0) == 1)
            pad->rumbleOn = 0;
    }

    // Simple baseline repeat handling (wOPL-style): decrement per-key counters when held,
    // otherwise reset to the initial delay. This removes read-miss carry/pausing behavior.
    for (i = 0; i < 16; ++i) {
        if (getKeyPressed(i + 1))
            delaycnt[i] -= (int)time_since_last;
        else
            delaycnt[i] = getKeyDelay(i + 1, 0);
    }

    return result;
}

/** Key getter with key repeats.
 * @param id The button ID
 * @return nonzero if button is being pressed just now
 */
int getKey(int id)
{
    if ((id <= 0) || (id >= 17))
        return 0;

    int kid = id - 1;

    // either the button was not pressed this frame, then reset counter and return
    // or it was, then handle the repetition
    if (getKeyOn(id)) {
        delaycnt[kid] = getKeyDelay(id, 0);
        KeyPressedOnce = 1;
        DisableCron = 1;
        return 1;
    }

    if (!getKeyPressed(id))
        return 0;

    if (delaycnt[kid] <= 0) {
        delaycnt[kid] = getKeyDelay(id, 1);
        KeyPressedOnce = 1;
        DisableCron = 1;
        return 1;
    }

    return 0;
}

/** Detects key-on event. Returns true if the button was not pressed the last frame but is pressed this frame.
 * @param id The button ID
 * @return nonzero if button is being pressed just now
 */
int getKeyOn(int id)
{
    if ((id <= 0) || (id >= 17))
        return 0;

    // old v.s. new pad data
    int keyid = keyToPad[id];

    return (edgedata & keyid) && (!(oldedgedata & keyid));
}

/** Detects key-off event. Returns true if the button was pressed the last frame but is not pressed this frame.
 * @param id The button ID
 * @return nonzero if button is being released
 */
int getKeyOff(int id)
{
    if ((id <= 0) || (id >= 17))
        return 0;

    // old v.s. new pad data
    int keyid = keyToPad[id];

    return (!(edgedata & keyid)) && (oldedgedata & keyid);
}

/** Returns true (nonzero) if the button is currently pressed
 * @param id The button ID
 * @return nonzero if button is being held
 */
int getKeyPressed(int id)
{
    if ((id <= 0) || (id >= 17))
        return 0;

    // old v.s. new pad data
    int keyid = keyToPad[id];

    return (paddata & keyid);
}

/** Sets the delay to wait for button repetition event to occur.
 * @param button The button ID
 * @param btndelay The button delay (in query count)
 */
void setButtonDelay(int button, int btndelay)
{
    if ((button <= 0) || (button >= 17))
        return;

    paddelay[button - 1] = btndelay;
}

int getButtonDelay(int button)
{
    if ((button <= 0) || (button >= 17))
        return 0;

    return paddelay[button - 1];
}

/** Unloads a single pad.
 * @see unloadPads */
static void unloadPad(struct pad_data_t *pad)
{
    padPortClose(pad->port, pad->slot);
}

/** Unloads all pads. Use to terminate the usage of the pads. */
void unloadPads()
{
    int i;

    // Backstop before ANY port closes: padPortClose/padEnd do NOT clear the actuators -- polling just
    // stops and the pad keeps whatever it was last told, i.e. it buzzes forever. The primary stop is at
    // deinitEx() entry; this covers the callers that unload pads without going through it.
    padRumbleStopAll();

    for (i = 0; i < pad_count; ++i)
        unloadPad(&pad_data[i]);

    padEnd();
}

/** Tries to start a single pad.
 * @param pad The pad data holding structure
 * @return 0 Error, != 0 Ok */
static int startPad(struct pad_data_t *pad)
{
    int newState;

    if (padPortOpen(pad->port, pad->slot, pad->padBuf) == 0) {
        return 0;
    }

    initializePad(pad);

    newState = waitPadReady(pad);
    updatePadState(pad, newState);
    return 1;
}

/** Starts all pads.
 * @return Count of dual shock compatible pads. 0 if none present. */
int startPads()
{
    // scan for pads that exist... at least one has to be present
    pad_count = 0;

    int maxports = padGetPortMax();

    int port; // 0 -> Connector 1, 1 -> Connector 2
    int slot; // Always zero if not using multitap

    for (port = 0; port < maxports; ++port) {
        int maxslots = padGetSlotMax(port);

        for (slot = 0; slot < maxslots && pad_count < MAX_PADS; ++slot) {

            struct pad_data_t *cpad = &pad_data[pad_count]; /* guard: pad_count < MAX_PADS asserted above */

            cpad->port = port;
            cpad->slot = slot;
            cpad->state = PAD_STATE_DISCONN;

            if (startPad(cpad))
                ++pad_count;
        }

        if (pad_count >= MAX_PADS)
            break; // enough already!
    }

    int n;
    for (n = 0; n < 16; ++n) {
        delaycnt[n] = DEFAULT_PAD_DELAY;
        paddelay[n] = DEFAULT_PAD_DELAY;
    }

    return pad_count;
}

void padStoreSettings(int *buffer)
{
    int i;

    for (i = 0; i < 16; i++)
        buffer[i] = paddelay[i];
}


void padRestoreSettings(int *buffer)
{
    int i;

    for (i = 0; i < 16; i++)
        paddelay[i] = buffer[i];
}
