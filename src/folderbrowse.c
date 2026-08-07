#include <stdio.h>
#include <string.h>

#include "include/opl.h" // umbrella: SDK base types (u32/GSTEXTURE) that iosupport.h depends on
#include "include/iosupport.h"
#include "include/folderbrowse.h"

// Set by opl.c from the "folder_nav" config key. Folder browsing is entirely inert unless this is
// on: scanForISO never emits a folder row and every helper below returns the root ("") state.
extern int gEnableFolderNav;

static char folderSub[MODE_COUNT][FOLDER_SUB_MAX]; // "/"-joined subpath below CD/DVD; "" == root
static unsigned char folderDirtyFlag[MODE_COUNT];  // 1 = descended/ascended/reset -> force one rescan
static unsigned char folderLevels[MODE_COUNT];     // pushed segment count (depth)

int folderModeSupported(int mode)
{
    // Loose-file tree scanners only (sbReadList/base_game_info_t, "/" separator): BDM (all 8 slots),
    // MMCE and UDPFS-Files. APA-HDD (HDL), the UDPFS block device and the VCD/POPS view do not scan a
    // CD/DVD directory tree, so they are excluded. SMB/ETH is deferred (uses "\\").
    return (mode >= BDM_MODE && mode <= BDM_MODE_LAST) || mode == MMCE_MODE || mode == UDPFS_MODE;
}

const char *folderGetSub(int mode)
{
    if (!gEnableFolderNav || mode < 0 || mode >= MODE_COUNT || !folderModeSupported(mode))
        return "";
    return folderSub[mode];
}

int folderDepth(int mode)
{
    if (!gEnableFolderNav || mode < 0 || mode >= MODE_COUNT || !folderModeSupported(mode))
        return 0;
    return folderLevels[mode];
}

int folderDescend(int mode, const char *name)
{
    if (!gEnableFolderNav || mode < 0 || mode >= MODE_COUNT || !folderModeSupported(mode))
        return 0;
    if (name == NULL || name[0] == '\0' || folderLevels[mode] >= FOLDER_DEPTH_MAX)
        return 0;

    char joined[FOLDER_SUB_MAX];
    int len;
    if (folderSub[mode][0] == '\0')
        len = snprintf(joined, sizeof(joined), "%s", name);
    else
        len = snprintf(joined, sizeof(joined), "%s/%s", folderSub[mode], name);

    // Refuse a join that would not fit: a truncated subpath would scan the WRONG directory, so the
    // user simply stays where they are (a folder with a name this long is not navigable). snprintf
    // returns the length it WOULD have written, so len >= buffer size means it did not fit.
    if (len < 0 || (size_t)len >= sizeof(folderSub[mode]))
        return 0;

    strcpy(folderSub[mode], joined);
    folderLevels[mode]++;
    folderDirtyFlag[mode] = 1;
    return 1;
}

int folderAscend(int mode)
{
    if (!gEnableFolderNav || mode < 0 || mode >= MODE_COUNT || !folderModeSupported(mode))
        return 0;
    if (folderLevels[mode] == 0)
        return 0;

    char *slash = strrchr(folderSub[mode], '/');
    if (slash != NULL)
        *slash = '\0'; // drop the last "/segment"
    else
        folderSub[mode][0] = '\0'; // back to root

    folderLevels[mode]--;
    folderDirtyFlag[mode] = 1;
    return 1;
}

void folderReset(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT)
        return;
    // Reset the raw state even for modes that are not folder-capable / when the toggle is off, so a
    // stale subpath can never leak across a device switch. Mark dirty only when something changed.
    if (folderSub[mode][0] != '\0' || folderLevels[mode] != 0) {
        folderSub[mode][0] = '\0';
        folderLevels[mode] = 0;
        folderDirtyFlag[mode] = 1;
    }
}

int folderConsumeDirty(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT || !folderDirtyFlag[mode])
        return 0;
    folderDirtyFlag[mode] = 0;
    return 1;
}
