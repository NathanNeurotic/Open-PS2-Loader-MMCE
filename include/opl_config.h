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

// Encapsulated Configuration Structures

typedef struct {
    int ps2_ip_use_dhcp;
    int ps2_ip[4];
    int ps2_netmask[4];
    int ps2_gateway[4];
    int ps2_dns[4];
    int eth_op_mode;
    int pc_share_is_netbios;
    int pc_ip[4];
    int pc_port;
    char pc_share_nb_addr[17];
    char pc_share_name[32];
    char pc_user_name[32];
    char pc_password[32];
    int network_startup;
    int eth_start_mode;
    char eth_prefix[32];
} OPL_NetworkConfig;

extern OPL_NetworkConfig gNetworkConfig;
extern char gExportName[32];

#endif
