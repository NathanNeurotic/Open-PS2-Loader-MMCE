/*
  Copyright 2009, Ifcaro, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/pad.h"
#include "include/diag.h"
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
    u32 oldpaddata;
    struct padButtonStatus buttons;

    // pad_dma_buf is provided by the user, one buf for each pad
    // contains the pad's current state
    char padBuf[256] __attribute__((aligned(64)));

    char actAlign[6];
    int actuators;
    int analogCapable; // -1 unknown/not ready, 0 digital-only, 1 DualShock-capable
    int analogRetryDelay;

    // Menu rumble (#172). actuators != 0 only says the pad HAS motors -- padSetActAlign can still fail
    // or be skipped by initializePad's early returns, so latch the confirmed alignment separately and
    // require it before ever driving an actuator.
    unsigned char actAligned;
    unsigned char rumbleOn;    // 1 = an "on" was sent that still owes its matching "off"
    unsigned char rumbleLevel; // big-engine level for the pulse in flight (0 = none armed)
    unsigned char rumbleSmall; // 1 = drive the small engine too (bumps); taps are big-ERM-only
    int rumbleMsLeft;          // ms remaining; ticked down in readPads() (see the ms-vs-frames note there)
    int realignDelay;          // backoff for the actuator-alignment self-heal (padRumbleRealign)
    u32 readMissMs;            // ms accumulated since the last successful read (see readPad's hold-carry)
};

/*
  How long a still-connected pad's held buttons may be carried across failed reads, in MILLISECONDS.

  Time, not frames: readPads() is called once per rendered frame, frames lengthen under exactly the
  SIO2 contention that causes the misses, and a few call sites poll it more than once per frame. A
  frame counter would therefore mean anything from 50 ms to several seconds. time_since_last is
  already computed for the rumble/repeat timers, so bounding in ms is free and honest.

  48 ms is chosen to sit UNDER the fastest key-repeat interval OPL ever uses for list movement
  (gScrollSpeed "fast" = 100 ms, gui.c). That matters because the carry keeps getKeyPressed() true,
  which keeps delaycnt counting down -- so if the window were longer than a repeat interval, a
  release that landed inside a miss burst could emit repeats the user never asked for, which is the
  very symptom this is meant to remove. Under the interval, at most one carried poll can sit inside
  any repeat cycle.

  Residual, stated rather than hidden: the colour picker runs a 1 ms repeat (dia.c), so a release
  inside a burst can still cost a couple of extra steps there. That is bounded, cosmetic, and
  reversible by the user, unlike the menu-wide input loss it replaces.
*/
#define PAD_READ_CARRY_MS 48

// Pad commands are asynchronous. Keep every wait bounded so a transient SIO2/pad error cannot hang the
// GUI thread, then retry DualShock recovery periodically for as long as the controller remains digital.
#define PAD_WAIT_POLLS          25
#define PAD_WAIT_POLL_US        1000
#define PAD_ANALOG_RETRY_DELAY  60
// Frames between actuator-alignment re-arm attempts (padRumbleRealign). Only ever runs while a pad
// has NOT been aligned, so on a healthy pad it fires once and never again.
#define PAD_REALIGN_RETRY_DELAY 60
// Frames to wait after a FAILED re-arm. Much longer, because the failure path already burned up to
// PAD_REQ_WAIT_POLLS ms (150ms) inside waitPadRequestComplete on the GUI thread -- retrying that
// once a second on a pad that never aligns would stutter the menu 150ms/sec forever.
#define PAD_REALIGN_FAIL_DELAY  600
// A padSetMainMode round-trip can NEVER finish under freepad's own minimum latency: the IOP main
// thread dispatches the task on the next vblank, SetMainModeThread needs three vblank-gated SIO2
// transfers, and the request only flips COMPLETE after the next good ReadData -- >= 5 vblanks,
// ~83-100 ms. The generic 25 ms budget above therefore ALWAYS timed out on this leg, so analog
// arming worked only when the IOP happened to finish in the background and the pressure/rumble
// setup below it was unreachable. Give request-completion waits a budget above the happy path.
#define PAD_REQ_WAIT_POLLS      150

