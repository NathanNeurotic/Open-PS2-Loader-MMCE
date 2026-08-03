#include "include/dialogs.h"
#include "include/opl.h"
#include "include/dia.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/guigame.h"

#include <stdio.h>

// Network page (settings-layout restructure): Protocol/Access/SMB-Version moved here from Game
// Sources; the POPStarter network section moved out to POPStarter -> Network Settings (diaPopsNetConfig).
struct UIItem diaNetConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MENU_NETWORK}}},
    {UI_SPLITTER},

    // Unified network rows (moved from diaDeviceConfig): Protocol (SMB/UDPFS/UDPBD) and Access
    // (Files/IMG) qualify the Game Sources "Network Start Mode" row. netConfigUpdater greys Access
    // for SMB and locks it to IMG for UDPBD (same rules guiDeviceUpdater used to apply).
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Protocol", -1}}},
    {UI_SPACER},
    {UI_ENUM, CFG_NETPROTOCOL, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Access", -1}}},
    {UI_SPACER},
    {UI_ENUM, CFG_UDPFSMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"SMB Version", -1}}},
    {UI_SPACER},
    {UI_ENUM, CFG_SMBDIALECT, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SHOW_ADVANCED_OPTS}}},
    {UI_SPACER},
    {UI_BOOL, NETCFG_SHOW_ADVANCED_OPTS, 1, 1, -1, 0, 0, {.intvalue = {1, 0}}}, // RiptOPL: advanced ON by default (port/op-mode editable)
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ETH_OPMODE}}},
    {UI_SPACER},
    {UI_ENUM, NETCFG_ETHOPMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {"- PS2 -", -1}}},
    {UI_BREAK},

    // ---- IP address type ----
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_IP_ADDRESS_TYPE}}},
    {UI_SPACER},
    {UI_ENUM, NETCFG_PS2_IP_ADDR_TYPE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // ---- IP address ----
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_IP_ADDRESS}}},
    {UI_SPACER},
    {UI_INT, NETCFG_PS2_IP_ADDR_0, 1, 1, -1, 0, 0, {.intvalue = {192, 192, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_IP_ADDR_1, 1, 1, -1, 0, 0, {.intvalue = {168, 168, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_IP_ADDR_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_IP_ADDR_3, 1, 1, -1, 0, 0, {.intvalue = {10, 10, 0, 255}}},
    {UI_BREAK},

    //  ---- Netmask ----
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MASK}}},
    {UI_SPACER},
    {UI_INT, NETCFG_PS2_NETMASK_0, 1, 1, -1, 0, 0, {.intvalue = {255, 255, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_NETMASK_1, 1, 1, -1, 0, 0, {.intvalue = {255, 255, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_NETMASK_2, 1, 1, -1, 0, 0, {.intvalue = {255, 255, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_NETMASK_3, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_BREAK},

    //  ---- Gateway ----
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_GATEWAY}}},
    {UI_SPACER},
    {UI_INT, NETCFG_PS2_GATEWAY_0, 1, 1, -1, 0, 0, {.intvalue = {192, 192, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_GATEWAY_1, 1, 1, -1, 0, 0, {.intvalue = {168, 168, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_GATEWAY_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_GATEWAY_3, 1, 1, -1, 0, 0, {.intvalue = {1, 1, 0, 255}}},
    {UI_BREAK},

    //  ---- DNS server ----
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_DNS_SERVER}}},
    {UI_SPACER},
    {UI_INT, NETCFG_PS2_DNS_0, 1, 1, -1, 0, 0, {.intvalue = {192, 192, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_DNS_1, 1, 1, -1, 0, 0, {.intvalue = {168, 168, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_DNS_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_PS2_DNS_3, 1, 1, -1, 0, 0, {.intvalue = {1, 1, 0, 255}}},
    {UI_SPLITTER},

    //  ---- SMB Server ----
    {UI_LABEL, NETCFG_LBL_SMB_SERVER, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CAT_SMB_SERVER}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_LBL_SHARE_ADDR_TYPE, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ADDRESS_TYPE}}},
    {UI_SPACER},
    {UI_ENUM, NETCFG_SHARE_ADDR_TYPE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_LBL_SHARE_ADDRESS, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ADDRESS}}},
    {UI_SPACER},
    {UI_STRING, NETCFG_SHARE_NB_ADDR, 1, 0, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_INT, NETCFG_SHARE_IP_ADDR_0, 1, 1, -1, 0, 0, {.intvalue = {192, 192, 0, 255}}},
    {UI_LABEL, NETCFG_SHARE_IP_ADDR_DOT_0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_SHARE_IP_ADDR_1, 1, 1, -1, 0, 0, {.intvalue = {168, 168, 0, 255}}},
    {UI_LABEL, NETCFG_SHARE_IP_ADDR_DOT_1, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_SHARE_IP_ADDR_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, NETCFG_SHARE_IP_ADDR_DOT_2, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_SHARE_IP_ADDR_3, 1, 1, -1, 0, 0, {.intvalue = {1, 1, 0, 255}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_LBL_SHARE_PORT, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PORT}}},
    {UI_SPACER},
    {UI_INT, NETCFG_SHARE_PORT, 1, 1, -1, 0, 0, {.intvalue = {1111, 1111, 0, 65535}}}, // RiptOPL default SMB port 1111 (non-privileged; was 445)
    {UI_BREAK},

    {UI_BREAK},

    //  ---- SMB share name ----
    {UI_LABEL, NETCFG_LBL_SHARE_NAME, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SHARE}}},
    {UI_SPACER},
    {UI_STRING, NETCFG_SHARE_NAME, 1, 1, _STR_HINT_SHARENAME, 0, 0, {.stringvalue = {"PS2SMB", "PS2SMB", NULL}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_LBL_SHARE_USER, 1, 1, -1, -40, 0, {.label = {NULL, _STR_USER}}},
    {UI_SPACER},
    {UI_STRING, NETCFG_SHARE_USERNAME, 1, 1, -1, 0, 0, {.stringvalue = {"GUEST", "GUEST", NULL}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_LBL_SHARE_PASSWORD, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PASSWORD}}},
    {UI_SPACER},
    {UI_PASSWORD, NETCFG_SHARE_PASSWORD, 1, 1, _STR_HINT_GUEST, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_BREAK},
    {UI_BUTTON, NETCFG_OK, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_SPACER},
    {UI_BUTTON, NETCFG_RECONNECT, 1, 1, -1, 0, 0, {.label = {NULL, _STR_RECONNECT}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// POPStarter -> Network Settings (VCD over SMB). Moved out of diaNetConfig by the settings-layout
// restructure; ids unchanged. POPSLoader-parity flow: these fields show POPSTARTER's OWN
// IPCONFIG.DAT / SMBCONFIG.DAT contents (read on dialog open), NOT OPL's settings. Absent files
// leave the fields blank with the notice visible; only an explicit edit or the Import button
// changes them, and only an actual change is written back on OK (see guiShowPopsNetConfig).
struct UIItem diaPopsNetConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NETCONFIG}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_POPS_SMB_SECTION}}},
    {UI_BREAK},

    {UI_LABEL, NETCFG_POPS_NOTICE, 1, 1, -1, -40, 0, {.label = {NULL, _STR_POPS_NONE_DETECTED}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_BUTTON, NETCFG_POPS_IMPORT, 1, 1, _STR_HINT_POPS_IMPORT, -40, 0, {.label = {NULL, _STR_POPS_IMPORT}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_IP_ADDRESS_TYPE}}},
    {UI_SPACER},
    {UI_ENUM, NETCFG_POPS_IPTYPE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_IP_ADDRESS}}},
    {UI_SPACER},
    {UI_INT, NETCFG_POPS_IP_0, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_IP_1, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_IP_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_IP_3, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MASK}}},
    {UI_SPACER},
    {UI_INT, NETCFG_POPS_MASK_0, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_MASK_1, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_MASK_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_MASK_3, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_GATEWAY}}},
    {UI_SPACER},
    {UI_INT, NETCFG_POPS_GW_0, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_GW_1, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_GW_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_GW_3, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ADDRESS}}},
    {UI_SPACER},
    {UI_INT, NETCFG_POPS_SMB_IP_0, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_SMB_IP_1, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_SMB_IP_2, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {".", -1}}},
    {UI_INT, NETCFG_POPS_SMB_IP_3, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 255}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PORT}}},
    {UI_SPACER},
    {UI_INT, NETCFG_POPS_SMB_PORT, 1, 1, _STR_HINT_POPS_PORT, 0, 0, {.intvalue = {0, 0, 0, 65535}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SHARE}}},
    {UI_SPACER},
    {UI_STRING, NETCFG_POPS_SMB_SHARE, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_USER}}},
    {UI_SPACER},
    {UI_STRING, NETCFG_POPS_SMB_USER, 1, 1, _STR_HINT_GUEST, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PASSWORD}}},
    {UI_SPACER},
    {UI_PASSWORD, NETCFG_POPS_SMB_PASS, 1, 1, _STR_HINT_GUEST, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Settings page (settings-layout restructure): the slim "Settings" category. Debug/PS2-logo/
// default-core/Neutrino/write-ops/rumble/prefix/cache rows moved out to Game Launching, Security,
// Controller, Advanced, POPStarter and MMCE pages; CFG ids unchanged.
struct UIItem diaConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_LASTPLAYED}}},
    {UI_SPACER},
    {UI_BOOL, CFG_LASTPLAYED, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, CFG_LBL_AUTOSTARTLAST, 1, 1, -1, 0, 0, {.label = {NULL, _STR_AUTOSTARTLAST}}},
    {UI_SPACER},
    {UI_INT, CFG_AUTOSTARTLAST, 1, 1, _STR_HINT_AUTOSTARTLAST, 0, 0, {.intvalue = {0, 0, 0, 9}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_FOLDER_NAV}}},
    {UI_SPACER},
    {UI_BOOL, CFG_FOLDERNAV, 1, 1, _STR_HINT_FOLDER_NAV, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_EXITTO}}},
    {UI_SPACER},
    {UI_STRING, CFG_EXITTO, 1, 1, _STR_HINT_EXITPATH, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Game Sources page (settings-layout restructure, was Device Settings): device selection + start
// modes. The Protocol/Access/SMB-Version rows moved to the Network page (diaNetConfig); the legacy
// CFG_ENABLEUDPBD/CFG_NETBOOTPROTOCOL ids stay retired placeholders.
struct UIItem diaDeviceConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_GAME_SOURCES}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_DEFDEVICE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_DEFDEVICE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BDMMODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_BDMMODE, 1, 1, _STR_HINT_BDM_START, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"USB", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_ENABLEUSB, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"iLink", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_ENABLEILK, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"MX4SIO", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_ENABLEMX4SIO, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"HDD (GPT/MBR)", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_ENABLEBDMHDD, 1, 1, _STR_HDD_HINT, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_HDDMODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_HDDMODE, 1, 1, _STR_HDD_HINT, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // Network Start Mode (Off/Manual/Auto) gates whether/when the stack loads; Protocol, Access and
    // SMB Version live on the Network page now.
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NET_PROTOCOL}}},
    {UI_SPACER},
    {UI_ENUM, CFG_NETSTART, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_APPMODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_APPMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_FAVMODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_FAVMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCEMODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_MMCEMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// POPStarter page (settings-layout restructure, was VCD Settings): PS1-via-POPSTARTER launch
// config. The BDMA equip rows, VCD list display options and POPStarter network fields now live in
// the chained sub-dialogs diaBdmaConfig / diaVcdListConfig / diaPopsNetConfig; CFG ids unchanged.
struct UIItem diaVcdConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_POPSTARTER}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_POPSTARTER_DEVICE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_POPSTARTER_DEVICE, 1, 1, _STR_HINT_POPSTARTER_DEVICE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, CFG_LBL_POPSTARTER_PATH, 1, 1, -1, -40, 0, {.label = {NULL, _STR_POPSTARTER_PATH}}},
    {UI_SPACER},
    {UI_STRING, CFG_POPSTARTER_PATH, 1, 1, _STR_HINT_POPSTARTER_PATH, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_POPSTARTER_RETROGEM_GAMEID}}},
    {UI_SPACER},
    {UI_BOOL, CFG_POPSTARTER_RETROGEM_GAMEID, 1, 1, _STR_HINT_POPSTARTER_RETROGEM_GAMEID, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},


    // sub-pages
    {UI_BUTTON, VCD_BDMA_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_BDMA_SETTINGS}}},
    {UI_BREAK},
    {UI_BUTTON, VCD_LIST_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_GAME_LIST_SETTINGS}}},
    {UI_BREAK},
    {UI_BUTTON, VCD_NET_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NETCONFIG}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// POPStarter -> BDMA Settings (BDMAssault exFAT-driver equip). Rows moved out of diaVcdConfig.
