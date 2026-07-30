#ifndef __OPL_DIAG_H
#define __OPL_DIAG_H

// Always-on, ship-in-release diagnostic counters for the MMCE #120 cascade investigation.
//
// Every serial/__DEBUG log path is compiled out on a stock retail PS2, and a log file on the MMCE card
// would itself write to the (possibly wedged) mmceman SIO2 bus we are trying to observe. So the ONLY
// diagnostic channel that reaches a hardware tester is the screen: these counters are bumped at existing
// choke points on whatever thread reaches them, and rendered as ONE line by the GUI (gui.c) every frame
// on the EE main thread -- proven alive during the cascade (the busy spinner keeps animating and the
// USB/Apps tabs stay live while MMCE art is wedged). The overlay only READS these ints; it issues no
// fileXio / SIO2 / card IO, so it adds ZERO traffic to the bus under investigation.
//
// Thread model: no field has more than one MEANINGFUL writer, and the GUI only reads. artOpens is bumped
// from BOTH the art worker AND the EE main thread (texDiscoverLoad also loads theme PNGs during thmLoad),
// so its non-atomic ++ can lose a rare update -- harmless for a delta-read diagnostic. memoHit/memoMiss are
// art-worker-only; artTerminate from the launch/theme path; vcdRescanPreserved/isoScanPreserved/lastSaveErrno
// from the deferred-IO (or config-write) path. Aligned 32-bit loads/stores are atomic on the EE, so a read
// is at most one frame stale (torn reads impossible) -- acceptable for diagnostics; no locks are taken.
//
// Reading the line (shown only when the user enables debug info, so it costs nothing in normal use). Read
// the counters as a DELTA while performing the repro (they are cumulative and some are not MMCE-exclusive):
//   AO artOpens          - EVERY texDiscoverLoad() entry: ISO + VCD art opens AND theme-element PNGs (theme
//                          loads spike it on a theme switch, NOT the MMCE bus). So don't read the absolute
//                          value -- watch whether AO keeps CLIMBING while parked on ONE PS1 info screen /
//                          scrolling a cover-less PS1 list: if it does, the miss-memo is NOT suppressing
//                          re-probes (epoch/key bug) and the storm is still live.
//   MH memoHit           - vcdLoadArt short-circuits with ZERO opens (memo working). This is the precise
//                          VCD-storm signal: AO plateauing while MH climbs == the storm is bounded.
//   MM memoMiss          - full VCD art misses recorded into the memo (first probe of each cover).
//   TK artTerminate      - **THE smoking gun.** The art watchdog TerminateThread'd the worker, which
//                          corrupts the shared mmceman RPC channel for the rest of the session. TK > 0
//                          means every reduce-the-opens fix is MOOT and we need a no-terminate / channel-
//                          reset fix instead. We have never been able to SEE this before.
//   Vp vcdRescanPreserved- a VCD (PS1) list rebuild kept its last-good list on a failed device read.
//                          NOTE: bumped for ANY VCD backend (mmce/bdm/eth/udpfs/hdd), so on a multi-device
//                          rig it is not MMCE-exclusive -- reliable as an MMCE signal only if MMCE is the
//                          rig's only VCD source (Andrew's case).
//   Ip isoScanPreserved  - an ISO (PS2) list rebuild kept its last-good list on a failed read (same all-
//                          backend caveat as Vp). Ip ticking up when the tester presses "show PS2 games"
//                          confirms the toggle is failing on the contended bus, not a handler that never fired.
//   SE lastSaveErrno     - errno of the last failed settings write (0 = none), latched at the actual write-
//                          failure site inside configWrite() (config.c). EIO/ENODEV here confirms the config
//                          save failed because the card itself is wedged, not a path problem.

