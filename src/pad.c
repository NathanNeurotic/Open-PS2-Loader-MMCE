/*
  Copyright 2009, Ifcaro, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/pad.h"
#include "include/ioman.h"
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
    int actAligned; // padSetActAlign confirmed; see padRumbleRealign()
};

/// current time in miliseconds (last update time)
static u32 curtime = 0;
static u32 time_since_last = 0;

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

/*
 * waitPadReady()
 */
static int waitPadReady(struct pad_data_t *pad)
{
    int state;

    // busy wait for the pad to get ready
    do {
        state = padGetState(pad->port, pad->slot);
    } while ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1) && (state != PAD_STATE_DISCONN));

    return state;
};

static int initializePad(struct pad_data_t *pad)
{
    int tmp;
    int modes;
    int i;

    LOG("PAD initializing pad %d,%d\n", pad->port, pad->slot);

    // Cleared up front because every path below can return before padInfoAct() is reached. Without
    // this, unplugging a DualShock and plugging a digital pad into the same port would leave the
    // old actuator count behind, and the rumble engine would drive engines that are not there.
    // actAligned goes with it: freepad resets its alignment to 0xFF on padPortOpen, so a stale 1
    // here would suppress the realign retry and every send would drive actuator index 0xFF.
    pad->actuators = 0;
    pad->actAligned = 0;

    // is there any device connected to that port?
    if (waitPadReady(pad) == PAD_STATE_DISCONN) {
        LOG("PAD pad %d,%d not connected.\n", pad->port, pad->slot);
        return 1; // nope, don't waste your time here!
    }

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

    // If modes == 0, this is not a Dual shock controller
    // (it has no actuator engines)
    if (modes == 0) {
        LOG("PAD This is a digital controller?\n");
        return 1;
    }

    // Verify that the controller has a DUAL SHOCK mode
    i = 0;
    do {
        if (padInfoMode(pad->port, pad->slot, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
            break;
        i++;
    } while (i < modes);

    if (i >= modes) {
        LOG("PAD This is no Dual Shock controller\n");
        return 1;
    }

    // If ExId != 0x0 => This controller has actuator engines
    // This check should always pass if the Dual Shock test above passed
    tmp = padInfoMode(pad->port, pad->slot, PAD_MODECUREXID, 0);
    if (tmp == 0) {
        LOG("PAD This is no Dual Shock controller??\n");
        return 1;
    }

    LOG("PAD Enabling dual shock functions\n");

    // When using MMODE_LOCK, user cant change mode with Select button
    padSetMainMode(pad->port, pad->slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);

    waitPadReady(pad);
    tmp = padInfoPressMode(pad->port, pad->slot);
    LOG("PAD infoPressMode: %d\n", tmp);

    waitPadReady(pad);
    tmp = padEnterPressMode(pad->port, pad->slot);
    LOG("PAD enterPressMode: %d\n", tmp);

    waitPadReady(pad);
    pad->actuators = padInfoAct(pad->port, pad->slot, -1, 0);
    LOG("PAD # of actuators: %d\n", pad->actuators);

    if (pad->actuators != 0) {
        pad->actAlign[0] = 0; // Enable small engine
        pad->actAlign[1] = 1; // Enable big engine
        pad->actAlign[2] = 0xff;
        pad->actAlign[3] = 0xff;
        pad->actAlign[4] = 0xff;
        pad->actAlign[5] = 0xff;

        waitPadReady(pad);
        tmp = padSetActAlign(pad->port, pad->slot, pad->actAlign);
        pad->actAligned = (tmp != 0);
        LOG("PAD padSetActAlign: %d\n", tmp);
    } else {
        LOG("PAD Did not find any actuators.\n");
    }

    waitPadReady(pad);

    return 1;
}

static void updatePadState(struct pad_data_t *pad, int state)
{ // To simplify processing, monitor only Disconnected, FindCTP1 & Stable states.
    if ((state == PAD_STATE_DISCONN) || (state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1))
        pad->state = state;
}

// Left-stick navigation: the stick is MERGED into the d-pad bits, so analog nav is always available
// and the X/Y Sensitivity rows are its control. Those rows offer FOUR options (Off/Low/Medium/High)
// -- this switch must have four cases to match. It previously had three (upstream's), so index 3
// ("High") fell into `default` and came out LESS sensitive than "Medium", and index 0 ("Off") mapped
// to a 100 deadzone that still navigated. A 128 deadzone cannot be exceeded by a 0..255 axis centred
// at 127, and the explicit `< 128` guards below make that intent unmistakable: Off really is off.
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
    int rcode = 0, oldState, newState, ret, padsRead;
    u32 newpdata = 0;

    padsRead = 0;
    oldState = pad->state;
    newState = padGetState(pad->port, pad->slot);
    updatePadState(pad, newState);
    if ((oldState == PAD_STATE_DISCONN) && ((pad->state == PAD_STATE_STABLE) || (pad->state == PAD_STATE_FINDCTP1))) {
        // Pad just connected.
        LOG("PAD pad %d,%d connected\n", pad->port, pad->slot);
        initializePad(pad);
    }
    // The pad may transit from any state to disconnected. So check only for the disconnected state.
    else if ((oldState != PAD_STATE_DISCONN) && (pad->state == PAD_STATE_DISCONN)) {
        LOG("PAD pad %d,%d disconnected\n", pad->port, pad->slot);
    }

    if ((pad->state == PAD_STATE_STABLE) || (pad->state == PAD_STATE_FINDCTP1)) {
        // pad is connected. Read pad button information.
        ret = padRead(pad->port, pad->slot, &pad->buttons); // port, slot, buttons

        if (ret != 0) {
            newpdata = 0xffff ^ pad->buttons.btns;
            padsRead++;
        }
    }

#ifdef PADEMU
    if (ds34bt_get_status(pad->port) & DS34BT_STATE_RUNNING) {
        ret = ds34bt_get_data(pad->port, (u8 *)&pad->buttons.btns);
        ds34bt_set_rumble(pad->port, 0, 0);
        if (ret != 0) {
            newpdata |= 0xffff ^ pad->buttons.btns;
            padsRead++;
        }
    }

    if (ds34usb_get_status(pad->port) & DS34USB_STATE_RUNNING) {
        ret = ds34usb_get_data(pad->port, (u8 *)&pad->buttons.btns);
        ds34usb_set_rumble(pad->port, 0, 0);
        if (ret != 0) {
            newpdata |= 0xffff ^ pad->buttons.btns;
            padsRead++;
        }
    }
#endif
    if (padsRead > 0) {
        if (newpdata != 0x0) // something
            rcode = 1;
        else
            rcode = 0;

        newpdata |= readLeftJoy(pad, newpdata);

        pad->oldpaddata = pad->paddata;
        pad->paddata = newpdata;

        // merge into the global vars
        paddata |= pad->paddata;
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

/*--    Menu rumble    ------------------------------------------------------------------------------
Opt-in (gEnableRumble). The pulse is timed against RAW cpu_ticks() elapsed since it started, and
NOT against time_since_last below -- that value is the difference of two ALREADY divided cpu_ticks()
readings, so it goes wild once every ~29.1 s when the 32-bit tick counter wraps. On a decrementing
"milliseconds left" counter a single bad sample adds ~29 s to the pulse: a motor latched on. An
elapsed comparison in raw ticks is correct across one wrap by ordinary unsigned arithmetic, and a
pulse is capped below at a fraction of a second, so one wrap is the most that can occur inside it.
This is why the feature needs none of the pad-clock rework it was originally blocked on.
----------------------------------------------------------------------------------------------------*/
static u32 rumbleStartTicks = 0;
static u32 rumbleDurTicks = 0;
static int rumbleActive = 0;
static int rumbleLarge = 0;

// Longest pulse we will ever hold a motor on for. Menu feedback, not a sustained buzz.
#define RUMBLE_MAX_MS 500

/* THE TWO THINGS THAT MAKE THIS SILENT ON REAL HARDWARE, both properties of freepad.irx -- which is
   what we ship as padman (Makefile: asm/padman.c is built from $(PS2SDK)/iop/irx/freepad.irx):

   1. freepad DROPS padSetActDirect unless its current task is TASK_UPDATE_PAD, and it still returns
      1 when it does. Sending once per pulse means one unlucky frame eats the whole pulse. So the
      command is RE-SENT every frame for the pulse's life. Do NOT "optimise" this back to send-once.

   2. freepad fills its actuator alignment with 0xFF on padPortOpen, and padSetActAlign is the only
      thing that overwrites it. padSetActDirect latches the bytes and returns 1 WITHOUT consulting
      the alignment -- so if padSetActAlign never ran, every send reports success and drives actuator
      index 0xFF, i.e. nothing at all. initializePad() has five bail-outs above padSetActAlign, so
      this is reachable; padRumbleRealign() below retries it.

   Neither shows up in a log or a return value. Both were found the expensive way. */

// Alignment confirmed for this pad. Tracked so it can be RETRIED -- never used to veto a send: an
// earlier fork revision gated sends on this and on a padInfoAct count, and any single init hiccup
// then killed rumble silently for the whole session.
static void padRumbleRealign(struct pad_data_t *pad)
{
    if (pad->actAligned || pad->actuators == 0)
        return;

    pad->actAlign[0] = 0; // small engine -> byte 0 of padSetActDirect
    pad->actAlign[1] = 1; // big engine   -> byte 1
    pad->actAlign[2] = 0xff;
    pad->actAlign[3] = 0xff;
    pad->actAlign[4] = 0xff;
    pad->actAlign[5] = 0xff;

    // No waitPadRequestComplete(): that blocks the GUI thread for up to ~150 ms, and this sits on
    // the path readPads() drives. A refused request just retries on the next pulse -- pulses are
    // user-driven and seconds apart, so an unbounded lazy retry costs nothing.
    if (padSetActAlign(pad->port, pad->slot, pad->actAlign))
        pad->actAligned = 1;
}

static void padActSet(struct pad_data_t *pad, int small, int large)
{
    char act[6];

    if (pad->actuators == 0)
        return;

    padRumbleRealign(pad);

    memset(act, 0, sizeof(act));
    act[0] = small ? 1 : 0;        // small engine: on/off only
    act[1] = (char)(large & 0xff); // large engine: 0..255
    // Fire-and-forget: padSetActDirect latches six bytes on the IOP and returns. Deliberately NOT
    // preceded by waitPadReady() -- that is a busy wait, and this runs on the GUI thread.
    padSetActDirect(pad->port, pad->slot, act);
}

/** Stops any pulse in flight, on every pad.
 * The decay only runs from readPads(), so anything that can block the GUI thread for longer than a
 * pulse has to stop the motor on the way in or it buzzes for the whole operation.
 */
void padRumbleFlush(void)
{
    int i;

    if (!rumbleActive)
        return;

    rumbleActive = 0;
    rumbleDurTicks = 0;
    rumbleLarge = 0;
    // Sent twice: the IOP LATCHES the actuator state, so a dropped "off" leaves the motor spinning
    // with nothing left to stop it. The "on" gets a re-send every frame; the "off" gets no frames.
    for (i = 0; i < pad_count; ++i)
        padActSet(&pad_data[i], 0, 0);
    for (i = 0; i < pad_count; ++i)
        padActSet(&pad_data[i], 0, 0);
}

/** Starts a rumble pulse on every connected pad that has actuators.
 * @param durationMs pulse length, clamped to RUMBLE_MAX_MS
 * @param large      large (variable-speed) engine strength, 0..255; the small engine runs for the
 *                   whole pulse regardless, since it is the one that gives a crisp click
 */
void padRumble(int durationMs, int large)
{
    int i;

    if (durationMs <= 0)
        return;
    if (durationMs > RUMBLE_MAX_MS)
        durationMs = RUMBLE_MAX_MS;
    if (large < 0)
        large = 0;
    if (large > 255)
        large = 255;

    rumbleStartTicks = cpu_ticks();
    rumbleDurTicks = (u32)durationMs * CLOCKS_PER_MILISEC;
    rumbleActive = 1;
    rumbleLarge = large;

    for (i = 0; i < pad_count; ++i)
        padActSet(&pad_data[i], 1, large);
}

/** polling method. Call every frame. */
int readPads()
{
    int i;
    oldpaddata = paddata;
    paddata = 0;

    // in ms.
    u32 newtime = cpu_ticks() / CLOCKS_PER_MILISEC;
    time_since_last = newtime - curtime;
    curtime = newtime;

    int rslt = 0;

    for (i = 0; i < pad_count; ++i) {
        rslt |= readPad(&pad_data[i]);
    }

    // Rumble decay + re-send. Unsigned elapsed in RAW ticks -- see the note above padActSet().
    // The re-send is not redundant: freepad drops padSetActDirect outside TASK_UPDATE_PAD and still
    // reports success, so a pulse sent only once can be silently swallowed whole.
    if (rumbleActive) {
        if ((u32)(cpu_ticks() - rumbleStartTicks) >= rumbleDurTicks) {
            padRumbleFlush();
        } else {
            for (i = 0; i < pad_count; ++i)
                padActSet(&pad_data[i], 1, rumbleLarge);
        }
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

    // Last chance to stop a motor. Closing the port does not clear the actuators, and OPL is on its
    // way out here -- a pulse still in flight would keep buzzing into whatever we hand off to.
    padRumbleFlush();

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

    if (!initializePad(pad)) {
        return 0;
    }

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

        for (slot = 0; slot < maxslots; ++slot) {

            struct pad_data_t *cpad = &pad_data[pad_count];

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