struct UIItem diaBdmaConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_BDMA_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BDMA_APPLY}}},
    {UI_SPACER},
    {UI_BOOL, CFG_BDMA_APPLY, 1, 1, _STR_HINT_BDMA_APPLY, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, CFG_LBL_BDMASOURCE, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BDMA_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_BDMASOURCE, 1, 1, _STR_HINT_BDMA_SOURCE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, CFG_LBL_BDMAMODE, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BDMA_MODE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_BDMAMODE, 1, 1, _STR_HINT_BDMA_MODE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// POPStarter -> Game List Settings (VCD list display options). Rows moved out of diaVcdConfig.
struct UIItem diaVcdListConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_GAME_LIST_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_VCD_HIDE_GAMEID}}},
    {UI_SPACER},
    {UI_BOOL, CFG_VCD_HIDE_GAMEID, 1, 1, _STR_HINT_VCD_HIDE_GAMEID, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_VCD_FIRST_DISC}}},
    {UI_SPACER},
    {UI_BOOL, CFG_VCD_FIRST_DISC_ONLY, 1, 1, _STR_HINT_VCD_FIRST_DISC, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// USB VCD launch mode pick: shown on EVERY USB .VCD launch (bdmLaunchVcd). The PS2 cannot detect the
// filesystem a USB stick is actually formatted with, so the user picks the POPSTARTER driver per
// launch -- fat32 (POPSTARTER's built-in USB stack, recommended for non-exFAT users) or exFAT (the
// BDMAssault usbexfat pair). The pressed button's id IS the result; Back cancels the launch.
struct UIItem diaVcdUsbMode[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VCD_USB_MODE_TITLE}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VCD_USB_MODE_QUESTION}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VCD_USB_MODE_HINT}}},
    {UI_BREAK},

    // buttons (each returns its own id from diaExecuteDialog)
    {UI_BUTTON, VCDUSB_BTN_FAT32, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VCD_USB_MODE_FAT32}}},
    {UI_BREAK},
    {UI_BUTTON, VCDUSB_BTN_EXFAT, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VCD_USB_MODE_EXFAT}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// MMCE page (settings-layout restructure, was MMCE Settings): SD2PSX / MemCard PRO2 tuning.
