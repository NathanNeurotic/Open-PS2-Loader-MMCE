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
void unloadPads();

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

/** During a screen transition, seed and freeze the key-on baseline with the triggering sample.
 * This prevents the trigger from replaying on the destination while preserving a different button
 * that is still held when the destination appears. */
void padFreezeEdgeBaseline(int freeze);

/** Enables menu rumble after startup. Before this, the intro loop can emit SFX_CURSOR without
 * polling pads, so actuator commands must stay disabled. */
void padRumbleActivate(void);

/** Navigation thump and decision bump. Both no-op when controller vibration is disabled. */
void padRumbleTap(void);
void padRumbleTapList(void);
void padRumbleBump(void);

/** Plays out a short in-flight decision bump, then stops it before blocking the GUI thread. */
void padRumbleFlush(void);

/** Stops all pad actuators immediately before polling or pad ownership stops. */
void padRumbleStopAll(void);

/** Debug HUD: longest run of polls where the pad could not be READ (freepad not ready -> SIO2 or
 * IOP-side), and longest run of polls that read fine but carried no buttons (fresh, empty sample ->
 * the press never reached freepad). Whichever grows when an input is lost names the fault's half.
 */
void padGetFaultCounters(u32 *maxNotReady, u32 *maxReadyEmpty);

/** Debug HUD: reports detected actuators and alignment status for pad 0 and pad 1. */
void padGetActuatorDiag(int *act0, int *aligned0, int *act1, int *aligned1);

#endif