#define PAD_INIT_RETRY       -1
#define PAD_INIT_UNSUPPORTED 0
#define PAD_INIT_OK          1

/// current time in miliseconds (last update time)
static u32 curtime = 0;
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

    gDiag.padReqTimeout++; // PAD HUD TO: request never left BUSY within the budget
    LOG("PAD pad %d,%d request timed out\n", pad->port, pad->slot);
    return 0;
}

static int initializePad(struct pad_data_t *pad)
{
    int tmp;
    int modes;
    int i;
    int state;

    LOG("PAD initializing pad %d,%d\n", pad->port, pad->slot);

    // Menu rumble state belongs to the CURRENT connection: a re-init (fresh pad, or the analog
    // self-heal firing on a transient digital report) invalidates it. Clear it before the alignment
    // below re-proves itself -- otherwise a pad unplugged mid-tap comes back with rumbleOn stuck at 1
    // and padRumbleArm()'s "already on, don't re-send" guard would skip it forever.
    pad->actAligned = 0;
    pad->rumbleOn = 0;
    pad->rumbleMsLeft = 0;
    // Clear the HUD mirrors HERE too, not just where they are set below. Every early return between
    // this point and the actuator block would otherwise leave AN/AK showing a PREVIOUS pad's values,
    // and the actuators==0 branch never touches AK at all -- so a stale AK:1 could survive onto a pad
    // that was never aligned. A diagnostic that lies is worse than no diagnostic (Gemini review, #176).
    gDiag.padActuators = 0;
    gDiag.padActAligned = 0;

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
        // Snapshot BEFORE bailing so a PERSISTENT empty-table failure shows md:0 on the PAD HUD
        // instead of a stale last-good snapshot masking exactly the state under investigation.
        gDiag.padModes = modes;
        gDiag.padMode0 = -1;
        gDiag.padMode1 = -1;
        LOG("PAD mode table is not ready (or controller is digital-only)\n");
        return PAD_INIT_RETRY;
    }

    // Verify that the controller has a DUAL SHOCK mode. CRITICAL: freepad's query threads have a
    // 10-attempt budget per stage, and under sustained SIO2 contention (MMCE/MX4SIO boot traffic)
    // they can publish numModes > 0 while the mode TABLE entries were never written (BSS zero).
    // Legal CTP mode types are 2..7 and freepad only writes an entry on a validated controller
    // response, so a 0/invalid entry PROVES the table is half-built -- that must stay RETRYABLE.
    // The old code latched analogCapable=0 ("digital-only") off exactly that half-built table,
    // permanently disarming the self-heal (its gate is analogCapable != 0) until a physical
    // unplug/replug reset it via the DISCONN edge: the intermittent dead-analog bug (Nathan +
    // FifthFox + 2 testers). Only a FULLY-published table lacking DUALSHOCK may latch digital-only.
    int sawInvalid = 0;
    int hasDualshock = 0;
    for (i = 0; i < modes; i++) {
        tmp = padInfoMode(pad->port, pad->slot, PAD_MODETABLE, i);
        if (tmp == PAD_TYPE_DUALSHOCK)
            hasDualshock = 1;
        else if (tmp == 0)
            sawInvalid = 1; // freepad writes entries only from validated responses -> 0 = unwritten slot
    }
    gDiag.padModes = modes; // PAD HUD md/t0/t1: last mode-table snapshot
    gDiag.padMode0 = (modes > 0) ? padInfoMode(pad->port, pad->slot, PAD_MODETABLE, 0) : -1;
    gDiag.padMode1 = (modes > 1) ? padInfoMode(pad->port, pad->slot, PAD_MODETABLE, 1) : -1;

    if (!hasDualshock) {
        if (sawInvalid) {
            LOG("PAD mode table half-built (contention?) -- retrying, NOT latching digital-only\n");
            return PAD_INIT_RETRY; // analogCapable stays -1/previous: the self-heal keeps trying
        }
        LOG("PAD This is no Dual Shock controller\n");
        gDiag.padUnsupported++; // PAD HUD UN: genuine fully-published digital-only verdicts
        pad->analogCapable = 0;
        gDiag.padAnalogCapable = 0; // PAD HUD AC
        return PAD_INIT_UNSUPPORTED;
    }
    pad->analogCapable = 1;
    gDiag.padAnalogCapable = 1; // PAD HUD AC

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
    gDiag.padSetMainModeOk++; // PAD HUD SM: padSetMainMode accepted AND observed complete

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
    gDiag.padActuators = pad->actuators; // #172 diag: AN (0 = the pad claims no motors)

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
        if (tmp == 1) {
            if (waitPadRequestComplete(pad))
                pad->actAligned = 1; // accepted AND observed complete -- the only gate rumble trusts
            else
                LOG("PAD padSetActAlign request failed\n");
        }
        gDiag.padActAligned = pad->actAligned; // #172 diag: AK (0 = alignment never landed -> no rumble)
    } else {
        LOG("PAD Did not find any actuators.\n");
    }

    waitPadReady(pad);
    return PAD_INIT_OK;
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
                xDeadzone = 100;
                break;
            case 1:
                xDeadzone = 80;
                break;
            case 2:
                xDeadzone = 60;
                break;
            default:
                xDeadzone = 80;
        }

        switch (gYSensitivity) {
            case 0:
                yDeadzone = 100;
                break;
            case 1:
                yDeadzone = 80;
                break;
            case 2:
                yDeadzone = 60;
                break;
            default:
                yDeadzone = 80;
        }

        if (pad->buttons.ljoy_h < 127 - xDeadzone)
            padData |= PAD_LEFT;
        else if (pad->buttons.ljoy_h > 127 + xDeadzone)
            padData |= PAD_RIGHT;

        if (pad->buttons.ljoy_v < 127 - yDeadzone)
            padData |= PAD_UP;
        else if (pad->buttons.ljoy_v > 127 + yDeadzone)
            padData |= PAD_DOWN;
    }

    return padData;
}