// Communication tuning and the path prefix moved to the chained sub-dialogs diaMmceCommConfig /
// diaMmcePathConfig; CFG ids unchanged.
struct UIItem diaMmceConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MMCE}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCE_SLOT}}},
    {UI_SPACER},
    {UI_ENUM, CFG_MMCESLOT, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCEIGR_SLOT}}},
    {UI_SPACER},
    {UI_ENUM, CFG_MMCEIGRSLOT, 1, 1, _STR_HINT_MMCEIGR_SLOT, 0, 0, {.intvalue = {0, 0, 0, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Send GameID on Launch", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_MMCEGAMEID, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // sub-pages
    {UI_BUTTON, MMCE_COMM_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COMM_SETTINGS}}},
    {UI_BREAK},
    {UI_BUTTON, MMCE_PATH_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PATH_SETTINGS}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// MMCE -> Communication Settings (ACK wait / alarm tuning). Rows moved out of diaMmceConfig.
struct UIItem diaMmceCommConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COMM_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCE_WAIT_CYCLES}}},
    {UI_SPACER},
    {UI_ENUM, CFG_MMCE_WAIT_CYCLES, 1, 1, _STR_HINT_MMCE_WAIT_CYCLES, 0, 0, {.intvalue = {0, 0, 0, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCE_USE_ALARMS}}},
    {UI_SPACER},
    {UI_ENUM, CFG_MMCE_USE_ALARMS, 1, 1, _STR_HINT_MMCE_USE_ALARMS, 0, 0, {.intvalue = {0, 0, 0, 1}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// MMCE -> Path Settings (library prefix). Row moved out of the old General Settings (diaConfig).
struct UIItem diaMmcePathConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PATH_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_MMCE_PREFIX}}},
    {UI_SPACER},
    {UI_STRING, CFG_MMCEPREFIX, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Interface page (settings-layout restructure, was Display Settings): theme/language/list behavior.
// Artwork, Coverflow and Colors are chained sub-dialogs; the video/offset/overscan/GameID-barcode
// rows moved to the Display page (diaDisplayConfig). Ids unchanged.
struct UIItem diaUIConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_INTERFACE_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_THEME}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_THEME, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_LANGUAGE}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_LANG, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Default game view", -1}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_GAMEVIEW, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_AUTOSORT}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_AUTOSORT, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_AUTOREFRESH}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_AUTOREFRESH, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLE_NOTIFICATIONS}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_NOTIFICATIONS, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    // sub-pages
    {UI_BUTTON, UICFG_ARTWORK_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_ARTWORK_SETTINGS}}},
    {UI_BREAK},
    {UI_BUTTON, UICFG_COVERFLOW_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COVERFLOW_SETTINGS}}},
    {UI_BREAK},
    {UI_BUTTON, UICFG_COLORS_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COLORS_SETTINGS}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Interface -> Artwork Settings. Rows moved out of diaUIConfig.
