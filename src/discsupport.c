/* Physical-disc RetroAchievements support, based on hacan359's RA disc mode.
 * Upstream: https://github.com/hacan359/Open-PS2-Loader/tree/cb713e686acb2fb63de20aa44b3fd4e8c59ca680
 * Academic Free License 3.0; see LICENSE.
 */
#include <stdio.h>
#include <string.h>

#include "include/opl.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/rahash.h"
#include "include/rawatch.h"
#include "include/ranet.h"
#include "include/gui.h"
#include "include/lang.h"
#include "include/ioman.h"
#include "include/iosupport.h"
#include "include/mmcesupport.h"
#include "include/discsupport.h"

static volatile int discBusy;

int discCheckBusy(void)
{
    return discBusy || sbHashGameBusy();
}

// The watch-list format keys games by their root boot executable (up to 15 chars).
static int discIdentity(char *bootPath, char *startup, void (*progress)(void))
{
    const char *name, *end;
    size_t len;
    if (sysGetDiscBootPath(bootPath, 64, progress) < 0)
        return -1;
    if (strncmp(bootPath, "cdrom0:", 7) != 0)
        return -1;
    name = bootPath + 7;
    if (*name == '\\' || *name == '/')
        name++;
    end = strchr(name, ';');
    len = end ? (size_t)(end - name) : strlen(name);
    if (len == 0 || len >= 16 || memchr(name, '\\', len) || memchr(name, '/', len))
        return -1;
    memcpy(startup, name, len);
    startup[len] = '\0';
    return 0;
}

static int discSupportPrefix(char *prefix, size_t size)
{
    const char *home = configGetHomePath();
    size_t len;
    if (home == NULL || home[0] == '\0')
        return -1;
    len = strlen(home);
    if (snprintf(prefix, size, "%s%s", home,
                 home[len - 1] == '/' || home[len - 1] == ':' ? "" : "/") >= (int)size)
        return -1;
    return 0;
}

static void discCheckWorker(void)
{
    char bootPath[64], startup[16], prefix[120], hash[33], info[96], info2[96];
    int result;
    if (discSupportPrefix(prefix, sizeof(prefix)) < 0) {
        guiShowRANotice(_l(_STR_RA_DISC_HOME_ERROR), NULL);
        goto done;
    }
    raHashLogOpen(prefix);
    raHashSetStepLog(&raHashStep);
    if (discIdentity(bootPath, startup, NULL) < 0) {
        guiShowRANotice(_l(_STR_DISC_LAUNCH_ERR), NULL);
        goto close_log;
    }
    // A failed re-check must not keep a stale set for this serial in memory.
    ClearWatchList();
    if (raHashDisc(bootPath, startup, hash) < 0) {
        guiShowRANotice(_l(_STR_RA_DISC_HASH_FAILED), NULL);
        goto close_log;
    }
    raHashLogAdd("(disc)", startup, hash);
    result = raAskPC(hash, startup, prefix, info, sizeof(info), info2, sizeof(info2));
    if (result == 0)
        guiShowRANotice(info[0] ? info : _l(_STR_RA_SUPPORTED),
                        info2[0] ? info2 : _l(_STR_RA_START_TO_TRACK));
    else if (result == 1)
        guiShowRANotice(_l(_STR_RA_UNKNOWN_IMAGE), hash);
    else if (result == -7)
        guiShowRANotice(_l(_STR_RA_STILL_IDENTIFYING), _l(_STR_RA_TRY_AGAIN));
    else if (result == -8)
        guiShowRANotice(_l(_STR_RA_NET_BUSY), _l(_STR_RA_NET_BUSY_HASH));
    else
        guiShowRANotice(_l(_STR_RA_PC_NO_ANSWER), _l(_STR_RA_PC_NO_ANSWER2));
close_log:
    raHashSetStepLog(NULL);
    raHashLogClose();
done:
    discBusy = 0;
}

int discCheckSupportDeferred(void)
{
    if (discCheckBusy())
        return 0;
    discBusy = 1;
    if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &discCheckWorker) != IO_OK) {
        discBusy = 0;
        return 0;
    }
    return 1;
}

void discLaunch(void (*progress)(void))
{
    char bootPath[64], startup[16], prefix[120];
    if (discCheckBusy()) {
        guiShowRANotice(_l(_STR_RA_CHECK_RUNNING), NULL);
        return;
    }
    if (!gRATelemetry) {
        guiShowRANotice(_l(_STR_RA_DISC_ENABLE), NULL);
        return;
    }
    if (discIdentity(bootPath, startup, progress) < 0) {
        guiShowRANotice(_l(_STR_DISC_LAUNCH_ERR), NULL);
        return;
    }
    if (discSupportPrefix(prefix, sizeof(prefix)) < 0) {
        guiShowRANotice(_l(_STR_RA_DISC_HOME_ERROR), NULL);
        return;
    }
    if (sbLoadWatchList(prefix, startup) <= 0) {
        guiShowRANotice(_l(_STR_RA_DISC_CHECK_FIRST), NULL);
        return;
    }
    mmceSendGameID(startup, NULL, 0);
    deinit(NO_EXCEPTION, IO_MODE_SELECTED_ALL);
    // Keep PS2LOGO's disc-authentication path; the EE core supplies telemetry.
    sysLaunchLoaderElf(startup, "DISC_MODE", 0, NULL, 0, NULL, 1, 0);
}