static int readPad(struct pad_data_t *pad)
{
    int rcode = 0, oldState, newState, ret, padsRead;
    u32 newpdata = 0;

    padsRead = 0;
    oldState = pad->state;
    newState = padGetState(pad->port, pad->slot);
    updatePadState(pad, newState);
    if ((oldState == PAD_STATE_DISCONN) && ((pad->state == PAD_STATE_STABLE) || (pad->state == PAD_STATE_FINDCTP1))) {
        // Pad just connected.
        LOG("PAD pad %d,%d connected\n", pad->port, pad->slot);
        pad->analogCapable = -1;
        pad->analogRetryDelay = 0;
        pad->readMissMs = 0;
        initializePad(pad);
    }
    // The pad may transit from any state to disconnected. So check only for the disconnected state.
    else if ((oldState != PAD_STATE_DISCONN) && (pad->state == PAD_STATE_DISCONN)) {
        LOG("PAD pad %d,%d disconnected\n", pad->port, pad->slot);
        pad->analogCapable = -1;
        pad->analogRetryDelay = 0;
        // Drop any held buttons immediately on unplug rather than carrying them for the tolerance
        // window: a real disconnect is not a transient miss, and the state gate below already stops
        // contributing -- this just makes a re-plug start from a clean slate.
        pad->readMissMs = 0;
        pad->paddata = 0;
        pad->oldpaddata = 0;
    }

    if ((pad->state == PAD_STATE_STABLE) || (pad->state == PAD_STATE_FINDCTP1)) {
        // pad is connected. Read pad button information.
        ret = padRead(pad->port, pad->slot, &pad->buttons); // port, slot, buttons

        if (ret != 0) {
            newpdata = 0xffff ^ pad->buttons.btns;
            padsRead++;

            if ((pad->buttons.mode >> 4) == 0x07) {
                pad->analogCapable = 1;
                pad->analogRetryDelay = 0;
            } else if (pad->analogCapable != 0) {
                // freepad can temporarily return a pad to digital mode while recovering from a read error.
                // Retry forever with a backoff; a real digital-only controller is disabled once its non-empty
                // mode table proves that DualShock mode is absent.
                if (pad->analogRetryDelay > 0) {
                    pad->analogRetryDelay--;
                } else {
                    gDiag.padSelfHeal++; // PAD HUD SH: analog self-heal re-init attempts
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
        pad->readMissMs = 0;

        newpdata = readLeftJoy(pad, newpdata);

        if (newpdata != 0x0) // something
            rcode = 1;
        else
            rcode = 0;

        pad->oldpaddata = pad->paddata;
        pad->paddata = newpdata;

        // merge into the global vars
        paddata |= pad->paddata;
    } else if ((pad->state == PAD_STATE_STABLE || pad->state == PAD_STATE_FINDCTP1) &&
               pad->paddata != 0 && pad->readMissMs < PAD_READ_CARRY_MS) {
        /*
          TRANSIENT READ MISS -- carry the held buttons forward (#272, and the scroll-skip half of #271).

          padRead() returns 0 whenever freepad has nothing fresh, which on real hardware happens
          regularly: the pads share the SIO2 bus with the memory-card slots and MMCE, so cover-art and
          config traffic makes reads intermittently come back empty. That is why this only shows up on
          console and never in an emulator, and why it gets worse the more games (and therefore art
          loads) are in the list.

          Before this, a miss simply skipped the merge below, and since readPads() zeroes the global
          `paddata` every frame, the pad's held bits vanished for that frame. Two bugs fell out of it,
          both of which were reported as separate issues:

            1. getKeyPressed() read false, so readPads()' loop reset delaycnt to the FULL initial delay.
               Holding a direction re-armed the initial delay on every miss, so the auto-repeat leg
               often never fired at all -- "sluggish and heavy, I have to press the D-pad several times
               to move one item" (#272).
            2. The next successful frame saw the bit set with the previous frame clear, so getKeyOn()
               fired a fresh EDGE. One physical hold produced extra discrete moves -- the "scrolling
               skips 2 to 3 games" half of #271.

          Carrying pad->paddata into the GLOBAL paddata fixes both: getKeyPressed() stays true so the
          repeat timer keeps running, and the bit never disappears, so the recovery frame is not an
          edge. (Note the mechanism is the global staying set -- NOT pad->oldpaddata, which is written
          but never read anywhere in the tree. Do not "simplify" this by clearing pad->paddata on a
          miss in the belief that pad->oldpaddata protects the edge; it does not.)

          Bounded three ways, because this synthesises input and must never invent a hold the user is
          not performing:
            - only while the pad still reports a ready state (an unplug drops to PAD_STATE_DISCONN and
              stops contributing at once);
            - only while something was actually held -- carrying an all-zero paddata is pointless and
              would just keep a released pad "alive";
            - only for PAD_READ_CARRY_MS of accumulated miss time, which is deliberately shorter than
              the fastest repeat interval so a release inside a burst cannot manufacture list movement.

          rcode is set for the same reason a real read sets it: guiReadPads() treats readPads() == 0 as
          "user idle" and uses it to release background art/config IO (gui.c, guiInactiveFrames). A
          carried hold that reported idle would schedule card traffic into precisely the SIO2 stall
          this branch exists to ride out -- making the stall worse and the fix self-defeating.
        */
        pad->readMissMs += time_since_last;
        paddata |= pad->paddata;
        rcode = 1;
    }

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

    // if not in repeat, the delay is enlarged
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

// Gate a rumble command. Deliberately as PERMISSIVE as the known-working reference.
//
// HISTORY -- do not re-tighten this without hardware proof (#172):
// This originally also required analogCapable == 1, actuators > 0, and a home-grown actAligned flag
// (set only when padSetActAlign returned 1 AND waitPadRequestComplete observed it). That was
// defensive-looking and shipped ZERO vibration on real hardware: any one of the three failing
// silently means padSetActDirect is never called at all, with no error anywhere.
//   actAligned was the worst of them -- OUR invention, latched off a request-completion wait that we
// ALREADY KNOW is flaky on metal (we ship a TO: counter on the debug HUD precisely because these
// waits time out, PR #151). One timeout during init and rumble is dead for the whole session.
// Enceladus (DanielSant0s), whose rumble the reporter confirms works on THIS console, gates on
// NOTHING but the pad state:
//     int state = padGetState(port, 0);
//     if ((state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1)) padSetActDirect(port, 0, act);
// It calls padSetActAlign, prints the result, and fires regardless. So do we now.
// A pad with no motors simply ignores the bytes -- the cost of being wrong here is nothing happening,
// which is exactly what the "safe" version delivered anyway.
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
    int r = padSetActDirect(pad->port, pad->slot, act);
    // #172 diag: RS climbing while the pad stays still means the IOP took the command and the
    // ENGINE/duration is wrong -- not our gating. RD counts IOP drops (outside TASK_UPDATE_PAD).
    // CAVEAT: RS is not proof the motor was driven. freepad's 600mA guard (CheckAirDirectTotal,
    // padMiscFuncs.c:265-292) can ZERO the actuator byte and still return 1, so a silent pad with RS
    // climbing has a third possible cause besides engine/duration: the IOP zeroed it on the budget.
    if (r == 1)
        gDiag.padRumbleSent++;
    else
        gDiag.padRumbleDropped++;
    return r;
}

// THE root cause of "rumble does nothing" (#172). Re-arm the actuator alignment if it never landed.
//
// freepad fills ee_actAlignData.data[0..5] with 0xFF on port open (ps2sdk padPortOpen.c:157-160), and
// padSetActAlign is the ONLY thing that ever overwrites it. If it never ran, padSetActDirect STILL
// RETURNS 1 -- it just latches bytes that map to actuator index 0xFF, i.e. nothing. Perfect silence,
// success return, and no error anywhere. (That is why the debug HUD would show AK:0 with RS climbing.)
//
// initializePad has several early returns between padSetMainMode and padSetActAlign, two of which
// report PAD_INIT_OK having skipped the alignment ("pressure/rumble setup can wait for reconnect").
// And it is unrecoverable on its own: startPad() discards initializePad's return, and the analog
// self-heal only re-inits while (buttons.mode >> 4) != 0x07. So if padSetMainMode LANDED but took
// longer than our PAD_REQ_WAIT_POLLS budget -- the likely case, since the happy path already needs
// ~83-100ms and this fork has known SIO2 contention from MMCE/art traffic -- analog works fine, the
// self-heal never fires, the alignment is NEVER retried, and rumble is dead for the whole session.
//
// The known-working reference (Enceladus/RETROLauncher) is structurally immune: it never checks
// padSetMainMode's return and its waitPadReady spins unbounded, so it cannot reach padInfoAct/
// padSetActAlign in a bad state -- it simply waits. Rather than un-bound our waits (which would hand
// a wedged pad the power to freeze the GUI), retry the alignment lazily from the poll loop.
//
// Rate-limited, and only while the pad is ready and actually has actuators, so a digital-only pad
// costs nothing. Runs on the GUI thread like every other libpad call here.
static void padRumbleRealign(struct pad_data_t *pad)
{
    // SAME gate as padRumbleArm, and it MUST stay first. padSetActAlign below is a libpad RPC and
    // waitPadRequestComplete blocks the GUI thread for up to PAD_REQ_WAIT_POLLS ms (150ms).
    // readPads() runs during BOOT, so without rumbleLive this fires exactly the boot-time
    // RPC-vs-SifLoadModuleBuffer race that #176 exists to prevent -- and without gEnableRumble it
    // would do it for EVERY user on the DEFAULT config, for a feature they never switched on.
    if (!rumbleLive || !gEnableRumble)
        return;
    if (pad->actAligned)
        return;
    if (pad->realignDelay > 0) {
        pad->realignDelay--;
        return;
    }
    pad->realignDelay = PAD_REALIGN_RETRY_DELAY;

    if (padGetReqState(pad->port, pad->slot) == PAD_RSTAT_BUSY)
        return; // a request is already in flight -- do not stomp it; try again later

    // Re-query the actuator count HERE rather than trusting pad->actuators, and rebuild actAlign
    // rather than trusting it was ever filled. initializePad's early returns can bail BEFORE
    // padInfoAct ever runs -- which is EXACTLY the case this self-heal exists for -- leaving
    // actuators at 0 and actAlign never populated. Guarding on pad->actuators would therefore
    // disable the rescue precisely when it is needed (Gemini review, #179). padInfoAct reads the
    // DMA'd pad buffer on the EE, so it costs no RPC.
    pad->actuators = padInfoAct(pad->port, pad->slot, -1, 0);
    gDiag.padActuators = pad->actuators;
    if (pad->actuators <= 0)
        return; // digital-only pad, or not ready yet: nothing to align

    pad->actAlign[0] = 0; // small engine
    pad->actAlign[1] = 1; // big engine
    pad->actAlign[2] = 0xff;
    pad->actAlign[3] = 0xff;
    pad->actAlign[4] = 0xff;
    pad->actAlign[5] = 0xff;

    if (padSetActAlign(pad->port, pad->slot, pad->actAlign) == 1 && waitPadRequestComplete(pad)) {
        pad->actAligned = 1;
        gDiag.padActAligned = 1;
        gDiag.padRealignOk++;
        LOG("PAD actuator alignment re-armed for pad %d,%d\n", pad->port, pad->slot);
        return;
    }

    // FAILED: back off hard. waitPadRequestComplete above blocks for up to PAD_REQ_WAIT_POLLS ms
    // (150ms) before giving up, so retrying once a second on a pad that never aligns would stutter
    // the GUI by 150ms EVERY SECOND, forever. Rumble is a nicety; a smooth menu is not.
    pad->realignDelay = PAD_REALIGN_FAIL_DELAY;
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

    gDiag.padRumbleArmed++; // #172 diag: RA past gEnableRumble + the rate limit (see include/diag.h)

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
    oldpaddata = paddata;
    paddata = 0;

    /*
      Elapsed time since the last poll, in ms.

      Difference the RAW tick counter, then divide. The old form divided FIRST
      (cpu_ticks() / CLOCKS_PER_MILISEC, then subtract), which put both operands in 0..29127 --
      cpu_ticks() is a 32-bit counter at 147.456 MHz, so it wraps every ~29.13 s and the DIVIDED
      values wrap with it. u32 modular subtraction is only wrap-correct on the full-width counter,
      so on the rollover poll time_since_last became ~4,294,938,169 instead of ~16.

      That poisoned all three consumers below:
        - delaycnt[i] -= time_since_last  (int minus u32 promotes to unsigned) ADDED ~29,127 ms, so
          getKey()'s only repeat exit (delaycnt <= 0) was unreachable until the key was released.
        - rumbleMsLeft -= (int)time_since_last SUBTRACTS ~-29,127, turning a 240 ms nav tap into a
          ~29 second continuous buzz with the motor re-asserted every frame.
        - the same divided-value wrap in padRumbleArm (now rumbleLastTicks).

      The remainder is CARRIED rather than discarded. Differencing absolute ms was drift-free by
      construction (16/17 alternating at 60 Hz); dividing a delta and dropping the sub-ms remainder
      would lose ~0.5 ms of every ~16.7 ms frame, i.e. make every repeat ~3% slow and every rumble
      tap correspondingly short. Carrying it keeps the old timing exactly.

      First poll: lastticks is seeded on use so an arbitrary power-on tick value cannot present as
      one enormous delta.
    */
    u32 nowticks = cpu_ticks();
    if (!padTicksSeeded) {
        lastticks = nowticks;
        padTicksSeeded = 1;
    }
    u32 dticks = (nowticks - lastticks) + tickrem; // wrap-correct: full-width counter
    lastticks = nowticks;
    time_since_last = dticks / CLOCKS_PER_MILISEC;
    tickrem = dticks % CLOCKS_PER_MILISEC;
    curtime += time_since_last;

    int rslt = 0;

    for (i = 0; i < pad_count; ++i) {
        rslt |= readPad(&pad_data[i]);
    }

    // Expire any rumble tap. Deliberately ms-based off time_since_last rather than a frame counter:
    // a few call sites poll readPads() twice within one frame to flush input, which would make a
    // frame counter double-decrement and cut the tap short. The ms delta is immune (the second call
    // sees ~0ms). One RPC to switch the motor off, then never again until the next tap.
    for (i = 0; i < pad_count; ++i) {
        struct pad_data_t *pad = &pad_data[i];

        // A pad that is gone will never accept anything -- and retrying would fire a BLOCKING RPC at
        // it EVERY frame, forever. Drop the state: freepad re-arms its own actuator bytes on
        // padPortOpen, so a reconnecting pad cannot come back still buzzing.
        if (!isPadReadyState(pad->state)) {
            pad->rumbleOn = 0;
            pad->rumbleMsLeft = 0;
            continue;
        }

        padRumbleRealign(pad); // self-heal the alignment (see there -- this is what makes rumble work)

        if (pad->rumbleMsLeft > 0) {
            pad->rumbleMsLeft -= (int)time_since_last;
            if (pad->rumbleMsLeft > 0) {
                // RE-SEND the ON every frame for the life of the tap. freepad DROPS padSetActDirect
                // whenever the IOP is not in TASK_UPDATE_PAD (ps2sdk padMiscFuncs.c:294-296) and says
                // nothing, so a single drop used to lose the entire tap. The known-working reference
                // (RETROLauncher) re-sends every frame and discards the return -- drops simply cannot
                // matter to it. ~4 RPCs per 60ms tap, and ZERO while idle.
                padRumbleSendNative(pad, 1);
                continue;
            }
            pad->rumbleMsLeft = 0;
        }
        if (!pad->rumbleOn)
            continue;

        if (padRumbleSendNative(pad, 0) == 1)
            pad->rumbleOn = 0; // else retry next frame: the IOP briefly drops it outside TASK_UPDATE_PAD
    }

    for (i = 0; i < 16; ++i) {
        if (getKeyPressed(i + 1))
            delaycnt[i] -= time_since_last;
        else
            delaycnt[i] = getKeyDelay(i + 1, 0);
    }

    return rslt;
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

    return (paddata & keyid) && (!(oldpaddata & keyid));
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

    return (!(paddata & keyid)) && (oldpaddata & keyid);
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

    pad->analogCapable = -1;
    pad->analogRetryDelay = 0;
    // Seed the PAD HUD fields to "unknown" so a pre-init read of the diag line can't be mistaken
    // for a latched digital-only verdict (AC:0) or an empty mode table (md:0).
    gDiag.padAnalogCapable = -1;
    gDiag.padModes = -1;
    gDiag.padMode0 = -1;
    gDiag.padMode1 = -1;
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