struct UIItem diaArtworkConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_ARTWORK_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_COVERART}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_COVERART, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Background Art", -1}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_ENABLE_BGART, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLE_ART_TAR}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_ENABLE_ART_TAR, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Art Delay", -1}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_ART_DELAY, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Interface -> Colors. Colour rows + Reset Colors moved out of diaUIConfig.
struct UIItem diaColorsConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COLORS_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_TXTCOLOR}}},
    {UI_SPACER},
    {UI_COLOUR, UICFG_TXTCOL, 1, 1, -1, -10, 17, {.colourvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_SELCOLOR}}},
    {UI_SPACER},
    {UI_COLOUR, UICFG_SELCOL, 1, 1, -1, -10, 17, {.colourvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_UICOLOR}}},
    {UI_SPACER},
    {UI_COLOUR, UICFG_UICOL, 1, 1, -1, -10, 17, {.colourvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_BGCOLOR}}},
    {UI_SPACER},
    {UI_COLOUR, UICFG_BGCOL, 1, 1, -1, -10, 17, {.colourvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_PLASCOLOR}}},
    {UI_SPACER},
    {UI_COLOUR, UICFG_PLASCOL, 1, 1, -1, -10, 17, {.colourvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BUTTON, UICFG_RESETCOL, 1, 1, -1, 0, 0, {.label = {NULL, _STR_RESETCOLOR}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Display page (settings-layout restructure): video mode / geometry / GameID barcode. Rows moved
// out of diaUIConfig.
struct UIItem diaDisplayConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_DISPLAY_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_VMODE}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_VMODE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_WIDE_SCREEN}}},
    {UI_SPACER},
    {UI_BOOL, UICFG_WIDESCREEN, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_XOFFSET}}},
    {UI_SPACER},
    {UI_INT, UICFG_XOFF, 1, 1, -1, 0, 0, {.intvalue = {0, 0, -300, 300}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_YOFFSET}}},
    {UI_SPACER},
    {UI_INT, UICFG_YOFF, 1, 1, -1, 0, 0, {.intvalue = {0, 0, -300, 300}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_OVERSCAN}}},
    {UI_SPACER},
    {UI_INT, UICFG_OVERSCAN, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"Show GameID Barcode (Pixel FX)", -1}}},
    {UI_SPACER},
    {UI_BOOL, CFG_APPLYGAMEID, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Game Launching page (settings-layout restructure): launch-time behavior + the global Neutrino and
// OSD defaults as chained sub-dialogs. Rows moved out of the old General Settings (diaConfig).
struct UIItem diaLaunchConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_GAME_LAUNCHING}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PS2LOGO}}},
    {UI_SPACER},
    {UI_BOOL, CFG_PS2LOGO, 1, 1, _STR_HINT_PS2LOGO, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_DEFAULT_CORE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_DEFAULT_CORE, 1, 1, _STR_HINT_DEFAULT_CORE, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    // sub-pages
    {UI_BUTTON, LAUNCH_NEUTRINO_DEFAULTS_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NEUTRINO_DEFAULTS}}},
    {UI_BREAK},
    {UI_BUTTON, LAUNCH_OSD_DEFAULTS_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OSD_DEFAULTS}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Game Launching -> Neutrino Defaults: the global Neutrino device/video/gsm-comp defaults (games
// whose per-game picker is "Default" follow these) + the structured Advanced Arguments editor.
// Rows moved out of the old General Settings (diaConfig).
struct UIItem diaNeutrinoDefaults[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NEUTRINO_DEFAULTS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NEUTRINO_DEVICE}}},
    {UI_SPACER},
    {UI_ENUM, CFG_NEUTRINO_DEVICE, 1, 1, _STR_HINT_NEUTRINO_DEVICE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NEUTRINO_VIDEO}}},
    {UI_SPACER},
    {UI_ENUM, CFG_NEUTRINO_VIDEO, 1, 1, _STR_HINT_NEUTRINO_VIDEO, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NEUTRINO_GSM_COMP}}},
    {UI_SPACER},
    {UI_ENUM, CFG_NEUTRINO_GSMCOMP, 1, 1, _STR_HINT_NEUTRINO_GSM_COMP, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BUTTON, CFG_NEUTRINO_ARGS, 1, 1, _STR_HINT_NEUTRINO_ARGS, 0, 0, {.label = {NULL, _STR_ADVANCED_ARGUMENTS}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Security page (settings-layout restructure): write-operations gate + the Parental Lock password
// as a chained sub-dialog. Rows moved out of the old General Settings (diaConfig).
struct UIItem diaSecurityConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_SECURITY_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLE_WRITE}}},
    {UI_SPACER},
    {UI_BOOL, CFG_ENWRITEOP, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BUTTON, SECURITY_PARENTAL_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PARENLOCKCONFIG}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Advanced page (settings-layout restructure): debug display + path prefixes and storage/cache as
// chained sub-dialogs. Rows moved out of the old General Settings (diaConfig).
struct UIItem diaAdvancedConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_ADVANCED_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_DEBUG}}},
    {UI_SPACER},
    {UI_BOOL, CFG_DEBUG, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BUTTON, ADV_PREFIX_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PATH_PREFIXES}}},
    {UI_BREAK},
    {UI_BUTTON, ADV_STORAGE_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_STORAGE_CACHE}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Advanced -> Path Prefixes. Rows moved out of the old General Settings (diaConfig).