typedef struct
{
    volatile unsigned int artOpens;
    volatile unsigned int memoHit;
    volatile unsigned int memoMiss;
    volatile unsigned int artTerminate;
    volatile unsigned int vcdRescanPreserved;
    volatile unsigned int isoScanPreserved;
    volatile int lastSaveErrno;
    // PAD line (intermittent dead-analog investigation, 2026-07-13; all writers on the EE main
    // thread via readPads/initializePad). Read when analog is dead:
    //   md/t0/t1 - last MODETABLE count + entries [0]/[1] seen by initializePad. md:2 with a zero
    //              t-entry = freepad published a HALF-BUILT table (the latch bug's trigger).
    //   AC       - last latched analogCapable verdict (0 = digital-only latched = dead analog).
    //   SH       - analog self-heal re-init attempts (climbing = retry loop alive).
    //   UN       - genuine fully-published digital-only verdicts (stays 0 for a real DualShock).
    //   SM       - padSetMainMode accepted AND observed complete (0 while SH climbs = IOP reject).
    //   TO       - request-completion waits that timed out (persistent TO = command-side contention).
    volatile int padModes;
    volatile int padMode0;
    volatile int padMode1;
    volatile int padAnalogCapable;
    volatile unsigned int padSelfHeal;
    volatile unsigned int padUnsupported;
    volatile unsigned int padSetMainModeOk;
    volatile unsigned int padReqTimeout;
    // RUMBLE sub-line (#172 "no vibration" on HW, 2026-07-15). Rumble is invisible from here -- these
    // split "we never asked" from "we asked and the motor ignored us", which no amount of source
    // reading can. Read with Debug ON + Vibration ON while moving the cursor:
    //   AN  - actuators reported by padInfoAct (0 = the pad claims no motors -> nothing we can do).
    //   AK  - actAligned: padSetActAlign accepted AND observed complete (0 = alignment never landed,
    //         so padRumbleCapable() gates every tap out and RS stays 0).
    //   RS  - padSetActDirect calls the IOP ACCEPTED (returned 1). Climbing while the pad stays still
    //         = the command lands and the ENGINE/duration is wrong, NOT our gating.
    //   RD  - padSetActDirect calls DROPPED (returned != 1; the IOP discards them outside
    //         TASK_UPDATE_PAD, e.g. during an initializePad re-init).
    //   RA  - taps ARMED by sfxPlay (padRumbleArm past gEnableRumble + the rate limit). RA:0 = the
    //         setting is off or the hook never fired; RA climbing with RS:0 = padRumbleCapable() is
    //         the blocker -- read AN/AK/AC to see which gate.
    //   RL  - actuator alignments RE-ARMED by the padRumbleRealign self-heal. RL>0 means initializePad
    //         had skipped/failed padSetActAlign and the poll loop rescued it -- which is the #172 bug:
    //         freepad leaves ee_actAlignData at 0xFF (= no actuator) until padSetActAlign lands, and
    //         padSetActDirect returns 1 regardless, so it silently drives nothing.
    volatile int padActuators;
    volatile int padActAligned;
    volatile unsigned int padRumbleSent;
    volatile unsigned int padRumbleDropped;
    volatile unsigned int padRumbleArmed;
    volatile unsigned int padRealignOk;
    // MISS sub-line (#272 settings-scroll freeze instrument, 2026-07-30; all writers on the EE main
    // thread inside readPads/readPad). This line is drawn by dia.c, because diaExecuteDialog renders
    // directly and never reaches guiDrawOverlays -- so the other lines are invisible in exactly the
    // dialogs where the #272 freeze repro lives. Read with Debug ON while holding a direction
    // through a hitch:
    //   PT  - max time_since_last (ms) seen by any readPads poll this session: the poll-period
    //         spike that accompanies an SIO2 stall (~16/17 at a steady 60 Hz).
    //   MB  - current consecutive-read-miss burst length in polls, max across pads.
    //   MX  - longest such burst this session. MX >= 4 at 60 Hz means a burst outlasted the 48 ms
    //         carry window -- the trigger the pause-on-miss fix in readPads() now covers.
    //   CD  - carry-drops: times a held button's hold fell out of the level view because readMissMs
    //         exceeded PAD_READ_CARRY_MS. On the pre-fix build, each CD was one ~900 ms re-arm.
    volatile unsigned int padPollMaxMs;
    volatile unsigned int padMissStreak;
    volatile unsigned int padMissStreakMax;
    volatile unsigned int padCarryDrops;
} opl_diag_t;

extern opl_diag_t gDiag;

#endif
