#ifndef __GUIGAME_H
#define __GUIGAME_H

#define SETTINGS_GLOBAL        0
#define SETTINGS_PERGAME       1
// Per-key GSM inheritance: a key PRESENT in the game's own config wins, a key ABSENT follows the
// global block. Deliberately a NEW value: only 0 and 1 have ever been written (by this fork, by the
// upstream fork, and by every build in the field), so a 2 cannot appear in any config that already
// exists -- which is what lets the two legacy branches in gsm.c InitGSMConfig stay bit-identical.
// Do NOT retrofit this meaning onto SETTINGS_PERGAME: that would silently change games whose keys
// were stripped for being zero (the per-game save leg removes zero-valued keys, so "absent" there
// means BOTH "unset" and "explicitly 0").
#define SETTINGS_PERGAME_MIXED 2

int guiGameAltStartupNameHandler(char *text, int maxLen);

char *gameConfigSource(void);

int guiGameVmcNameHandler(char *text, int maxLen);
void guiGameShowVMCMenu(int id, item_list_t *support);
void guiGameShowCompatConfig(int id, item_list_t *support, config_set_t *configSet);
void guiGameShowGSConfig(int forceGlobal);
void guiGameShowCheatConfig(void);

#ifdef PADEMU
void guiGameShowPadEmuConfig(int forceGlobal);
void guiGameShowPadMacroConfig(int forceGlobal);
void guiGameSavePadEmuGlobalConfig(config_set_t *configGame);
void guiGameSavePadMacroGlobalConfig(config_set_t *configGame);
#endif

void guiGameShowOSDLanguageConfig(int forceGlobal);
void guiGameSaveOSDLanguageGlobalConfig(config_set_t *configGame);

void guiGameLoadConfig(item_list_t *support, config_set_t *configSet);
int guiGameSaveConfig(config_set_t *configSet, item_list_t *support);
void guiGameTestSettings(int id, item_list_t *support, config_set_t *configSet);

void guiGameRemoveSettings(config_set_t *configSet);
void guiGameRemoveGlobalSettings(config_set_t *configGame);
#endif