struct UIItem diaPathPrefixConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PATH_PREFIXES}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BDM_PREFIX}}},
    {UI_SPACER},
    {UI_STRING, CFG_BDMPREFIX, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ETH_PREFIX}}},
    {UI_SPACER},
    {UI_STRING, CFG_ETHPREFIX, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Advanced -> Storage and Cache. Rows moved out of the old General Settings (diaConfig).
struct UIItem diaStorageConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_STORAGE_CACHE}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_HDD_SPINDOWN}}},
    {UI_SPACER},
    {UI_INT, CFG_HDDSPINDOWN, 1, 1, _STR_HINT_SPINDOWN, 0, 0, {.intvalue = {20, 20, 0, 20}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_CACHE_HDD_GAME_LIST}}},
    {UI_SPACER},
    {UI_BOOL, CFG_HDDGAMELISTCACHE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"BDM Cache", -1}}},
    {UI_SPACER},
    {UI_INT, CFG_BDMCACHE, 1, 1, -1, 0, 0, {.intvalue = {16, 8, 0, 32, NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"HDD Cache", -1}}},
    {UI_SPACER},
    {UI_INT, CFG_HDDCACHE, 1, 1, -1, 0, 0, {.intvalue = {8, 0, 0, 32, NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {"SMB Cache", -1}}},
    {UI_SPACER},
    {UI_INT, CFG_SMBCACHE, 1, 1, -1, 0, 0, {.intvalue = {16, 4, 0, 32, NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Tools page (settings-layout restructure): actions that used to be top-level menu entries.
struct UIItem diaToolsConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_TOOLS}}},
    {UI_SPLITTER},

    {UI_BUTTON, TOOLS_NET_UPDATE_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NET_UPDATE}}},
    {UI_BREAK},
    {UI_BUTTON, TOOLS_NBD_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_STARTNBD}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Per-Game Modes Menu (row order per the settings-layout restructure tree: Loader Core, DMA,
// Game ID, Alternate Startup, Modes 1-7, then the Neutrino overrides)
struct UIItem diaCompatConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_COMPAT_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_CORE_LOADER}}},
    {UI_SPACER},
    {UI_ENUM, COMPAT_LOADER, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_DMA_MODE}}},
    {UI_SPACER},
    {UI_ENUM, COMPAT_DMA, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_GAME_ID}}},
    {UI_SPACER},
    {UI_STRING, COMPAT_GAMEID, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_SPACER},
    {UI_BUTTON, COMPAT_LOADFROMDISC, 1, 1, -1, 0, 0, {.label = {NULL, _STR_LOAD_FROM_DISC}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_ALTSTARTUP}}},
    {UI_SPACER},
    {UI_STRING, COMPAT_ALTSTARTUP, 1, 1, -1, 0, 0, {.stringvalue = {"", "", &guiGameAltStartupNameHandler}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE1}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE, 1, 1, _STR_HINT_MODE1, -10, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE2}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 1, 1, 1, _STR_HINT_MODE2, -10, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE3}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 2, 1, 1, _STR_HINT_MODE3, -10, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE4}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 3, 1, 1, _STR_HINT_MODE4, -10, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE5}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 4, 1, 1, _STR_HINT_MODE5, -10, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE6}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 5, 1, 1, _STR_HINT_MODE6, -10, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_MODE7}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_MODE_BASE + 6, 1, 1, _STR_HINT_MODE7, -10, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BUTTON, COMPAT_DL_DEFAULTS, 1, 1, -1, 0, 0, {.label = {NULL, _STR_DL_DEFAULTS}}},
    {UI_SPLITTER},

    {UI_SPACER},
    {UI_BUTTON, COMPAT_NEUTRINO_ARGS, 1, 1, _STR_HINT_NEUTRINO_ARGS, 0, 0, {.label = {NULL, _STR_NEUTRINO_ARGS}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_NEUTRINO_VIDEO}}},
    {UI_SPACER},
    {UI_ENUM, COMPAT_NEUTRINO_VIDEO, 1, 1, _STR_HINT_NEUTRINO_VIDEO, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_NEUTRINO_GSM_COMP}}},
    {UI_SPACER},
    {UI_ENUM, COMPAT_NEUTRINO_GSMCOMP, 1, 1, _STR_HINT_NEUTRINO_GSM_COMP, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_NEUTRINO_BSDFS}}},
    {UI_SPACER},
    {UI_ENUM, COMPAT_NEUTRINO_BSDFS, 1, 1, _STR_HINT_NEUTRINO_BSDFS, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Per-Game VMC Menu
struct UIItem diaVMCConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_VMC_SCREEN}}},
    {UI_SPLITTER},

    // VMC
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_VMC_SLOT1}}},
    {UI_SPACER},
    {UI_BUTTON, COMPAT_VMC1_DEFINE, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_SPACER},
    {UI_BUTTON, COMPAT_VMC1_ACTION, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_VMC_SLOT2}}},
    {UI_SPACER},
    {UI_BUTTON, COMPAT_VMC2_DEFINE, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_SPACER},
    {UI_BUTTON, COMPAT_VMC2_ACTION, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_BREAK},
    {UI_SPLITTER},

    // Per-slot disable (parity-audit #14): launch without a slot's VMC while keeping its card
    // configured. Consulted by the Neutrino -mc builder only; OPL-core mcemu ignores it.
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_VMC_SLOT1_DISABLE}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_VMC1_DISABLE, 1, 1, _STR_HINT_VMC_SLOT_DISABLE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -30, 0, {.label = {NULL, _STR_VMC_SLOT2_DISABLE}}},
    {UI_SPACER},
    {UI_BOOL, COMPAT_VMC2_DISABLE, 1, 1, _STR_HINT_VMC_SLOT_DISABLE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Per-Game Game Settings > VMC Menu
struct UIItem diaVMC[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_VMC_SCREEN}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_VMC_NAME}}},
    {UI_SPACER},
    {UI_STRING, VMC_NAME, 1, 1, -1, 0, 0, {.stringvalue = {"", "", &guiGameVmcNameHandler}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_SIZE}}},
    {UI_SPACER},
    {UI_ENUM, VMC_SIZE, 1, 1, _STR_HINT_VMC_SIZE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_VMC_STATUS}}},
    {UI_SPACER},
    {UI_LABEL, VMC_STATUS, 0, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_VMC_PROGRESS}}},
    {UI_SPACER},
    {UI_INT, VMC_PROGRESS, 0, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {"%", -1}}},
    {UI_BREAK},

    // buttons
    {UI_BUTTON, VMC_BUTTON_CREATE, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_SPACER},
    {UI_BUTTON, VMC_BUTTON_DELETE, 1, 1, -1, 0, 0, {.label = {NULL, _STR_DELETE}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Per-Game Game Settings > GSM Menu (--Bat--)
struct UIItem diaGSConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_GSM_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SETTINGS_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, GSMCFG_GSMSOURCE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLEGSM}}},
    {UI_SPACER},
    {UI_BOOL, GSMCFG_ENABLEGSM, 1, 1, _STR_HINT_ENABLEGSM, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_VMODE}}},
    {UI_SPACER},
    {UI_ENUM, GSMCFG_GSMVMODE, 1, 1, _STR_HINT_GSMVMODE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_XOFFSET}}},
    {UI_SPACER},
    {UI_INT, GSMCFG_GSMXOFFSET, 1, 1, _STR_HINT_XOFFSET, -5, 0, {.intvalue = {0, 0, -100, 100}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_YOFFSET}}},
    {UI_SPACER},
    {UI_INT, GSMCFG_GSMYOFFSET, 1, 1, _STR_HINT_YOFFSET, -5, 0, {.intvalue = {0, 0, -100, 100}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_GSM_FIELD_FIX}}},
    {UI_SPACER},
    {UI_BOOL, GSMCFG_GSMFIELDFIX, 1, 1, _STR_HINT_GSM_FIELD_FIX, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Per Game Settings > Cheat Menu --Bat--
struct UIItem diaCheatConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CHEAT_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SETTINGS_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, CHTCFG_CHEATSOURCE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLECHEAT}}},
    {UI_SPACER},
    {UI_BOOL, CHTCFG_ENABLECHEAT, 1, 1, _STR_HINT_ENABLECHEAT, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_CHEATMODE}}},
    {UI_SPACER},
    {UI_ENUM, CHTCFG_CHEATMODE, 1, 1, _STR_HINT_CHEATMODE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLEIMAGE}}},
    {UI_SPACER},
    {UI_BOOL, CHTCFG_ENABLEIMAGE, 1, 1, _STR_HINT_ENABLEIMAGE, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

#ifdef PADEMU
struct UIItem diaPadEmuConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PADEMU_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_SETTINGS_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, PADCFG_PADEMU_SOURCE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADEMU_ENABLE}}},
    {UI_SPACER},
    {UI_BOOL, PADCFG_PADEMU_ENABLE, 1, 1, _STR_HINT_PADEMU_ENABLE, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADEMU_MODE}}},
    {UI_SPACER},
    {UI_ENUM, PADCFG_PADEMU_MODE, 1, 1, _STR_HINT_PADEMU_MODE, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_MTAP_ENABLE}}},
    {UI_SPACER},
    {UI_BOOL, PADCFG_PADEMU_MTAP, 1, 1, _STR_HINT_MTAP_ENABLE, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_MTAP_PORT}}},
    {UI_SPACER},
    {UI_INT, PADCFG_PADEMU_MTAP_PORT, 1, 1, _STR_HINT_MTAP_PORT, 0, 0, {.intvalue = {1, 1, 1, 2}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADPORT}}},
    {UI_SPACER},
    {UI_ENUM, PADCFG_PADPORT, 1, 1, _STR_HINT_PAD_PORT, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADEMU_PORT}}},
    {UI_SPACER},
    {UI_BOOL, PADCFG_PADEMU_PORT, 1, 1, _STR_HINT_PADEMU_PORT, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADEMU_VIB}}},
    {UI_SPACER},
    {UI_BOOL, PADCFG_PADEMU_VIB, 1, 1, _STR_HINT_PADEMU_VIB, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_SPLITTER},
    {UI_BREAK},

    {UI_LABEL, PADCFG_USBDG_MAC_STR, 1, 1, -1, -50, 0, {.label = {NULL, _STR_USBDG_MAC}}},
    {UI_SPACER},
    {UI_LABEL, PADCFG_USBDG_MAC, 1, 1, -1, 0, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, PADCFG_PAD_MAC_STR, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PAD_MAC}}},
    {UI_SPACER},
    {UI_LABEL, PADCFG_PAD_MAC, 1, 1, -1, 0, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, PADCFG_PAIR_STR, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PAIR_PAD}}},
    {UI_SPACER},
    {UI_BUTTON, PADCFG_PAIR, 1, 1, _STR_HINT_PAIRPAD, 0, 0, {.label = {NULL, _STR_PAIR}}},
    {UI_BREAK},

    {UI_LABEL, PADCFG_PADEMU_WORKAROUND_STR, 1, 1, -1, -50, 0, {.label = {NULL, _STR_PADEMU_WORKAROUND}}},
    {UI_SPACER},
    {UI_BOOL, PADCFG_PADEMU_WORKAROUND, 1, 1, _STR_HINT_PADEMU_WORKAROUND, 0, 0, {.intvalue = {1, 1}}},
    {UI_BREAK},

    {UI_BREAK},
    {UI_BUTTON, PADCFG_BTINFO, 1, 1, _STR_HINT_BTINFO, 0, 0, {.label = {NULL, _STR_BTINFO}}},
    {UI_BREAK},

    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},

    // end of dialog
    {UI_TERMINATOR}};

