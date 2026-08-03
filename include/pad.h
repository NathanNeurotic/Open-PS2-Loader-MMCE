#ifndef __PAD_H
#define __PAD_H

// PAD handling

#define KEY_LEFT     1
#define KEY_DOWN     2
#define KEY_RIGHT    3
#define KEY_UP       4
#define KEY_START    5
#define KEY_R3       6
#define KEY_L3       7
#define KEY_SELECT   8
#define KEY_SQUARE   9
#define KEY_CROSS    10
#define KEY_CIRCLE   11
#define KEY_TRIANGLE 12
#define KEY_R1       13
#define KEY_L1       14
#define KEY_R2       15
#define KEY_L2       16

int startPads();
int readPads();
// During a screen transition, seed and freeze the key-on baseline with the triggering sample.
// This prevents the trigger from replaying on the destination while preserving a different button
// that is still held when the destination appears.
void padFreezeEdgeBaseline(int freeze);
void unloadPads();

// Debug-Colors instrumentation snapshot (#271/#272). Counters are cumulative and maintained on
// the EE main thread regardless of the setting; they are only RENDERED when Settings -> Debug
// Colors is on (gui.c / dia.c). Field semantics live with the writers in pad.c.
typedef struct
{
    unsigned int readMisses;   // polls where at least one ready-state pad produced no fresh sample
    unsigned int missBurst;    // current consecutive run of such polls (0 when all ready pads read)
    unsigned int missBurstMax; // session peak of the above
    unsigned int pollMaxMs;    // worst readPads() period this session
    unsigned int stateFlaps;   // DISCONN -> ready reconnect edges seen
    unsigned int reinitRuns;   // initializePad runs (reconnect edge + analog self-heal)
    unsigned int reinitDefers; // reconnect-edge inits deferred by the idle gate
    unsigned int reinitLastMs; // wall time of the last initializePad
    unsigned int reinitMaxMs;  // worst initializePad this session
    int analogCapable;         // last verdict (-1 unknown / 0 digital-only / 1 DualShock-capable)
} pad_diag_t;

void padGetDiag(pad_diag_t *out);

// Menu rumble (#172), gated by gEnableRumble. Tap/Bump arm a pulse on every capable pad and never
// block -- safe to call from the GUI thread; they no-op when disabled or the pad can't rumble.
// Rumble is INERT until this is called (once, from main(), when the boot is over and guiMainLoop is
// about to start polling pads). Boot-time libpad RPCs raced the IO worker's IOP module loads and hung
// the boot (#172) -- guiIntroLoop runs handleInput() against frozen paddata, so a button held at
// power-on made sfxPlay fire every frame. See the long note in pad.c.
void padRumbleActivate(void);

void padRumbleTap(void);     // light tick: cursor moved in a menu / dialog
void padRumbleTapList(void); // game-list cursor tick: identical to padRumbleTap since the RETRO retune (seam kept -- see pad.c)
void padRumbleBump(void);    // firmer: confirm / cancel

// Play out any in-flight pulse then stop. Call before anything that blocks the GUI thread for long,
// since readPads() -- which ticks the decay -- stops running then; the launch path would otherwise turn
// a 90ms bump into a multi-second buzz. Adds at most ~90ms.
void padRumbleFlush(void);

// Force-stop every actuator. MUST run before anything that stops polling the pad (game launch / exit):
// closing the pad ports does NOT clear the motors, so one left running keeps buzzing into the game.
void padRumbleStopAll(void);

int getKey(int num);

int getKeyOn(int num);
int getKeyOff(int num);
int getKeyPressed(int num);

/** Sets the repetition delay for the specified button
 * @param button id (KEY_XXX values)
 * @param btndelay the delay in miliseconds per repeat (clamped by framerate!) */
void setButtonDelay(int button, int btndelay);

/** Gets the repetition delay for the specified button */
int getButtonDelay(int button);


/** Store's the button delay into specified integer array (has to have 16 items) */
void padStoreSettings(int *buffer);

/** Restore's the button delay from specified integer array (has to have 16 items) */
void padRestoreSettings(int *buffer);

#endif
