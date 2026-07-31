#ifndef __SOUND_H
#define __SOUND_H

enum SFX {
    SFX_BOOT = 0,
    SFX_CANCEL,
    SFX_CONFIRM,
    SFX_CURSOR,
    SFX_MESSAGE,
    SFX_TRANSITION,
    SFX_BD_CONNECT,
    SFX_BD_DISCONNECT,

    SFX_COUNT
};

void audioInit(void);
void audioEnd(void);
void audioSetVolume(void);

int sfxInit(int bootSnd);
int sfxGetSoundDuration(int id);
void sfxPlay(int id);
// Debug-Colors instrumentation (#271/#272): wall time of the last / worst audsrv play RPC.
void sfxGetPlayDiag(unsigned int *lastMs, unsigned int *maxMs);

void bgmStart(void);
void bgmStop(void);
int isBgmPlaying(void);
void bgmMute(void);
void bgmUnMute(void);

#endif