struct UIItem diaPadEmuInfo[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_BTINFO}}}, {UI_SPACER},

    {UI_SPLITTER},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {"VID:", -1}}},
    {UI_LABEL, PADCFG_VID, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {"PID:", -1}}},
    {UI_LABEL, PADCFG_PID, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {"REV:", -1}}},
    {UI_LABEL, PADCFG_REV, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {NULL, _STR_HCIVER}}},
    {UI_LABEL, PADCFG_HCIVER, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {NULL, _STR_LMPVER}}},
    {UI_LABEL, PADCFG_LMPVER, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {NULL, _STR_MANUFACTURER}}},
    {UI_LABEL, PADCFG_MANID, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -45, 0, {.label = {NULL, _STR_SUPFEATURES}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"00.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 0, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"08.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 8, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"16.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 16, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"24.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 24, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"32.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 32, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"40.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 40, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"48.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 48, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"56.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 56, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"01.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 1, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"09.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 9, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"17.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 17, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"25.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 25, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"33.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 33, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"41.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 41, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"49.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 49, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"57.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 57, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"02.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 2, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"10.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 10, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"18.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 18, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"26.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 26, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"34.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 34, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"42.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 42, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"50.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 50, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"58.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 58, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"03.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 3, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"11.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 11, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"19.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 19, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"27.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 27, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"35.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 35, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"43.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 43, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"51.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 51, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"59.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 59, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"04.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 4, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"12.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 12, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"20.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 20, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"28.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 28, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"36.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 36, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"44.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 44, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"52.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 52, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"60.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 60, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"05.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 5, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"13.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 13, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"21.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 21, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"29.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 29, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"37.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 37, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"45.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 45, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"53.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 53, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"61.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 61, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"06.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 6, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"14.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 14, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"22.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 22, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"30.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 30, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"38.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 38, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"46.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 46, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"54.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 54, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"62.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 62, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"07.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 7, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"15.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 15, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"23.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 23, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"31.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 31, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"39.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 39, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"47.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 47, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"55.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 55, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_LABEL, 0, 1, 1, -1, -4, 0, {.label = {"63.", -1}}},
    {UI_LABEL, PADCFG_FEAT_START + 63, 0, 1, -1, -7, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, PADCFG_BT_SUPPORTED, 1, 1, -1, -45, 0, {.label = {"", -1}}},
    {UI_BREAK},
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},

    // end of dialog
    {UI_TERMINATOR}};

