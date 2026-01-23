#ifndef __OPL_CONFIG_H
#define __OPL_CONFIG_H

#include "config.h"

// Configuration handling logic (App layer)
int loadConfig(int types);
int saveConfig(int types, int showUI);
void applyConfig(int themeID, int langID, int skipDeviceRefresh);
void setDefaults(void);
void setDefaultColors(void);

// Config logic helpers possibly needed by other modules (like autolaunch)
int checkLoadConfigBDM(int types);
int checkLoadConfigHDD(int types);

#endif