struct UIItem diaPadMacroConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PADMACRO_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -50, 0, {.label = {NULL, _STR_SETTINGS_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, PADMACRO_CFG_SOURCE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PADMACRO_SLOWDOWN}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_LEFT_ANALOG}}},
    {UI_ENUM, PADMACRO_SLOWDOWN_L, 1, 1, _STR_HINT_PADMACRO_SLOWDOWN_AXIS, -20, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_RIGHT_ANALOG}}},
    {UI_ENUM, PADMACRO_SLOWDOWN_R, 1, 1, _STR_HINT_PADMACRO_SLOWDOWN_AXIS, -20, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_ENUM, PADMACRO_SLOWDOWN_TOGGLE_L, 1, 1, -1, -40, 0, {.intvalue = {0, 0}}},
    {UI_SPACER},
    {UI_ENUM, PADMACRO_SLOWDOWN_TOGGLE_R, 1, 1, -1, -40, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PADMACRO_INVERT_AXIS}}},
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -10, 0, {.label = {"LX:", -1}}},
    {UI_BOOL, PADMACRO_INVERT_LX, 1, 1, _STR_HINT_PADMACRO_INVERT_AXIS, -10, 0, {.intvalue = {0, 0}}},
    {UI_LABEL, 0, 1, 1, -1, -10, 0, {.label = {"LY:", -1}}},
    {UI_BOOL, PADMACRO_INVERT_LY, 1, 1, _STR_HINT_PADMACRO_INVERT_AXIS, -10, 0, {.intvalue = {0, 0}}},
    {UI_LABEL, 0, 1, 1, -1, -10, 0, {.label = {"RX:", -1}}},
    {UI_BOOL, PADMACRO_INVERT_RX, 1, 1, _STR_HINT_PADMACRO_INVERT_AXIS, -10, 0, {.intvalue = {0, 0}}},
    {UI_LABEL, 0, 1, 1, -1, -10, 0, {.label = {"RY:", -1}}},
    {UI_BOOL, PADMACRO_INVERT_RY, 1, 1, _STR_HINT_PADMACRO_INVERT_AXIS, -10, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -20, 0, {.label = {NULL, _STR_TURBO_SPEED}}},
    {UI_SPACER},
    {UI_INT, PADMACRO_TURBO_SPEED, 1, 1, _STR_HINT_TURBO_SPEED, -10, 0, {.intvalue = {4, 3, 1, 4}}},
    {UI_BREAK},

    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};
#endif

// About Menu
struct UIItem diaAbout[] = {
    {UI_HEADER, ABOUT_TITLE, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_SPLITTER},

    // Coders
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_DEVS}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"BatRastard - bbsan2k - belek666 - crazyc - dlanor - doctorxyz", -1}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"hominem.te.esse - ifcaro - izdubar - jimmikaelkael - KrahJohlito", -1}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"kr_ps2 - Maximus32 - misfire - Polo35 - reprep - saildot4k - SP193 - volca", -1}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"... and the anonymous ...", -1}}},
    {UI_BREAK},

    {UI_BREAK},

    // Quality Assurance
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {NULL, _STR_QANDA}}},
    {UI_BREAK},

    {UI_SPACER},
    // Blade1984 and zackcage6 sit here rather than in a fork-testers block of their own: the About
    // cannot scroll (its only navigable control is the trailing OK, and diaRenderUI pins
    // diaScrollOffset to 0 while focus is on the first control), so a new heading + name row pushed
    // the content bottom from 367px to 442px -- past visibleBottom (gTheme->usedHeight - 40 = 440,
    // and only 408 on a 448-line theme). That would have rendered the credit, and the OK button,
    // off-screen. APPEND fork testers to an existing row; never add a row.
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"algol - Berion - Blade1984 - El_Patas - EP - gledson999 - jolek - lee4", -1}}},
    {UI_BREAK},

    {UI_SPACER},
    // 68 chars, still shorter than the 70-char row above it, so this stays inside the width the
    // block already proved safe on a 448-line theme.
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"LocalH - RandQalan - ShaolinAssassin - yoshi314 - zero35 - zackcage6", -1}}},
    {UI_BREAK},

    {UI_BREAK},

    // Network update
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {NULL, _STR_NET_UPDATE}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"icyson55", -1}}},
    {UI_BREAK},

    // Financial Support (RiptOPL fork)
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {NULL, _STR_FINANCIAL_SUPPORT}}},
    {UI_BREAK},

    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 15, {.label = {"Akilluminati47", -1}}},
    {UI_BREAK},

    // Build Options
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_BUILD_DETAILS}}},
    {UI_SPACER},
    {UI_LABEL, ABOUT_BUILD_DETAILS, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_BREAK},

    // Support details
    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_FORUM_DETAILS}}},
    {UI_SPACER},
    {UI_LABEL, 0, 1, 1, -1, 0, 0, {.label = {"psx-place.com", -1}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Network Update Menu
struct UIItem diaNetCompatUpdate[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NET_UPDATE}}},
    {UI_SPLITTER},

    {UI_LABEL, NETUPD_OPT_UPD_ALL_LBL, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NET_UPDATE_ALL}}},
    {UI_SPACER},
    {UI_BOOL, NETUPD_OPT_UPD_ALL, 0, 1, _STR_NET_UPDATE_HINT, 0, 0, {.intvalue = {0, 0, 0, 1}}},
    {UI_BREAK},

    {UI_LABEL, NETUPD_PROGRESS_LBL, 1, 1, -1, -40, 0, {.label = {NULL, _STR_VMC_PROGRESS}}},
    {UI_SPACER},
    {UI_INT, NETUPD_PROGRESS, 0, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_LABEL, NETUPD_PROGRESS_PERC_LBL, 1, 1, -1, 0, 0, {.label = {"%", -1}}},
    {UI_BREAK},

    // buttons
    {UI_BUTTON, NETUPD_BTN_START, 1, 1, -1, 0, 0, {.label = {NULL, _STR_START}}},
    {UI_SPACER},
    {UI_BUTTON, NETUPD_BTN_CANCEL, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CANCEL}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Parental Lock Config Menu
struct UIItem diaParentalLockConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_PARENLOCKCONFIG}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_PARENLOCK_PASSWORD}}},
    {UI_SPACER},
    {UI_PASSWORD, CFG_PARENLOCK_PASSWORD, 1, 1, _STR_PARENLOCK_PASSWORD_HINT, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},

    // end of dialog
    {UI_TERMINATOR}};

// Audio Settings Menu
struct UIItem diaAudioConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_AUDIO_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SFX}}},
    {UI_SPACER},
    {UI_BOOL, CFG_SFX, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BOOT_SND}}},
    {UI_SPACER},
    {UI_BOOL, CFG_BOOT_SND, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BGM}}},
    {UI_SPACER},
    {UI_BOOL, CFG_BGM, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SFX_VOLUME}}},
    {UI_SPACER},
    {UI_INT, CFG_SFX_VOLUME, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BOOT_SND_VOLUME}}},
    {UI_SPACER},
    {UI_INT, CFG_BOOT_SND_VOLUME, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_BGM_VOLUME}}},
    {UI_SPACER},
    {UI_INT, CFG_BGM_VOLUME, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, 100}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_DEF_BGM_PATH}}},
    {UI_SPACER},
    {UI_STRING, CFG_DEFAULT_BGM_PATH, 1, 1, _STR_DEF_BGM_PATH_HINT, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},
    // end of dialog
    {UI_TERMINATOR}};

// Controller Settings Menu
struct UIItem diaControllerConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CONTROLLER_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SCROLLING}}},
    {UI_SPACER},
    {UI_ENUM, UICFG_SCROLL, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SELECTBUTTON}}},
    {UI_SPACER},
    {UI_ENUM, CFG_SELECTBUTTON, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_BREAK},
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_XSENSITIVITY}}},
    {UI_SPACER},
    {UI_ENUM, CFG_XSENSITIVITY, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_YSENSITIVITY}}},
    {UI_SPACER},
    {UI_ENUM, CFG_YSENSITIVITY, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // Menu Rumble moved here from the old General Settings (diaConfig) by the layout restructure.
    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_RUMBLE}}},
    {UI_SPACER},
    {UI_BOOL, CFG_RUMBLE, 1, 1, _STR_HINT_RUMBLE, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
#ifdef PADEMU
    {UI_BREAK},
    {UI_BUTTON, PADEMU_GLOBAL_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CONTROLLER_EMULATION}}},
    {UI_BREAK},
    {UI_BUTTON, PADMACRO_GLOBAL_BUTTON, 1, 1, -1, 0, 0, {.label = {NULL, _STR_CONTROLLER_MACROS}}},
    {UI_BREAK},
#endif
    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},
    // end of dialog
    {UI_TERMINATOR}};

struct UIItem diaCoverflowConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_COVERFLOW_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_COVERFLOW_COUNT}}},
    {UI_SPACER},
    {UI_ENUM, COVERFLOW_CFG_COUNT, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_COVERFLOW_SCALE}}},
    {UI_SPACER},
    {UI_ENUM, COVERFLOW_CFG_SCALE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_COVERFLOW_ANIM}}},
    {UI_SPACER},
    {UI_ENUM, COVERFLOW_CFG_ANIM, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_COVERFLOW_DIM}}},
    {UI_SPACER},
    {UI_BOOL, COVERFLOW_CFG_DIM, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},
    // end of dialog
    {UI_TERMINATOR}};

struct UIItem diaNeutrinoArgs[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_NEUTRINO_ARGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_QB}}},
    {UI_SPACER},
    {UI_BOOL, NARGS_QB, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_DBC}}},
    {UI_SPACER},
    {UI_BOOL, NARGS_DBC, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_LOGO}}},
    {UI_SPACER},
    {UI_BOOL, NARGS_LOGO, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_CWD}}},
    {UI_SPACER},
    {UI_STRING, NARGS_CWD, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_CFG}}},
    {UI_SPACER},
    {UI_STRING, NARGS_CFG, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_ELF}}},
    {UI_SPACER},
    {UI_STRING, NARGS_ELF, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_ATA0}}},
    {UI_SPACER},
    {UI_STRING, NARGS_ATA0, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_ATA0ID}}},
    {UI_SPACER},
    {UI_STRING, NARGS_ATA0ID, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_ATA1}}},
    {UI_SPACER},
    {UI_STRING, NARGS_ATA1, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_EXTRA}}},
    {UI_SPACER},
    {UI_STRING, NARGS_EXTRA, 1, 1, -1, 0, 0, {.stringvalue = {"", "", NULL}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_NARGS_AUTO_NOTE}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},
    // end of dialog
    {UI_TERMINATOR}};

struct UIItem diaOSDConfig[] = {
    {UI_HEADER, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OSD_SETTINGS}}},
    {UI_SPLITTER},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_SETTINGS_SOURCE}}},
    {UI_SPACER},
    {UI_ENUM, OSD_LANGUAGE_SOURCE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -40, 0, {.label = {NULL, _STR_ENABLE_LNG}}},
    {UI_SPACER},
    {UI_BOOL, OSD_LANGUAGE_ENABLE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -41, 0, {.label = {NULL, _STR_OSD_SETTINGS_LNG}}},
    {UI_SPACER},
    {UI_ENUM, OSD_LANGUAGE_VALUE, 1, 1, _STR_HINT_OSD_SETTINGS_LNG, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -41, 0, {.label = {NULL, _STR_OSD_SETTINGS_TVASPECT}}},
    {UI_SPACER},
    {UI_ENUM, OSD_TVASPECT_VALUE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    {UI_LABEL, 0, 1, 1, -1, -41, 0, {.label = {NULL, _STR_OSD_SETTINGS_VMODE}}},
    {UI_SPACER},
    {UI_ENUM, OSD_VMODE_VALUE, 1, 1, -1, 0, 0, {.intvalue = {0, 0}}},
    {UI_BREAK},

    // buttons
    {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}},
    {UI_BREAK},
    // end of dialog
    {UI_TERMINATOR}};
