#include "sys/fcntl.h"
#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/hddsupport.h"
#include "include/cuesupport.h" // Ember rows share the HDD PS1 list with the VCD ones
#include "include/vcdsupport.h" // HDD VCD view: vcdScanDirRoot + vcd_entry_t
#include "include/libview.h"    // libViewActive / libListViewActive -- which list this page shows
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/texcache.h" // cache quiesce before the POPSTARTER launch remounts pfs0:
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/mmcesupport.h" // mmceSendGameID() cross-device game-id (#261)
#include "modules/iopcore/common/cdvd_config.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioFormat, fileXioMount, fileXioUmount, fileXioDevctl
#include <io_common.h>   // FIO_MT_RDWR

#include <hdd-ioctl.h>
#include <delaythread.h> // DelayThread for the post-ATAD settle in hddLoadModules

#define OPL_HDD_MODE_PS2LOGO_OFFSET 0x17F8

#include "../modules/isofs/zso.h"

extern int probed_fd;
extern u32 probed_lba;
extern u8 IOBuffer[2048];

static unsigned char hddForceUpdate = 0;
static unsigned char hddHDProKitDetected = 0;
static unsigned char hddModulesLoadCount = 0;
static unsigned char hddModulesLoaded = 0;
static unsigned char hddSupportModulesLoaded = 0;
// One toast per failure streak: hddUpdateGameList now RETRIES the support-module load every refresh
// while it keeps failing, and re-toasting the same error box each pass would bury the UI. Reset on
// success so a later, different failure toasts again.
static unsigned char hddSupportErrToasted = 0;
static unsigned char hddPfsDeferredFailed = 0;
static int hddRetryQueued = 0;

// Settled PS2FS attempts that must ALL fail before Code 222 is shown. A drive still spinning up
// answers the ATA stack well before it will answer PS2FS, so a single refusal after the settle says
// nothing -- hardware shows PFS declining a few of these and then mounting normally. Reset by
// hddClearPfsDiagFailure() the moment the support stack latches.
#define HDD_PFS_REPORT_AFTER_FAILURES 3
static int hddPfsSettledFailures = 0;

typedef enum {
    HDD_PFS_DIAG_REASON_NONE = 0,
    HDD_PFS_DIAG_REASON_HDD_CHECK_STATUS_1,
    HDD_PFS_DIAG_REASON_PS2FS_LOAD_FAILURE,
} hdd_pfs_diag_reason_t;

#define HDD_PFS_DIAG_NOT_RUN (-9999)

static hdd_pfs_diag_reason_t hddPfsDiagReason = HDD_PFS_DIAG_REASON_NONE;
static int hddPfsDiagHddCheckResult = HDD_PFS_DIAG_NOT_RUN;
static int hddPfsDiagPs2fsResult = HDD_PFS_DIAG_NOT_RUN;
static unsigned char hddDiagBootStageActive = 0;
// A Settings selection is deliberately staged until Save Settings. It never changes the active
// pfs0: mount: a live OPL data home can have artwork/config readers using it.
static int hddOplHomePending = -1;
// After the selector is committed, retain its next-boot value for later saves in this process.
// The live pfs0: data home remains unchanged until the requested restart.
static int hddOplHomeCommitted = -1;
// +OPL can be the existing automatic home only when __common was unavailable during discovery.
// Keep that distinct from an explicit conf_hdd.cfg redirect, which may fall back to __common.
static unsigned char hddOplHomeAutoPlus = 0;

static const char *hddPfsDiagReasonName(hdd_pfs_diag_reason_t reason)
{
    switch (reason) {
        case HDD_PFS_DIAG_REASON_HDD_CHECK_STATUS_1:
            return "HDD_CHECK_STATUS_1_UNFORMATTED";
        case HDD_PFS_DIAG_REASON_PS2FS_LOAD_FAILURE:
            return "PS2FS_LOAD_FAILURE";
        default:
            return "NONE";
    }
}

static void hddLogPfsDiagState(const char *action, const char *why)
{
    LOG("[HDD PFS DIAG] action=%s why=%s reason=%s hddCheck=%d ps2fs=%d "
        "hddModulesLoaded=%u hddSupportModulesLoaded=%u gOPLPart=\"%s\" gHDDPrefix=\"%s\"\n",
        action, why, hddPfsDiagReasonName(hddPfsDiagReason), hddPfsDiagHddCheckResult,
        hddPfsDiagPs2fsResult, hddModulesLoaded, hddSupportModulesLoaded, gOPLPart,
        gHDDPrefix != NULL ? gHDDPrefix : "<null>");
}

static void hddArmPfsDiagFailure(hdd_pfs_diag_reason_t reason, int hddCheckResult, int ps2fsResult)
{
    hddPfsDiagReason = reason;
    hddPfsDiagHddCheckResult = hddCheckResult;
    hddPfsDiagPs2fsResult = ps2fsResult;
    hddPfsDeferredFailed = 1;
    hddLogPfsDiagState("arm", "support-load-failure");
}

static void hddClearPfsDiagFailure(const char *why)
{
    hddLogPfsDiagState("clear", why);
    hddPfsDeferredFailed = 0;
    hddPfsSettledFailures = 0; // PFS came up: the retries that failed before it were spin-up, not fault
    hddPfsDiagReason = HDD_PFS_DIAG_REASON_NONE;
    hddPfsDiagHddCheckResult = HDD_PFS_DIAG_NOT_RUN;
    hddPfsDiagPs2fsResult = HDD_PFS_DIAG_NOT_RUN;
}

// Diagnostic-build only, same rule as bdmDiagBootStage*: this publishes untranslated developer text
// to the boot status line that every other caller feeds through _l(). hddDiagBootStageActive
// additionally scopes it to the bracket opened by hddDiagLoadModulesReady(), so even a diagnostic
// build only narrates the ATA startup it was asked to narrate.
#ifdef __OPLDIAG
static void hddDiagBootStageBegin(const char *stage)
{
    char status[96];

    if (!hddDiagBootStageActive)
        return;

    snprintf(status, sizeof(status), "BEGIN %s", stage);
    LOG("%s\n", status);
    guiSetBootStatusStickyCopy(status);
}

static void hddDiagBootStageEnd(const char *stage, int result)
{
    char status[96];

    if (!hddDiagBootStageActive)
        return;

    snprintf(status, sizeof(status), "END %s result=%d", stage, result);
    LOG("%s\n", status);
    guiSetBootStatusStickyCopy(status);
}

static void hddDiagBootStageEndVoid(const char *stage)
{
    char status[96];

    if (!hddDiagBootStageActive)
        return;

    snprintf(status, sizeof(status), "END %s", stage);
    LOG("%s\n", status);
    guiSetBootStatusStickyCopy(status);
}
#else
static void hddDiagBootStageBegin(const char *stage)
{
    (void)stage;
}

static void hddDiagBootStageEnd(const char *stage, int result)
{
    (void)stage;
    (void)result;
}

static void hddDiagBootStageEndVoid(const char *stage)
{
    (void)stage;
}
#endif

static void hddClearRecoveredErrors(void)
{
    // Do not blank an unrelated queued notification. These are the only messages emitted by this
    // support path, and each call is made only after the underlying APA/PFS check succeeded.
    clearErrorMessageIf(_STR_HDD_NOT_CONNECTED_ERROR);
    clearErrorMessageIf(_STR_HDD_NOT_FORMATTED_ERROR);
    clearErrorMessageIf(_STR_HDD_UNAVAILABLE_ERROR);
    clearErrorMessageIf(_STR_HDD_PFS_UNAVAILABLE_ERROR);
}

static char *hddPrefix = "pfs0:";
static hdl_games_list_t hddGames;

// How long the HDD VCD launch waits for the art worker to let go before it remounts pfs0: under it.
// Milliseconds (cacheCancelPendingImageLoadsTimed sleeps via DelayThread(1000), NOT util.c's delay(),
// whose "tick" is a ~0.25 s NOP spin). Generous because an un-abortable whole-file cover read on this
// device has been measured at several seconds, and the alternative is remounting under a live reader.
#define HDD_ART_QUIESCE_MS 2000

// HDD VCD view: PS1 games gathered from pooled __.POPS[0-9]? partitions and one-game PP. / __.
// partitions. hddVcdParts is index-parallel to hddVcdGames -- it records the full owning partition
// label for the launch handoff.
static base_game_info_t *hddVcdGames = NULL;
static int hddVcdGameCount = 0;
static char (*hddVcdParts)[APA_IDMAX + 1] = NULL;

// forward declaration
static item_list_t hddGameList;

static int hddLoadGameListCache(hdl_games_list_t *cache);
static int hddUpdateGameListCache(hdl_games_list_t *cache, hdl_games_list_t *game_list);

static void hddInitModules(void)
{
    hddRetryQueued = 0;
    hddLoadModules();
    hddLoadSupportModules();

    // Existing-partitions-only discovery may legitimately fail closed. Do not dereference a
    // missing data home, and do not create any folders until a PFS partition is actually mounted.
    if (gHDDPrefix == NULL) {
        LOG("HDDSUPPORT InitModules: no existing PFS data home mounted\n");
        return;
    }

    // update Themes
    char path[256];
    snprintf(path, sizeof(path), "%sTHM", gHDDPrefix);
    thmAddElements(path, "/", 1);

    snprintf(path, sizeof(path), "%sLNG", gHDDPrefix);
    lngAddLanguages(path, "/", hddGameList.mode);

    // Create normal OPL folders only inside the selected mounted PFS data home. A configured
    // existing APA partition owns its root; the canonical __common fallback owns __common/OPL/.
    // Never create an APA partition to obtain a home.
    sbCreateFolders(gHDDPrefix, 0);
}

// HD Pro Kit is mapping the 1st word in ROM0 seg as a main ATA controller,
// The pseudo ATA controller registers are accessed (input/ouput) by writing
// an id to the main ATA controller
#define HDPROreg_IO8   (*(volatile unsigned char *)0xBFC00000)
#define CDVDreg_STATUS (*(volatile unsigned char *)0xBF40200A)

static int hddCheckHDProKit(void)
{
    int ret = 0;

    DIntr();
    ee_kmode_enter();
    // HD Pro IO start commands sequence
    HDPROreg_IO8 = 0x72;
    CDVDreg_STATUS = 0;
    HDPROreg_IO8 = 0x34;
    CDVDreg_STATUS = 0;
    HDPROreg_IO8 = 0x61;
    CDVDreg_STATUS = 0;
    u32 res = HDPROreg_IO8;
    CDVDreg_STATUS = 0;

    // check result
    if ((res & 0xff) == 0xe7) {
        // HD Pro IO finish commands sequence
        HDPROreg_IO8 = 0xf3;
        CDVDreg_STATUS = 0;
        ret = 1;
    }
    ee_kmode_exit();
    EIntr();

    if (ret)
        LOG("HDDSUPPORT HD Pro Kit detected!\n");

    return ret;
}

static int hddCheckOPLFolder(const char *mountPoint)
{
    DIR *dir;
    char path[32];
    int n;

    n = snprintf(path, sizeof(path), "%sOPL", mountPoint);
    if (n < 0 || n >= (int)sizeof(path))
        return 0;

    dir = opendir(path);
    if (dir != NULL) {
        closedir(dir);
        return 1;
    }

    if (mkdir(path, 0777) == 0)
        return 1;

    // A competing normal-folder creation can win the race. Treat that existing folder as success,
    // but never report a usable common-home target when the directory still cannot be opened.
    dir = opendir(path);
    if (dir == NULL)
        return 0;
    closedir(dir);
    return 1;
}

static int hddPartitionMountable(const char *partition)
{
    int ret;

    // Discovery is deliberately non-creating at the APA level: fileXioMount can only
    // attach an existing partition. Never probe availability with open(..., O_CREAT),
    // because hddN: is the raw APA namespace and O_CREAT means APA partition creation.
    fileXioUmount(hddPrefix);
    ret = fileXioMount(hddPrefix, partition, FIO_MT_RDWR);
    if (ret == 0)
        fileXioUmount(hddPrefix);

    return ret == 0;
}

static void hddFindOPLPartition(void)
{
    config_set_t *config;
    char name[64] = {0};
    char candidate[sizeof(gOPLPart)];
    const char *label;

    /* ORDER OF PREFERENCE, and the middle two are deliberately swapped from what this used to do:
     *
     *   1. an explicit conf_hdd.cfg selection  -- a stated choice outranks any default
     *   2. an existing +OPL partition          -- PREFERRED default
     *   3. __common                            -- fallback
     *   4. nothing: fail closed
     *
     * +OPL used to sit at 3 and __common at 2, so a drive that has both silently landed on __common
     * and the only route to +OPL was the Settings row -- which is exactly the path that has been
     * failing. Preferring the partition the user actually created means the common case needs no
     * setting at all.
     *
     * Discovery still never manufactures a partition, resizes one, or writes raw hdd0: metadata.
     * Every branch below only MOUNTS something that already exists, and the last one gives up
     * rather than create anything. */
    int commonAvailable = 0;

    hddOplHomeAutoPlus = 0;
    fileXioUmount(hddPrefix);
    if (fileXioMount(hddPrefix, "hdd0:__common", FIO_MT_RDONLY) == 0) {
        config = configAlloc(0, NULL, "pfs0:OPL/conf_hdd.cfg");
        if (config != NULL) {
            if (configRead(config))
                configGetStrCopy(config, "hdd_partition", name, sizeof(name));
            configFree(config);
        }
        fileXioUmount(hddPrefix);

        if (name[0] != '\0') {
            // The historical value is a bare APA ID (for example +OPL). Accept an hdd0:
            // spelling too, but never silently redirect this drive's owner to hdd1.
            if (!strncmp(name, "hdd0:", 5))
                label = name + 5;
            else if (!strncmp(name, "hdd", 3))
                label = NULL;
            else
                label = name;

            // conf_hdd.cfg names one APA partition label, not a raw hdd device or a PFS path.
            // Reject delimiters and an empty/oversized label before passing anything to the mount
            // RPC, so an invalid redirect can only select the documented __common fallback.
            if (label == NULL || label[0] == '\0' || strlen(label) > APA_IDMAX ||
                strpbrk(label, ":/\\") != NULL)
                candidate[0] = '\0';
            else
                snprintf(candidate, sizeof(candidate), "hdd0:%s", label);

            if (candidate[0] != '\0' && hddPartitionMountable(candidate)) {
                snprintf(gOPLPart, sizeof(gOPLPart), "%s", candidate);
                LOG("HDD: using configured existing data partition %s\n", gOPLPart);
                return;
            }
            LOG("HDD: configured data partition %s is unavailable; ignoring it\n", name);
        }

        // __common mounted but named nothing usable. Record that it is available and keep going --
        // +OPL is preferred below when it exists. The successful mount above is already all the
        // proof this needs; do not remount merely to rediscover the same topology.
        commonAvailable = 1;
    }

    // PREFERRED DEFAULT: an existing +OPL. It owns its PFS root directly, so the mounted data
    // prefix is pfs0:. Still an automatic effective choice, not a request to write conf_hdd.cfg or
    // to create an APA partition -- taken only when the partition is already there and mountable.
    if (hddPartitionMountable("hdd0:+OPL")) {
        snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:+OPL");
        hddOplHomeAutoPlus = 1;
        LOG("HDD: using existing +OPL data home (preferred over __common)\n");
        return;
    }

    // Fallback: the canonical __common/OPL/ layout, already proven mountable above.
    if (commonAvailable) {
        snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:__common");
        LOG("HDD: no +OPL partition; using canonical __common/OPL/ fallback\n");
        return;
    }

    // Nothing suitable exists. Leave the data partition unresolved and fail closed.
    gOPLPart[0] = '\0';
    LOG("HDD: no existing PFS data partition is available; refusing to create one\n");
}

int hddLoadModules(void)
{
    int retLoadModule = HDD_PFS_DIAG_NOT_RUN;
    int retBdmModule = HDD_PFS_DIAG_NOT_RUN;
    int retXhddModule = HDD_PFS_DIAG_NOT_RUN;
    int retStatus = HDD_LOADMODULES_STATUS_UNK;

    LOG("[HDD STARTUP DIAG] hddLoadModules entry count=%u loaded=%u\n", hddModulesLoadCount, hddModulesLoaded);

    if (hddModulesLoaded)
        retStatus = HDD_LOADMODULES_STATUS_ALREADYLOADED;

    if (hddModulesLoadCount == 0) {
        // Increment the load count as soon as possible to prevent thread scheduling from allowing another thread to
        // call into here and try to double load modules.
        hddModulesLoadCount = 1;

        // DEV9 must be loaded, as HDD.IRX depends on it. Even if not required by the I/F (i.e. HDPro)
        hddDiagBootStageBegin("HDD:DEV9");
        sysInitDev9();
        hddDiagBootStageEndVoid("HDD:DEV9");

        // try to detect HD Pro Kit (not the connected HDD),
        // if detected it loads the specific ATAD module
        hddDiagBootStageBegin("HDD:HDPRO-PROBE");
        hddHDProKitDetected = hddCheckHDProKit();
        hddDiagBootStageEnd("HDD:HDPRO-PROBE", hddHDProKitDetected);
        if (hddHDProKitDetected) {
            LOG("[ATAD_HDPRO]:\n");
            hddDiagBootStageBegin("HDD:ATAD-HDPRO");
            retLoadModule = sysLoadModuleBuffer(&hdpro_atad_irx, size_hdpro_atad_irx, 0, NULL);
            hddDiagBootStageEnd("HDD:ATAD-HDPRO", retLoadModule);
            LOG("[XHDD]:\n");
            hddDiagBootStageBegin("HDD:XHDD-HDPRO");
            retXhddModule = sysLoadModuleBuffer(&xhdd_irx, size_xhdd_irx, 6, "-hdpro");
            hddDiagBootStageEnd("HDD:XHDD-HDPRO", retXhddModule);
        } else {
            LOG("[BDM]:\n");
            hddDiagBootStageBegin("HDD:BDM");
            retBdmModule = sysLoadModuleBuffer(&bdm_irx, size_bdm_irx, 0, NULL);
            hddDiagBootStageEnd("HDD:BDM", retBdmModule);
            LOG("[ATAD]:\n");
            hddDiagBootStageBegin("HDD:ATAD");
            retLoadModule = sysLoadModuleBuffer(&ps2atad_irx, size_ps2atad_irx, 0, NULL);
            hddDiagBootStageEnd("HDD:ATAD", retLoadModule);
            LOG("[XHDD]:\n");
            hddDiagBootStageBegin("HDD:XHDD");
            retXhddModule = sysLoadModuleBuffer(&xhdd_irx, size_xhdd_irx, 0, NULL);
            hddDiagBootStageEnd("HDD:XHDD", retXhddModule);
        }

        if (retLoadModule < 0) {
            LOG("HDD: No HardDisk Drive detected.\n");
            setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_IF_NOT_DETECTED);
            retStatus = HDD_LOADMODULES_STATUS_ERROR;
            // Make the failure RETRYABLE. Leaving the count consumed turned one bad first probe into a
            // whole-session poison: every later call took the else branch and returned BUSYLOADING(2),
            // which the >= 0 caller tests read as SUCCESS while nothing was loaded -- so the APA page
            // sat silently empty under BOTH Auto and Manual (Vapor's report; the drive lists fine in
            // wOPL/upstream because their earliest callers run at tab entry, when the drive is ready --
            // our fork adds boot-time callers like bdmResolveBootDir's ATA escalation, seconds after
            // power-on). sysInitDev9/sysLoadModuleBuffer are safe to re-run; a later caller (e.g. the
            // HDD tab) now gets a real second attempt instead of a poisoned latch.
            //
            // Release the dev9 reference taken above before clearing the count, or the retry that this
            // line exists to permit takes a SECOND one and never gives either back. dev9 is refcounted
            // and shared with ETH/UDPBD, so an inflated count means a later teardown can never power
            // dev9 down. The UDPBD arm in bdmsupport.c already pairs its init/shutdown this way; this
            // arm did not, and rebuild-153's device-refresh bump made the retry more frequent.
            sysShutdownDev9();
            hddModulesLoadCount = 0;
        } else {
            retStatus = HDD_LOADMODULES_STATUS_NOERROR;
            hddModulesLoaded = 1;
            // Settle ~1 s after a FRESH ATA-stack load before anyone probes xhdd0: or loads ps2hdd.
            // Every working reference does this: NHDDL sleeps 1 s after ata_bd ("prevents ps2hdd from
            // hanging") and POPSLoader settles 1 s around ata_bd as well. Our earliest callers run
            // seconds after power-on (APA boot-identity config resolution), so the very first
            // ATA_DEVCTL_READ_PARTITION_SECTOR probe / ps2hdd init can otherwise race a drive that is
            // still spinning up. Runs once per load generation: the dedupe branch above never gets here.
            hddDiagBootStageBegin("HDD:SETTLE");
            DelayThread(1000 * 1000);
            hddDiagBootStageEndVoid("HDD:SETTLE");
        }
    } else {
        hddModulesLoadCount++;
        if (!hddModulesLoaded)
            retStatus = HDD_LOADMODULES_STATUS_BUSYLOADING;
    }

    LOG("[HDD STARTUP DIAG] hddLoadModules exit count=%u loaded=%u ret=%d bdm=%d atad=%d xhdd=%d\n",
        hddModulesLoadCount, hddModulesLoaded, retStatus, retBdmModule, retLoadModule, retXhddModule);
    return retStatus;
}

int hddDiagLoadModulesReady(void)
{
    int result;

    hddDiagBootStageActive = 1;
    hddDiagBootStageBegin("HDD:READY");
    LOG("[HDD STARTUP DIAG] hddLoadModulesReady entry count=%u loaded=%u\n", hddModulesLoadCount, hddModulesLoaded);
    result = hddLoadModulesReady();
    LOG("[HDD STARTUP DIAG] hddLoadModulesReady exit count=%u loaded=%u result=%d\n",
        hddModulesLoadCount, hddModulesLoaded, result);
    hddDiagBootStageEnd("HDD:READY", result);
    hddDiagBootStageActive = 0;

    return result;
}

int hddModulesAreLoaded(void)
{
    return hddModulesLoaded != 0;
}

// Validate an APA header sector without ps2hdd: the "APA" magic plus the header checksum
// (sum of the 127 little-endian words after the checksum word itself, per ps2sdk apaCheckSum).
static int hddApaHeaderValid(const u8 *pSectorData)
{
    const u32 *pWords = (const u32 *)pSectorData;
    u32 sum = 0;
    int i;

    if (memcmp(pSectorData + 4, "APA", 3) != 0)
        return 0;

    for (i = 1; i < 128; i++)
        sum += pWords[i];

    return sum == pWords[0];
}

// Returns 1 for MBR/GPT, 0 for APA, and -1 if an error occured
int hddDetectNonSonyFileSystem()
{
    int result = -1;
    // Allocate memory for storing data for the first two sectors.
    u8 *pSectorData = (u8 *)malloc(512 * 2);
    if (pSectorData == NULL) {
        LOG("hddDetectNonSonyFileSystem: failed to allocate scratch memory\n");
        return -1;
    }

    // Trying to load the APA/PFS irx modules when a non-sony formatted HDD is connected (ie: MBR/GPT  w/ exFAT) runs
    // the risk of corrupting the HDD. To avoid that get the first two sectors and perform some sanity checks. If
    // we reasonably suspect the disk is not APA formatted bail out from loading the sony fs irx modules.
    result = fileXioDevctl("xhdd0:", ATA_DEVCTL_READ_PARTITION_SECTOR, NULL, 0, pSectorData, 512 * 2);
    if (result < 0) {
        LOG("hddDetectNonSonyFileSystem: failed to read data from hdd %d\n", result);
        free(pSectorData);
        return -1;
    }

    // Check for a valid APA header FIRST, and only treat MBR/GPT evidence as decisive when no valid
    // APA header exists. The MBR test used to win: but a modern formatter (ps2sdk's GPT-capable
    // ps2hdd, PC-side POPS/HDL partition tools) legitimately stamps a protective/residual 0x55AA at
    // bytes 510-511 of the __mbr header while the sector is STILL a fully valid, checksummed APA
    // header. Such a drive was misclassified as MBR and ps2hdd never loaded -- silently (that branch
    // raises no error by design), leaving the APA page empty with zero HDL/PFS content while ATA
    // itself worked fine. The checksummed APA magic is far stronger evidence than two signature
    // bytes; a genuine exFAT/MBR/GPT drive has no valid APA header and still bails below.
    if (memcmp((const char *)&pSectorData[4], "APA", 3) == 0) {
        if (hddApaHeaderValid(pSectorData)) {
            // Found APA partition type.
            LOG("hddDetectNonSonyFileSystem: found APA partition data\n");
            result = 0;
        } else {
            // APA magic with a BAD checksum: fail closed. Do not let an accompanying 0x55AA
            // reclassify a possibly-corrupt APA drive as safe-to-ignore MBR media either.
            LOG("hddDetectNonSonyFileSystem: APA magic present but header checksum invalid\n");
            result = -1;
        }
    } else if (pSectorData[0x1FE] == 0x55 && pSectorData[0x1FF] == 0xAA) {
        // Found MBR partition type.
        LOG("hddDetectNonSonyFileSystem: found MBR partition data\n");
        result = 1;
    } else if (strncmp((const char *)&pSectorData[0x200], "EFI PART", 8) == 0) {
        // Found GPT partition type.
        LOG("hddDetectNonSonyFileSystem: found GPT partition data\n");
        result = 1;
    } else {
        // Even though we didn't find evidence of non-APA partition data, if we load the APA irx module
        // it will write to the drive and potentially corrupt any data that might be there.
        LOG("hddDetectNonSonyFileSystem: partition data not recognized\n");
        result = -1;
    }

    // Cleanup and return.
    free(pSectorData);
    return result;
}

// Bring up only the APA/PFS support needed for a read-only pfs1: probe. This intentionally does
// not discover, mount, or create the persistent pfs0: OPL data home: Settings uses it to validate
// an already-existing selector target without changing the live data-home state.
static int hddLoadCoreSupportModules(void)
{
    static char hddarg[] = "-o"
                           "\0"
                           "4"
                           "\0"
                           "-n"
                           "\0"
                           "20";
    // No leading "\0": LOADFILE already supplies argv[0] ("LBbyEE") for buffer loads, so the first
    // string here is argv[1]. PFS stops parsing at the first token that doesn't start with '-', so a
    // leading empty string silently discarded EVERY option below (upstream OPL carries the same dead
    // byte) -- leaving PFS at its defaults (-m 1, -o 2, -n 8) and making the pfs1: scan mount fail.
    static char pfsarg[] = "-m" // max mounts: keep pfs0: on OPL data while pfs1: scans POPS partitions
                           "\0"
                           "2"
                           "\0"
                           "-o" // max open
                           "\0"
                           "10" // Default value: 2
                           "\0"
                           "-n" // Number of buffers
                           "\0"
                           "40"; // Default value: 8 | Max value: 127
    int ret;
    int nonSony;

    LOG("HDDSUPPORT LoadSupportModules\n");

    // ALREADY PROVEN -- do not re-litigate it. The non-Sony probe below exists to avoid loading APA
    // modules onto a non-APA drive, which is a one-time question: if hddSupportModulesLoaded is set
    // then ps2hdd and PS2FS are up, and that cannot have happened on a drive this probe would
    // reject. Re-running it on every call re-asks a settled question against a live drive, and
    // hddDetectNonSonyFileSystem() answers -1 on a transient devctl error (busy bus, drive mid-seek)
    // as well as >0 on a coexisting exFAT/MBR signature. Either one made this return 0 while APA was
    // demonstrably working -- which is how the APA data-home selector came to report failure on a
    // console that was browsing HDD games at the time.
    //
    // Placed above the probe, not merged into the existing !hddSupportModulesLoaded guard below,
    // because that guard only covers the module loads -- the probe already ran by the time it is
    // reached.
    if (hddSupportModulesLoaded)
        return 1;

    // Check if the drive contains MBR/GPT partition data before we load the APA/PFS modules. If the drive is not
    // APA then loading the APA irx modules can corrupt the drive as it will try to write APA partition data.
    nonSony = hddDetectNonSonyFileSystem();
    if (nonSony != 0) {
        // Drive is MBR/GPT style, or unknown, bail out or risk corrupting the drive.
        LOG("HDDSUPPORT LoadSupportModules bailing out early (%d)...\n", nonSony);
        // A PROBE FAILURE (-1: the xhdd0: partition-sector devctl errored -- drive not ready, module
        // missing, transient bus fault) was completely SILENT: no error box, no list, an APA page that
        // just looks like a dead drive for the whole session. Surface it. The 1 (genuine MBR/GPT/exFAT)
        // branch stays silent on purpose -- that is the NORMAL coexistence case for BDM-HDD users and
        // must not toast at every boot.
        if (nonSony < 0 && !hddSupportErrToasted) {
            setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_NOT_DETECTED);
            hddSupportErrToasted = 1;
        } else if (nonSony > 0) {
            // A recognized MBR/GPT/exFAT disk is normal BDM territory, not an APA formatting
            // failure. It also proves a prior transient APA probe error is stale.
            hddSupportErrToasted = 0;
            hddClearRecoveredErrors();
        }
        return 0;
    }

    if (!hddSupportModulesLoaded) {
        LOG("[HDD]:\n");
        ret = sysLoadModuleBuffer(&ps2hdd_irx, size_ps2hdd_irx, sizeof(hddarg), hddarg);
        if (ret < 0) {
            LOG("HDD: No HardDisk Drive detected.\n");
            if (!hddSupportErrToasted) {
                setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_MODULE_HDD_FAILURE);
                hddSupportErrToasted = 1;
            }
            return 0;
        }

        // hddCheck distinguishes a connected formatted APA disk (0), a genuinely unformatted one
        // (1), an unusable drive (2), and no usable drive (<0). Only status 1 earns the specific
        // "not formatted" wording; a PFS module/load or selected-data-home problem is not proof of
        // that condition.
        ret = hddCheck();
        if (ret < 0) {
            LOG("HDD: No HardDisk Drive detected.\n");
            if (!hddSupportErrToasted) {
                setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_NOT_DETECTED);
                hddSupportErrToasted = 1;
            }
            return 0;
        }
        if (ret == 1) {
            LOG("HDD: APA status reports an unformatted drive.\n");
            hddSupportErrToasted = 0;
            hddArmPfsDiagFailure(HDD_PFS_DIAG_REASON_HDD_CHECK_STATUS_1, ret, HDD_PFS_DIAG_NOT_RUN);
            return 0;
        }
        if (ret == 2) {
            LOG("HDD: APA status reports an unavailable drive.\n");
            if (!hddSupportErrToasted) {
                setErrorMessageWithCode(_STR_HDD_UNAVAILABLE_ERROR, ERROR_HDD_NOT_DETECTED);
                hddSupportErrToasted = 1;
            }
            return 0;
        }

        LOG("[PS2FS]:\n");
        ret = sysLoadModuleBuffer(&ps2fs_irx, size_ps2fs_irx, sizeof(pfsarg), pfsarg);
        if (ret < 0) {
            LOG("HDD: PFS support module failed to load.\n");
            hddSupportErrToasted = 0;
            hddArmPfsDiagFailure(HDD_PFS_DIAG_REASON_PS2FS_LOAD_FAILURE, 0, ret);
            return 0;
        }

        hddSupportModulesLoaded = 1;
        hddClearPfsDiagFailure("core-support-load-succeeded");
        hddSupportErrToasted = 0;
        hddClearRecoveredErrors();
        LOG("HDDSUPPORT modules loaded\n");
    }

    return 1;
}

void hddLoadSupportModules(void)
{
    int ret;

    if (!hddLoadCoreSupportModules())
        return;

    // The modules and the persistent pfs0: data mount have separate lifetimes. If an
    // existing partition was temporarily unavailable, keep the modules loaded and retry
    // discovery/mount on a later call instead of poisoning the session or reloading IRXes.
    if (gHDDPrefix != NULL)
        return;

    if (gOPLPart[0] == '\0')
        hddFindOPLPartition();

    if (gOPLPart[0] == '\0') {
        // HDL/VCD enumeration can still work without a persistent OPL data home. A missing or
        // stale redirect is not evidence that the APA disk is unformatted, so retry discovery
        // later without poisoning the user with a false formatting warning.
        hddSupportErrToasted = 0;
        return;
    }

    fileXioUmount(hddPrefix);
    ret = fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR);

    // A configured data partition may disappear or cease to mount. Never respond by
    // creating/reformatting APA metadata. An automatic +OPL home was chosen because __common
    // was unavailable, so fail closed rather than probing it again. An explicit redirect keeps
    // the established existing-__common fallback.
    if (ret < 0 && strcmp(gOPLPart, "hdd0:__common") != 0 && !hddOplHomeAutoPlus) {
        LOG("HDD: could not mount %s (%d); trying existing __common/OPL/ fallback\n", gOPLPart, ret);
        fileXioUmount(hddPrefix);
        ret = fileXioMount(hddPrefix, "hdd0:__common", FIO_MT_RDWR);
        if (ret == 0)
            snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:__common");
    }

    if (ret < 0) {
        LOG("HDD: no existing PFS data partition could be mounted (%d); no partition was created\n", ret);
        // Force the next call to rediscover. This does not unload/reload the APA/PFS modules.
        gOPLPart[0] = '\0';
        gHDDPrefix = NULL;
        hddSupportErrToasted = 0;
        return;
    }

    hddSupportErrToasted = 0;
    hddClearRecoveredErrors();
    if (!strcmp(gOPLPart, "hdd0:__common")) {
        (void)hddCheckOPLFolder(hddPrefix);
        gHDDPrefix = "pfs0:OPL/";
    } else {
        gHDDPrefix = "pfs0:";
    }

    // A prior list pass may have parked relative HDD artwork as unavailable while no persistent
    // PFS home existed. Re-arm those misses exactly when the home becomes usable again.
    cacheInvalidateFailMemo();
}

void hddInit(item_list_t *itemList)
{
    LOG("HDDSUPPORT Init\n");
    hddForceUpdate = 0; // Use cache at initial startup.
    hddGameList.delay = gArtDelay;
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &hddInitModules);
    hddGameList.enabled = 1;
}

item_list_t *hddGetObject(int initOnly)
{
    if (initOnly && !hddGameList.enabled)
        return NULL;
    return &hddGameList;
}

// ---- HDD VCD view (PS1 games on APA/PFS partitions) ---------------------------------------

// Once-per-session latch (POPSLoader's HAS_CHECKED parity: "HDD is checked only once since it cannot
// be removed/replaced without damaging the console"). While set, hddUpdateGameList's VCD branch reuses
// the built arrays so an L3 toggle rebuilds only the submenu, never re-walks the partitions. A latch
// (not hddVcdGames != NULL) so a drive whose candidates scanned to ZERO VCDs is also remembered.
// Each POPS/Ember partition enumerator distinguishes a successful zero-candidate walk from a failed
// APA walk; only a complete two-phase scan may replace/latch the current combined PS1 list.
// Cleared by hddFreeVcdGameList (covers rebuilds + hddCleanUp/hddShutdown teardown) and by
// hddVcdInvalidateCache (the first-disc-only setting filters at scan time, so its change must rescan).
// The generation counter keeps an invalidation from being SWALLOWED by a build already in flight on
// the IO worker (GUI-thread dialog save during a cold scan): the build re-latches only if no
// invalidation arrived since it started.
static unsigned char hddVcdListBuilt = 0;
static volatile unsigned int hddVcdCacheGen = 0;

void hddVcdInvalidateCache(void)
{
    hddVcdCacheGen++;
    hddVcdListBuilt = 0;
}

static void hddPublishVcdGameList(base_game_info_t *games, char (*parts)[APA_IDMAX + 1], int count, int built)
{
    base_game_info_t *oldGames;
    char(*oldParts)[APA_IDMAX + 1];

    guiLock();
    oldGames = hddVcdGames;
    oldParts = hddVcdParts;
    hddVcdGames = games;
    hddVcdParts = parts;
    hddVcdGameCount = count;
    hddVcdListBuilt = built;
    free(oldGames);
    free(oldParts);
    guiUnlock();
}

static void hddFreeVcdGameList(void)
{
    hddPublishVcdGameList(NULL, NULL, 0, 0);
}

static void hddPublishHdlGameList(hdl_games_list_t *replacement)
{
    hdl_games_list_t old;

    if (replacement == NULL)
        return;

    guiLock();
    old = hddGames;
    hddGames = *replacement;
    replacement->games = NULL;
    replacement->count = 0;
    hddFreeHDLGamelist(&old);
    guiUnlock();
}

// Build the HDD VCD game list from both supported APA/PFS shapes. Exact __.POPS / __.POPS0..9
// containers are always mounted and contribute every root *.VCD. PP.<DISC-ID>.POPS.<NAME> one-game
// installs (vcdPopsPartitionTitleOffset) contribute one entry each from the APA table alone, ZERO
// mounts, gated by the enumeration-only gVcdShowPpPops setting. Every other PP.*/__.* label is
// IGNORED with no mount and no probe: the literal ".POPS." marker is what separates a POPS
// partition from an ordinary ID-labelled HDDOSD/HDL one (e.g. PP.SLUS-21025.01.BATTLEFIELD_2_H),
// so a drive full of app/game partitions no longer pays a PFS mount/probe/umount cycle per label.
// Mounts use the dedicated pfs1: scan slot; pfs0: stays on the OPL data partition throughout.

static int hddBuildVcdGameList(void)
{
    hdd_pops_list_t parts;
    base_game_info_t *newGames = NULL;
    char(*newParts)[APA_IDMAX + 1] = NULL;
    int total = 0;
    int scanIncomplete = 0;
    unsigned int genAtEntry = hddVcdCacheGen; // re-latch below only if no invalidation raced this build

    // Best-effort cleanup from an interrupted prior scan. This never touches pfs0:, which remains the
    // live OPL data mount used by both PS2 and VCD config/art lookups.
    fileXioUmount("pfs1:");

    int partCount = hddGetPopsPartitionList(&parts);
    if (partCount < 0) {
        // TRANSACTIONAL LIST OWNERSHIP: a failed APA table walk is not an empty VCD library. Keep the
        // last-good arrays exactly as they are and leave the cache unlatched so a later explicit
        // refresh can retry.
        LOG("HDD VCD: APA partition enumeration failed (%d); preserving %d last-good game(s)\n", partCount, hddVcdGameCount);
        hddVcdListBuilt = 0;
        return hddVcdGameCount;
    }
    // Zero POPSTARTER candidates is authoritative only for the POPS half of this combined PS1
    // library. Do not publish an empty list yet: an Ember-only drive may still have __.EMBER or
    // __.EMBER0..9 containers, which are collected by the second phase below.

    for (int p = 0; p < parts.count; p++) {
        char mountSrc[64];
        snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", parts.names[p]);

        // PP.<DISC-ID>.POPS.<name> one-game install: strict label match from the APA table alone,
        // ZERO mounts. Display the title past the ".POPS." marker and retain the FULL label for
        // launch. Enumeration-only setting: launch semantics are untouched either way.
        int titleOfs = vcdPopsPartitionTitleOffset(parts.names[p]);
        if (titleOfs > 0) {
            if (!gVcdShowPpPops)
                continue;

            base_game_info_t *grownGames = realloc(newGames, (total + 1) * sizeof(base_game_info_t));
            if (grownGames == NULL) {
                scanIncomplete = 1;
                break;
            }
            newGames = grownGames;
            char(*grownParts)[APA_IDMAX + 1] = realloc(newParts, (total + 1) * sizeof(*newParts));
            if (grownParts == NULL) {
                scanIncomplete = 1;
                break;
            }
            newParts = grownParts;

            base_game_info_t *g = &newGames[total];
            memset(g, 0, sizeof(base_game_info_t));
            // The title past ".POPS." is the display name AND the launch key (hddFindVcdByName and
            // the Favourites entry point both resolve by name), so it must stay unique: two region
            // variants like PP.SCUS-....POPS.Game / PP.SCPS-....POPS.Game would otherwise both be
            // "Game" and always launch the first partition. On a collision fall back to the full
            // partition label, which is unique by construction. Check every entry collected so far,
            // pooled-container VCDs included.
            const char *title = parts.names[p] + titleOfs;
            int nameTaken = 0;
            for (int i = 0; i < total; i++) {
                if (strcmp(newGames[i].name, title) == 0) {
                    nameTaken = 1;
                    break;
                }
            }
            snprintf(g->name, sizeof(g->name), "%s", nameTaken ? parts.names[p] : title);
            snprintf(g->startup, sizeof(g->startup), "%s", g->name); // keep VCD identity = name
            snprintf(g->extension, sizeof(g->extension), ".VCD");
            g->parts = 1;
            g->format = GAME_FORMAT_ISO;                                    // VCD flag gates launch
            snprintf(newParts[total], APA_IDMAX + 1, "%s", parts.names[p]); // case-preserved label
            total++;
            continue;
        }

        // A loose PP.*/__.* label without the strict ".POPS." marker (app/HDL-style partitions,
        // ID-less labels like PP.CASTLEVANIA) is not VCD content: skip with zero I/O. Only the
        // exact __.POPS[0-9]? pooled containers below are mounted. hddIsPopsPartitionGame excludes
        // those containers by construction, so what remains IS a container.
        if (hddIsPopsPartitionGame(parts.names[p]))
            continue;

        // __.POPS[0-9]? pooled container: mount and scan its root for *.VCD entries.
        if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDONLY) < 0) {
            scanIncomplete = 1;
            continue;
        }

        vcd_entry_t *vcds = NULL;
        int n = vcdScanDirRoot("pfs1:/", &vcds);
        fileXioUmount("pfs1:");
        if (n < 0) {
            free(vcds);
            scanIncomplete = 1;
            continue;
        }
        if (n == 0) {
            free(vcds);
            continue;
        }

        base_game_info_t *grownGames = realloc(newGames, (total + n) * sizeof(base_game_info_t));
        if (grownGames == NULL) {
            free(vcds);
            scanIncomplete = 1;
            break;
        }
        newGames = grownGames;
        char(*grownParts)[APA_IDMAX + 1] = realloc(newParts, (total + n) * sizeof(*newParts));
        if (grownParts == NULL) {
            free(vcds);
            scanIncomplete = 1;
            break;
        }
        newParts = grownParts;

        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (gVcdFirstDiscOnly && vcdIsHiddenDisc(vcds[i].name))
                continue;
            base_game_info_t *g = &newGames[total + kept];
            memset(g, 0, sizeof(base_game_info_t));
            snprintf(g->name, sizeof(g->name), "%s", vcds[i].name);
            snprintf(g->startup, sizeof(g->startup), "%s", vcds[i].name);
            snprintf(g->extension, sizeof(g->extension), ".VCD");
            g->parts = 1;
            g->format = GAME_FORMAT_ISO;
            snprintf(newParts[total + kept], APA_IDMAX + 1, "%s", parts.names[p]);
            kept++;
        }
        free(vcds);
        total += kept;
    }

    hddFreePopsPartitionList(&parts);

    // ---- second phase: EMBER --------------------------------------------------------------------
    // ONE list, BOTH cores -- the APA restatement of what ps1FillGameList does for every other
    // device. The rows land in the SAME arrays, so the PS1 view, Favourites resolution, art lookup
    // and the sort all keep working with no idea that two different emulators are involved. What
    // tells them apart is the row's extension, exactly as on USB and SMB.
    hdd_pops_list_t emberParts;
    int emberCount = hddGetEmberPartitionList(&emberParts);
    if (emberCount < 0) {
        // A failed walk is not an empty Ember library, and it must not blank the VCD rows already
        // collected above either. Mark the refresh incomplete and let the transaction below decide.
        LOG("HDD EMBER: APA partition enumeration failed (%d)\n", emberCount);
        scanIncomplete = 1;
    } else {
        for (int p = 0; p < emberParts.count; p++) {
            char mountSrc[64];
            snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", emberParts.names[p]);

            if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDONLY) < 0) {
                scanIncomplete = 1;
                continue;
            }

            // cueScanDir gates itself on open()ing the core, so an __.EMBER partition that does
            // not actually hold an install costs this mount plus one failed open and returns 0.
            cue_entry_t *cues = NULL;
            int n = cueScanDir("pfs1:/", &cues);
            fileXioUmount("pfs1:");
            if (n < 0) {
                free(cues);
                scanIncomplete = 1;
                continue;
            }
            if (n == 0) {
                free(cues);
                continue;
            }

            base_game_info_t *grownGames = realloc(newGames, (total + n) * sizeof(base_game_info_t));
            if (grownGames == NULL) {
                free(cues);
                scanIncomplete = 1;
                break;
            }
            newGames = grownGames;
            char(*grownParts)[APA_IDMAX + 1] = realloc(newParts, (total + n) * sizeof(*newParts));
            if (grownParts == NULL) {
                free(cues);
                scanIncomplete = 1;
                break;
            }
            newParts = grownParts;

            int kept = 0;
            for (int i = 0; i < n; i++) {
                // Names are resolved back to a partition by hddFindVcdByName, which matches on name
                // alone. An Ember folder that collides with a VCD title (or with an Ember folder on
                // another partition) would otherwise always launch whichever came first, so drop
                // the duplicate rather than list a row that launches someone else's game.
                int taken = 0;
                for (int j = 0; j < total + kept; j++) {
                    if (strcmp(newGames[j].name, cues[i].name) == 0) {
                        taken = 1;
                        break;
                    }
                }
                if (taken) {
                    LOG("HDD EMBER: '%s' on %s shadowed by an earlier row; skipped\n", cues[i].name, emberParts.names[p]);
                    continue;
                }

                base_game_info_t *g = &newGames[total + kept];
                memset(g, 0, sizeof(base_game_info_t));
                // IDENTITY IS THE FOLDER NAME, unchanged: it is Ember's launch argument, and art,
                // per-game config and Favourites all key off it. ART/<folder name>_COV.png resolves
                // through the ordinary hddGetImage path with no special case.
                snprintf(g->name, sizeof(g->name), "%s", cues[i].name);
                snprintf(g->startup, sizeof(g->startup), "%s", cues[i].name);
                snprintf(g->extension, sizeof(g->extension), "%s", CUE_ROW_EXTENSION);
                g->parts = 1;
                g->format = GAME_FORMAT_ISO; // harmless; the row's extension gates the launch path
                snprintf(newParts[total + kept], APA_IDMAX + 1, "%s", emberParts.names[p]);
                kept++;
            }
            free(cues);
            total += kept;
        }
        hddFreePopsPartitionList(&emberParts);
    }

    fileXioUmount("pfs1:");

    // If a refresh could not inspect every candidate, a non-empty last-good list has more authority
    // than a newly-built partial one. This is the VCD twin of the transactional HDL refresh above.
    if (scanIncomplete && hddVcdGameCount > 0) {
        free(newGames);
        free(newParts);
        hddVcdListBuilt = 0;
        LOG("HDD VCD: incomplete refresh; preserving %d last-good game(s)\n", hddVcdGameCount);
        return hddVcdGameCount;
    }

    // A first-ever incomplete scan may still expose the entries it proved readable, but it is never
    // latched as complete. A fully successful zero/nonzero scan is latched unless invalidated mid-run.
    hddPublishVcdGameList(newGames, newParts, total, !scanIncomplete && (genAtEntry == hddVcdCacheGen));
    return total;
}

static int hddNeedsUpdate(item_list_t *itemList)
{ /* Auto refresh is disabled by setting HDD_MODE_UPDATE_DELAY to MENU_UPD_DELAY_NOUPDATE, within hddsupport.h.
       Hence any update request would be issued by the user, which should be taken as an explicit request to re-scan the HDD. */
    if (libViewConsumeDirty(itemList->mode))
        return 1; // L3 toggle / default-view change -> rebuild the submenu (the ARRAY may be cached)
    if (libListViewActive(itemList) == LIB_VIEW_PS1)
        // The locked-to-VCD startup path also receives the normal initial deferred update, but no
        // toggle dirtied the view first. Build once when the VCD backing list does not exist yet.
        // A manual HDD/VCD refresh invalidates this latch before posting the same deferred update.
        return !hddVcdListBuilt;
    return 1;
}

static int hddUpdateGameList(item_list_t *itemList)
{
    int view = libListViewActive(itemList);
    // GUI thread must never block on the 1 s ATA settle (hddLoadModules DelayThread).
    // If the base ATA stack has never completed (hddModulesLoadCount==0), defer and
    // ensure a deduplicated IO-worker retry is queued. hddLoadModules resets the count
    // to 0 on failure for retryability, and the one-shot hddInit queue may already be
    // consumed, so without this the stack would strand permanently.
    if (hddModulesLoadCount == 0) {
        if (!hddRetryQueued) {
            if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &hddInitModules) == IO_OK)
                hddRetryQueued = 1;
        }
        return 0;
    }

    // Self-heal (wLaunchELF-R3Z3N parity: latch only on success, retry per use): a boot-time first
    // touch that raced drive spin-up used to leave the APA page EMPTY for the whole session -- the
    // one-shot hddInitModules never retried, and nothing else reloads the support stack. Both calls
    // are idempotent (hddLoadModules dedupes via ALREADYLOADED and its failed count is retryable
    // post-#241; hddLoadSupportModules no-ops once hddSupportModulesLoaded), so tab entry / refresh
    // becomes a real second attempt. Runs on the IO worker like the original init.
    if (!hddSupportModulesLoaded || gHDDPrefix == NULL) {
        // Only chase the support stack when the base modules are actually resident -- calling it
        // anyway made a failed base load toast TWICE (base failure + the doomed non-Sony probe's)
        // on the first pass (Gemini + CodeRabbit review of #249, vetted). A loaded module stack
        // with no data mount is also retryable: Step-209 deliberately separates those lifetimes.
        // WHETHER THE RETRY ACTUALLY RAN decides whether a failure is reportable. hddLoadModules
        // stamps hddModulesLoadCount=1 on ENTRY -- before sysInitDev9, before the ATAD load, and
        // before the 1 s settle, all of which run on the io worker. So the guard at the top of this
        // function stops firing almost immediately, while the base stack is still coming up, and
        // hddLoadModulesReady() correctly answers BUSYLOADING -> false for that whole window.
        //
        // Without this flag the code then fell through to the toast having attempted NOTHING, and
        // announced it as "settled-update-retry-still-failed" -- which is why Code 222 appeared on
        // consoles whose APA worked perfectly a moment later. A base stack that is still loading is
        // not evidence of anything; say nothing until it is actually resident.
        int baseReady = hddLoadModulesReady();
        if (baseReady)
            hddLoadSupportModules();
        // Deferred Code 222: early boot probes never toast; a settled retry that really ran may.
        if (hddSupportModulesLoaded) {
            hddClearPfsDiagFailure("update-retry-loaded-support");
            hddSupportErrToasted = 0;
            hddClearRecoveredErrors();
        } else if (baseReady && hddPfsDeferredFailed && !hddSupportErrToasted) {
            // ONE FAILED ATTEMPT IS NOT EVIDENCE OF A DEAD DRIVE. Gating on baseReady stopped us
            // reporting a retry that never ran, but it still reported the FIRST settled retry that
            // failed -- and hardware says PFS can decline several of those and then mount fine, so
            // the box appeared on consoles whose APA worked seconds later. The 1 s post-ATAD settle
            // is a floor, not a guarantee: a drive that is still spinning up answers the ATA stack
            // long before it will answer PS2FS.
            //
            // So require the failure to PERSIST across a few settled attempts. Each pass through
            // here is a genuine retry (hddLoadSupportModules ran and did not latch), and the success
            // arm above resets the count, so this only fires when PFS has really refused to come up.
            if (++hddPfsSettledFailures >= HDD_PFS_REPORT_AFTER_FAILURES) {
                hddLogPfsDiagState("emit-code-222", "settled-retries-exhausted");
                setErrorMessageWithCodeAndDetail(_STR_HDD_PFS_UNAVAILABLE_ERROR, ERROR_HDD_MODULE_PFS_FAILURE,
                                                 hddPfsDiagReasonName(hddPfsDiagReason));
                hddSupportErrToasted = 1;
            } else {
                hddLogPfsDiagState("defer-code-222", "settled-retry-failed-below-threshold");
            }
        }
    } else {
        // Successful path also clears any prior deferred failure.
        if (hddPfsDeferredFailed) {
            hddClearPfsDiagFailure("update-found-support-already-ready");
            hddSupportErrToasted = 0;
            hddClearRecoveredErrors();
        }
    }

    // Game discovery and the persistent config/art PFS home are separate lifetimes. HDL games
    // live in the APA table and HDD VCDs live in existing POPS partitions; neither requires the
    // long-lived pfs0: data home. Step 211 accidentally returned an empty page whenever that home
    // was unavailable even though the APA/PFS support modules were healthy. Require only the support
    // stack here; config/art accessors below remain NULL-safe and fail closed independently.
    // The combined support latch includes PS2FS/PFS readiness, but the PS2 list itself is an
    // APA-table read and must not disappear merely because the persistent PFS side is still settling.
    // Retry above, then let each read-only scanner prove what is actually available. HDL enumeration
    // fails harmlessly if hdd0: is not ready; the VCD builder likewise skips mounts it cannot open.
    if (!hddSupportModulesLoaded)
        LOG("HDDSUPPORT UpdateGameList: PFS support incomplete; attempting read-only APA enumeration anyway\n");

    if (view == LIB_VIEW_PS1 || view == LIB_VIEW_MIXED) {
        // Reuse the session's built list on view flips; hddBuildVcdGameList runs only when never
        // built, invalidated (first-disc-only change), or freed by teardown (hddFreeVcdGameList).
        if (!hddVcdListBuilt)
            hddBuildVcdGameList();
        if (view == LIB_VIEW_PS1)
            return hddVcdGameCount;
    }

    hdl_games_list_t cachedGames = {0};
    hdl_games_list_t hddGamesNew = {0};
    int cacheRet, scanRet = 0;

    // TRANSACTIONAL REFRESH. hddGames is the LIVE backing array used by the menu. The old path
    // passed it directly to hddLoadGameListCache(), whose first action is hddFreeHDLGamelist(): a
    // missing/corrupt games.bin therefore destroyed a perfectly good live list BEFORE the fallback
    // APA scan had proved it could replace it. If that scan failed, VCD -> HDL returned to a blank
    // PS2 page even though the previous HDL list had been valid. Build cache/live candidates off to
    // the side and publish only a successful replacement.
    cacheRet = hddLoadGameListCache(&cachedGames);

    // Force a live APA scan when the cache is absent/bad, a prior build requested a refresh, or the
    // cache is empty. Otherwise the initial boot may use the valid games.bin exactly as before.
    if (cacheRet == 0 && !hddForceUpdate && cachedGames.count > 0) {
        hddPublishHdlGameList(&cachedGames);
    } else {
        scanRet = hddGetHDLGamelist(&hddGamesNew);
        if (scanRet == 0) {
            hddUpdateGameListCache(&cachedGames, &hddGamesNew);
            hddPublishHdlGameList(&hddGamesNew);
        } else {
            // Keep the last-good live list. On a first entry with no live list yet, a valid cache is
            // still a safer fallback than turning a transient scan failure into an empty page.
            if (hddGames.count == 0 && cachedGames.count > 0)
                hddPublishHdlGameList(&cachedGames);
            LOG("HDDSUPPORT HDL refresh failed (%d); preserving %u last-good game(s)\n", scanRet, hddGames.count);
        }
    }

    hddFreeHDLGamelist(&cachedGames);
    hddFreeHDLGamelist(&hddGamesNew);
    hddForceUpdate = 1; // Subsequent refresh operations will cause the HDD to be scanned.

    return view == LIB_VIEW_MIXED ? (int)hddGames.count + hddVcdGameCount : (int)hddGames.count;
}

static int hddGetGameCount(item_list_t *itemList)
{
    int view = libListViewActive(itemList);
    return view == LIB_VIEW_MIXED ? (int)hddGames.count + hddVcdGameCount :
           view == LIB_VIEW_PS1   ? hddVcdGameCount :
                                    (int)hddGames.count;
}

static int hddGetItemView(item_list_t *itemList, int id)
{
    int view = libListViewActive(itemList);
    if (view == LIB_VIEW_MIXED)
        return id >= 0 && id < (int)hddGames.count ? LIB_VIEW_ISO : LIB_VIEW_PS1;
    return view;
}

static int hddGetSourceId(item_list_t *itemList, int id)
{
    return libListViewActive(itemList) == LIB_VIEW_MIXED && id >= (int)hddGames.count ? id - (int)hddGames.count : id;
}

// Split-store guard (Codex/Fable audit), mirroring mmceActiveGame. L3 stages its destination until
// the old submenu is cleared, but either backing array can still be NULL/shorter when never scanned,
// partially refreshed, or torn down. Resolve every id through these: an out-of-range id returns a
// static empty entry so a stale read is safe empty data and launch/delete/rename early-return on the
// sentinel. HDD needs TWO resolvers -- ISO is hdl_game_info_t, VCD is base_game_info_t.
static base_game_info_t hddEmptyVcd = {.extension = ".VCD"};
static hdl_game_info_t hddEmptyHdl = {0};
static base_game_info_t *hddActiveVcd(int id)
{
    if (hddVcdGames == NULL || id < 0 || id >= hddVcdGameCount)
        return &hddEmptyVcd;
    return &hddVcdGames[id];
}
static hdl_game_info_t *hddActiveHdl(int id)
{
    if (hddGames.games == NULL || id < 0 || id >= (int)hddGames.count)
        return &hddEmptyHdl;
    return &hddGames.games[id];
}

static void *hddGetGame(item_list_t *itemList, int id)
{
    int ps1 = hddGetItemView(itemList, id) == LIB_VIEW_PS1;
    id = hddGetSourceId(itemList, id);
    return ps1 ? (void *)hddActiveVcd(id) : (void *)hddActiveHdl(id);
}

static char *hddGetGameName(item_list_t *itemList, int id)
{
    int ps1 = hddGetItemView(itemList, id) == LIB_VIEW_PS1;
    id = hddGetSourceId(itemList, id);
    return ps1 ? hddActiveVcd(id)->name : hddActiveHdl(id)->name;
}

static int hddGetGameNameLength(item_list_t *itemList, int id)
{
    return hddGetItemView(itemList, id) == LIB_VIEW_PS1 ? VCD_NAME_MAX : (HDL_GAME_NAME_MAX + 1);
}

static char *hddGetGameStartup(item_list_t *itemList, int id)
{
    // VCD view keys per-game CFG/art off the VCD filename (game->name), not a disc id.
    int ps1 = hddGetItemView(itemList, id) == LIB_VIEW_PS1;
    id = hddGetSourceId(itemList, id);
    return ps1 ? hddActiveVcd(id)->name : hddActiveHdl(id)->startup;
}

static void hddDeleteGame(item_list_t *itemList, int id)
{
    if (hddGetItemView(itemList, id) == LIB_VIEW_PS1)
        return; // a VCD is not an HDL partition -- no delete in VCD view
    id = hddGetSourceId(itemList, id);
    hdl_game_info_t *game = hddActiveHdl(id);
    if (game == &hddEmptyHdl)
        return; // stale/invalid id -> don't delete a wrong/OOB HDL partition
    hddDeleteHDLGame(game);
    hddForceUpdate = 1;
}

// Settings probes use pfs1:, never pfs0:. The latter is the persistent OPL data-home mount and
// remounting it under live readers is both unsafe and unnecessary just to prove an APA partition
// already exists.
static int hddPartitionMountableAt(const char *mountPoint, const char *partition, int flags)
{
    int ret;

    fileXioUmount(mountPoint);
    ret = fileXioMount(mountPoint, partition, flags);
    if (ret == 0)
        fileXioUmount(mountPoint);

    return ret == 0;
}

int hddGetOplHomeSelection(void)
{
    if (hddOplHomePending >= 0)
        return hddOplHomePending;
    if (hddOplHomeCommitted >= 0)
        return hddOplHomeCommitted;
    return strcmp(gOPLPart, "hdd0:+OPL") == 0 ? HDD_OPL_HOME_PLUS : HDD_OPL_HOME_COMMON;
}

int hddOplHomeIsLegacy(void)
{
    if (hddOplHomePending >= 0 || hddOplHomeCommitted >= 0)
        return 0;
    return gOPLPart[0] != '\0' && strcmp(gOPLPart, "hdd0:__common") != 0 &&
           strcmp(gOPLPart, "hdd0:+OPL") != 0;
}

int hddOplHomeSelectionPending(void)
{
    return hddOplHomePending >= 0;
}

void hddDiscardOplHomeSelection(void)
{
    hddOplHomePending = -1;
}

// Why the last staging attempt ended the way it did. Five distinct conditions were all collapsed
// into two user-visible messages ("not found" / "busy"), which is why this row resisted several
// rounds of fixing: the message never named which one had fired. Surfaced in OPLDIAG builds only.
static const char *hddOplHomeStageReasonText = "none";

const char *hddOplHomeStageReason(void)
{
    return hddOplHomeStageReasonText;
}

int hddStageOplHomeSelection(int selection)
{
    const char *partition;

    hddOplHomeStageReasonText = "entered";

    if (selection != HDD_OPL_HOME_COMMON && selection != HDD_OPL_HOME_PLUS) {
        hddOplHomeStageReasonText = "invalid-selection";
        return 0;
    }

    // Use only the already-resident ATA stack. The selector is a short-lived proof, not an owner
    // of a module reference; hddLoadModulesReady() would retain one on every stage/save cycle.
    // Do not call hddLoadSupportModules here: its normal data-home recovery may mount pfs0: and
    // create OPL folders, neither of which belongs to a source selector proof.
    // "COULD NOT CHECK" IS NOT "DOES NOT EXIST". These two arms used to return 0, the same value the
    // failed mount below returns, and the caller renders 0 as "+OPL partition not found." -- so a
    // user with a perfectly good +OPL was told it was missing whenever the ATA stack or PS2FS was
    // not up yet. That is the SAME condition behind the deferred Code 222, which is why the two get
    // reported together. -1 is the caller's existing "cannot answer right now" state.
    // BRING THE STACK UP RATHER THAN REFUSING. This used to require the ATA modules to already be
    // resident, on the reasoning that a selector should not own a module reference. But nothing
    // guarantees they are resident when the user opens Settings, and when they are not, the check
    // can never pass -- so the row reported "not found" (before) or "busy" (after that was
    // corrected) on every single attempt, with no sequence of user actions able to fix it. A
    // setting that cannot be changed is a worse outcome than an extra hddModulesLoadCount bump.
    //
    // Affordable here specifically: this runs on the io worker behind guiHandleDeferedIO's spinner
    // with a 15 s budget, which is exactly the machinery for a load that may take a second.
    // hddLoadSupportModules() is still deliberately NOT called -- its data-home recovery mounts
    // pfs0: and creates OPL folders, and neither belongs to a source-selector proof.
    if (!hddModulesAreLoaded() && !hddLoadModulesReady()) {
        hddOplHomeStageReasonText = "ata-stack-unavailable";
        return -1; // cannot answer, not "absent"
    }
    if (!hddLoadCoreSupportModules()) {
        hddOplHomeStageReasonText = "pfs-support-unavailable";
        return -1;
    }

    partition = selection == HDD_OPL_HOME_PLUS ? "hdd0:+OPL" : "hdd0:__common";
    if (!hddPartitionMountableAt("pfs1:", partition, FIO_MT_RDONLY)) {
        hddOplHomeStageReasonText = "partition-mount-refused";
        return 0; // genuinely absent: the stack was up and the mount still refused
    }

    hddOplHomeStageReasonText = "ok";
    hddOplHomePending = selection;
    return 1;
}

int hddOplHomeSelectionNeedsTargetSave(void)
{
    int selection = hddGetOplHomeSelection();

    if (hddOplHomePending < 0 && hddOplHomeCommitted < 0)
        return 0;

    return selection == HDD_OPL_HOME_PLUS ? strcmp(gOPLPart, "hdd0:+OPL") != 0 : strcmp(gOPLPart, "hdd0:__common") != 0;
}

int hddMountSelectedOplHome(char *prefix, int prefixLen)
{
    const char *partition;
    const char *selectedPrefix;
    int selection;
    int n;

    if (prefix == NULL || prefixLen <= 0)
        return 0;
    prefix[0] = '\0';

    selection = hddGetOplHomeSelection();
    if (hddOplHomePending < 0 && hddOplHomeCommitted < 0)
        return 0;
    if (!hddModulesAreLoaded() || !hddLoadCoreSupportModules())
        return 0;

    partition = selection == HDD_OPL_HOME_PLUS ? "hdd0:+OPL" : "hdd0:__common";
    fileXioUmount("pfs1:");
    if (fileXioMount("pfs1:", partition, FIO_MT_RDWR) < 0)
        return 0;

    if (selection == HDD_OPL_HOME_COMMON) {
        if (!hddCheckOPLFolder("pfs1:")) {
            fileXioUmount("pfs1:");
            return 0;
        }
        selectedPrefix = "pfs1:OPL/";
    } else {
        selectedPrefix = "pfs1:";
    }

    n = snprintf(prefix, prefixLen, "%s", selectedPrefix);
    if (n < 0 || n >= prefixLen) {
        fileXioUmount("pfs1:");
        prefix[0] = '\0';
        return 0;
    }

    return 1;
}

void hddUnmountSelectedOplHome(void)
{
    fileXioUmount("pfs1:");
}

int hddCommitOplHomeSelection(void)
{
    config_set_t *config = NULL;
    const char *path = "pfs1:OPL/conf_hdd.cfg";
    int fd;
    int exists;
    int result = 0;

    if (hddOplHomePending < 0)
        return 1;

    // An automatic +OPL home has no __common control file. The active pfs0: mount proves the
    // staged effective choice, so commit it as a fileless no-op without probing or writing
    // __common. This preserves the no-__common topology across ordinary Settings saves.
    if (!hddModulesAreLoaded())
        return 0;
    if (!hddLoadCoreSupportModules())
        return 0;
    if (hddOplHomePending == HDD_OPL_HOME_PLUS && hddOplHomeAutoPlus &&
        !strcmp(gOPLPart, "hdd0:+OPL") && gHDDPrefix != NULL) {
        hddOplHomeCommitted = hddOplHomePending;
        hddOplHomePending = -1;
        return 1;
    }

    // Re-prove the common owner at commit time. No raw APA operation, partition creation, or pfs0:
    // remount is involved; this is only a bounded pfs1: read/write mount of an existing partition.
    if (!hddPartitionMountableAt("pfs1:", "hdd0:__common", FIO_MT_RDWR))
        return 0;

    if (fileXioMount("pfs1:", "hdd0:__common", FIO_MT_RDWR) < 0)
        return 0;

    fd = open(path, O_RDONLY);
    exists = fd >= 0;
    if (fd >= 0)
        close(fd);

    // A missing selector already means __common/OPL/. Keep that default fileless; only an
    // explicit +OPL selection needs to materialize conf_hdd.cfg.
    if (!exists && hddOplHomePending == HDD_OPL_HOME_COMMON)
        result = 1;
    else
        config = configAlloc(0, NULL, (char *)path);
    if (config != NULL) {
        // A present-but-unreadable file is user data, not an invitation to replace it with a new
        // selector. A missing file is the normal common-home default and can be born only for an
        // explicit +OPL choice.
        if (!exists || configRead(config)) {
            if (hddOplHomePending == HDD_OPL_HOME_PLUS)
                configSetStr(config, "hdd_partition", "+OPL");
            else
                configRemoveKey(config, "hdd_partition");

            result = configWrite(config) > 0;
        }
        configFree(config);
    }

    fileXioUmount("pfs1:");
    if (result) {
        hddOplHomeCommitted = hddOplHomePending;
        hddOplHomePending = -1;
    }
    return result;
}

// A PP.<DISC-ID>.POPS.<title> VCD is the APA/PFS partition itself, not a loose .VCD file. Preserve
// the strict POPSTARTER prefix and replace only its title component; the next VCD scan then resolves
// the new literal partition label for the handoff selector.
static int hddRenamePopsPartition(const char *part, const char *newName)
{
    char oldPath[APA_IDMAX + 6];
    char newPart[APA_IDMAX + 1];
    char newPath[APA_IDMAX + 6];
    int titleOfs;
    int n;

    if (part == NULL || newName == NULL || newName[0] == '\0' || strpbrk(newName, ":/\\") != NULL)
        return -1;
    titleOfs = vcdPopsPartitionTitleOffset(part);
    if (titleOfs <= 0)
        return -1;

    n = snprintf(newPart, sizeof(newPart), "%.*s%s", titleOfs, part, newName);
    if (n < 0 || n > APA_IDMAX)
        return -1; // an APA label cannot grow past its on-disk field
    snprintf(oldPath, sizeof(oldPath), "hdd0:%s", part);
    snprintf(newPath, sizeof(newPath), "hdd0:%s", newPart);
    return fileXioRename(oldPath, newPath);
}

static void hddRenameVcd(int id, const char *newName)
{
    char oldName[VCD_NAME_MAX];
    char part[APA_IDMAX + 1];
    int renamed = 0, isEmber = 0;

    // The list builder owns pfs1: and the backing arrays on the IO worker. Block it before copying
    // the selected storage identity and before temporarily mounting a pooled POPS partition RW.
    ioBlockOps(1);
    if (hddVcdGames != NULL && hddVcdParts != NULL && id >= 0 && id < hddVcdGameCount) {
        snprintf(oldName, sizeof(oldName), "%s", hddVcdGames[id].name);
        snprintf(part, sizeof(part), "%s", hddVcdParts[id]);
        isEmber = cueIsCueEntry(&hddVcdGames[id]);

        if (isEmber) {
            // An Ember title IS its folder, on the partition the scan recorded. Mount that partition
            // RW on the scan slot, exactly as the pooled-VCD branch below does for a loose file.
            char mountSrc[APA_IDMAX + 6];

            snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", part);
            fileXioUmount("pfs1:");
            if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDWR) == 0) {
                renamed = (cueRenameGame("pfs1:/", oldName, newName) == 0);
                fileXioUmount("pfs1:");
            }
        } else if (vcdPopsPartitionTitleOffset(part) > 0) {
            // One-game PP.* POPS installs boot from their partition label, so a file rename would
            // change nothing. Rename the APA record while retaining its required prefix instead.
            renamed = (hddRenamePopsPartition(part, newName) == 0);
        } else {
            char mountSrc[APA_IDMAX + 6];

            snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", part);
            fileXioUmount("pfs1:");
            if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDWR) == 0) {
                renamed = (vcdRenameFileInDir("pfs1:/", oldName, newName) == 0);
                fileXioUmount("pfs1:");
            }
        }
    }
    ioBlockOps(0);

    if (renamed) {
        // vcdRenameFileInDir already clears this for a loose file; doing it again keeps the
        // partition-label path identical and is harmless.
        vcdInvalidateGameIds();
        hddVcdInvalidateCache();
    }
}

static void hddRenameGame(item_list_t *itemList, int id, char *newName)
{
    if (hddGetItemView(itemList, id) == LIB_VIEW_PS1) {
        hddRenameVcd(hddGetSourceId(itemList, id), newName);
        return;
    }
    id = hddGetSourceId(itemList, id);
    hdl_game_info_t *game = hddActiveHdl(id);
    if (game == &hddEmptyHdl)
        return; // stale/invalid id -> don't rename a wrong/OOB HDL partition
    strcpy(game->name, newName);
    hddSetHDLGameInfo(game);
    hddForceUpdate = 1;
}

// Index of the VCD whose basename matches vcdName in the current hddVcdGames list, or -1.
static int hddFindVcdByName(const char *vcdName)
{
    for (int i = 0; i < hddVcdGameCount; i++) {
        if (strcmp(hddVcdGames[i].name, vcdName) == 0)
            return i;
    }
    return -1;
}

// Put pfs0: back on the OPL data partition after a launch attempt borrowed the slot. A list-only
// APA session may legitimately have no persistent data home, in which case there is nothing truthful
// to remount and an empty gOPLPart must never be passed to fileXioMount.
static void hddRestoreDataHome(void)
{
    fileXioUmount(hddPrefix);
    if (gOPLPart[0] != '\0' && fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR) < 0) {
        // Never leave a non-NULL prefix advertising an unmounted pfs0:.
        gHDDPrefix = NULL;
        gOPLPart[0] = '\0';
    }
}

// Resolve POPSTARTER.ELF for an HDD VCD launch from hdd0:__common/POPS/ only. APA VCD data can live
// in its normal __.POPS / PP.* partitions and OPL data can be redirected elsewhere, but loose
// POPS/POPSTARTER support payloads are always owned by __common. On success returns 1 with
// elfOut = "pfs0:/POPS/POPSTARTER.ELF"
// and LEAVES pfs0: mounted on that partition so the caller can load the ELF from it; on failure returns 0
// with pfs0: restored to the OPL data partition. The CALLER must quiesce the art + IO workers first --
// this remounts the single pfs0: slot, and umounting it under a live cover read is the HDD-freeze hazard.
static int hddResolveHddPopstarter(char *elfOut, int elfLen)
{
    fileXioUmount(hddPrefix);
    if (fileXioMount(hddPrefix, "hdd0:__common", FIO_MT_RDONLY) == 0) {
        (void)vcdInstallPopstarterMc("pfs0:/");
        int fd = open("pfs0:/POPS/POPSTARTER.ELF", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            snprintf(elfOut, elfLen, "pfs0:/POPS/POPSTARTER.ELF");
            return 1; // keep pfs0: on __common for the ELF load
        }
    }

    // Not found: restore the default OPL data-partition mount. The restore is part of the
    // persistent-home invariant -- never leave a non-NULL prefix advertising an unmounted pfs0:
    // after POPSTARTER discovery borrowed the slot.
    hddRestoreDataHome();
    return 0;
}

// Hand an APA/PFS Ember title off with pfs0: STILL MOUNTED on the partition that holds it.
//
// This is the one launch in OPL where the mount is not a means of finding an ELF but the thing the
// child actually runs on: Ember does not reset the IOP (sysLoadELFKeepIOP), so the ps2fs driver, the
// pfs0: mount and the descriptors under it are all inherited live, and every read Ember makes from
// here on goes through them. Both the core and the game data are on this one partition, which is why
// a single mount is enough where POPSTARTER needs a partition passed out of band.
//
// Three consequences, all load-bearing:
//   RDWR, not RDONLY. Ember writes memory cards and settings.txt through this mount. The POPSTARTER
//   resolver mounts read-only because its mount dies at the IOP reset moments later; this one has to
//   survive and stay writable.
//   UNMOUNT_EXCEPTION keeps hddCleanUp from unmounting pfs0:.
//   KEEPIOP_EXCEPTION keeps it from issuing PDIOC_CLOSEALL, which would drop every pfs descriptor in
//   the IOP -- harmless before an IOP reset, fatal before a handoff that does not reset.
static void hddDoLaunchEmber(item_list_t *itemList, const char *name, const char *part)
{
    char emberElf[256], biosPath[288], mountSrc[APA_IDMAX + 6];

    if (name == NULL || name[0] == '\0' || part == NULL || part[0] == '\0')
        return;

    // Refuse what Ember itself would refuse, while a dialog can still be drawn and before anything
    // has been torn down. Same order as every other device's Ember leg.
    if (!cueNameLaunchable(name)) {
        guiMsgBox(_l(_STR_EMBER_BAD_NAME), 0, NULL);
        return;
    }

    // There is exactly ONE pfs0: slot and the remount below is destructive to any HDD art read still
    // in flight, so quiesce first -- the same discipline, and the same reason, as the VCD leg.
    cacheAbortMmceImageLoadsTimed(HDD_ART_QUIESCE_MS);
    if (!cacheCancelPendingImageLoadsTimed(HDD_ART_QUIESCE_MS)) {
        LOG("HDD EMBER: art did not quiesce; refusing the pfs0: remount\n");
        guiMsgBox(_l(_STR_PLEASE_WAIT), 0, NULL);
        return;
    }
    ioBlockOps(1);

    snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", part);
    fileXioUmount(hddPrefix);
    if (fileXioMount(hddPrefix, mountSrc, FIO_MT_RDWR) < 0) {
        hddRestoreDataHome();
        ioBlockOps(0);
        guiMsgBox(_l(_STR_EMBER_NOT_FOUND), 0, NULL);
        return;
    }

    // Verify against the partition as it is mounted RIGHT NOW, not against what the scan saw: the
    // scan read this partition read-only through pfs1: and may be minutes old.
    if (!cueResolveEmber("pfs0:/", emberElf, sizeof(emberElf))) {
        hddRestoreDataHome();
        ioBlockOps(0);
        guiMsgBox(_l(_STR_EMBER_NOT_FOUND), 0, NULL);
        return;
    }
    if (!cueResolveEmberBios("pfs0:/", biosPath, sizeof(biosPath))) {
        hddRestoreDataHome();
        ioBlockOps(0);
        guiMsgBox(_l(_STR_EMBER_BIOS_MISSING), 0, NULL);
        return;
    }
    if (!cueGameHasImage("pfs0:/", name)) {
        hddRestoreDataHome();
        ioBlockOps(0);
        guiMsgBox(_l(_STR_EMBER_NO_DISC), 0, NULL);
        return;
    }
    cueApplyDisplaySetting("pfs0:/"); // best-effort marker, never a launch gate -- needs the RDWR mount

    // Past this point pfs0: stays where it is and IO stays blocked; deinit re-blocks anyway.
    deinit(UNMOUNT_EXCEPTION | KEEPIOP_EXCEPTION, itemList->mode);
    sysLaunchEmber(emberElf, name);
}

// Shared POPSTARTER handoff for an HDD VCD. argv[0] differs by partition shape: PP.<name> / __.<name>
// one-game installs boot by their literal partition label; a pooled __.POPS[0-9]? entry boots by its
// VCD name. POPSTARTER derives the HDD route from that selector, so it must remain target argv[0].
// Everything is built on stack before deinit() frees the VCD list.
static void hddDoLaunchVcd(item_list_t *itemList, const char *name, const char *part)
{
    char vcdElf[256], vcdSelector[320];

    if (name == NULL || name[0] == '\0' || part == NULL || part[0] == '\0')
        return;

    if (hddIsPopsPartitionGame(part))
        snprintf(vcdSelector, sizeof(vcdSelector), "%s.ELF", part); // literal PP.Game.ELF / __.Hidden.ELF
    else
        snprintf(vcdSelector, sizeof(vcdSelector), "%s.ELF", name); // GAME.ELF (pooled-container VCD)

    // Resolve + keep pfs0: on the POPSTARTER.ELF partition. Quiesce art+IO first (this remounts pfs0:).
    //
    // The budget used to be 0 on both, i.e. no wait at all -- harmless while these were `return 1`
    // stubs, and not harmless now that they are real. There is exactly ONE pfs0: slot, so the remount
    // below is destructive to any HDD art read still running; a zero budget quiesced nothing and just
    // reported success. Note cacheAbortMmce* only covers SIO2 requests, so the second call (which
    // covers ALL of them) is the one that matters here and its result is the one worth honouring.
    cacheAbortMmceImageLoadsTimed(HDD_ART_QUIESCE_MS);
    if (!cacheCancelPendingImageLoadsTimed(HDD_ART_QUIESCE_MS)) {
        LOG("HDD VCD: art did not quiesce; refusing the pfs0: remount\n");
        guiMsgBox(_l(_STR_PLEASE_WAIT), 0, NULL);
        return;
    }
    ioBlockOps(1);
    if (!hddResolveHddPopstarter(vcdElf, sizeof(vcdElf))) {
        ioBlockOps(0); // resolver already restored pfs0: to the OPL data partition on failure
        guiMsgBox(_l(_STR_POPSTARTER_NOT_FOUND), 0, NULL);
        return;
    }
    // Success: leave IO blocked (deinit re-blocks anyway) and pfs0: on the POPSTARTER partition for the
    // argv-preserving load below. POPSTARTER takes over and performs its own IOP reset.
    char vcdFullPath[256];
    snprintf(vcdFullPath, sizeof(vcdFullPath), "%s/%s.VCD", part, name);
    vcdPrepareRetroGemBarcode(vcdFullPath);
    deinit(UNMOUNT_EXCEPTION, itemList->mode);
    sysLaunchPopstarter(vcdElf, vcdSelector);
}

// Launch an HDD PS1/.VCD entry BY NAME -- the Favourites tab's view-independent entry point. The
// per-game __.POPS* partition lives in hddVcdParts, which is only populated while the HDD page is in
// its VCD view; from Favourites it may be empty/stale, so (re)scan via the SAME safe partition walk the
// VCD view uses (hddBuildVcdGameList mounts each __.POPS on the dedicated pfs1: scan slot) to resolve
// name -> partition, then hand off exactly as the in-view launch does.
static void hddLaunchVcd(item_list_t *itemList, const char *vcdName, config_set_t *configSet)
{
    char resolvedName[VCD_NAME_MAX];
    char resolvedPart[APA_IDMAX + 1];

    if (vcdName == NULL || vcdName[0] == '\0' || !strcasecmp(vcdName, "POPSTARTER")) // reserved-name belt: the scanner no longer lists it (#154); strcasecmp -- FAT is case-insensitive
        return;

    // Serialize against a queued HDD refresh that can use the same pfs1: slot and shared list. No art
    // cancellation is needed because the live pfs0: mount is not disturbed. Copy the result while the
    // worker remains blocked so a later refresh cannot invalidate the list storage passed to launch.
    ioBlockOps(1);
    int idx = hddFindVcdByName(vcdName);
    if (idx < 0) {
        // Cold path: the per-game __.POPS partition is unknown (hddVcdGames is only built while the HDD
        // page is in VCD view), so rebuild it through the dedicated pfs1: scan slot.
        hddBuildVcdGameList();
        idx = hddFindVcdByName(vcdName);
    }
    int isEmber = 0;
    if (idx >= 0) {
        snprintf(resolvedName, sizeof(resolvedName), "%s", hddVcdGames[idx].name);
        snprintf(resolvedPart, sizeof(resolvedPart), "%s", hddVcdParts[idx]);
        isEmber = cueIsCueEntry(&hddVcdGames[idx]); // kind read while the worker is still blocked
    }
    ioBlockOps(0); // resolvedName/resolvedPart are now independent of the mutable backing arrays
    if (idx < 0) {
        guiMsgBox(_l(_STR_POPSTARTER_NOT_FOUND), 0, NULL);
        return;
    }
    if (isEmber) {
        hddDoLaunchEmber(itemList, resolvedName, resolvedPart);
        return;
    }
    hddDoLaunchVcd(itemList, resolvedName, resolvedPart);
}

// Δ8 (NHDDL parity): the LEAN Neutrino launch path for HDL games -- see bdmTryNeutrinoLaunch's
// rationale in bdmsupport.c. The native flow's VMC block-chain prompts + mcemu patching, DMA
// setup, sbPrepare, cheats (with dialogs), PS2RD images and CheckPS2Logo all exist for the
// embedded cdvdman core; Neutrino re-derives everything from -bsd=ata -bsdfs=hdl -dvd=hdl:<part>
// after its own IOP reset. The one probe Neutrino DOES need is the ZSO header check (its hdl
// backend can't run ZSO) -- a single sector read, kept here.
// Returns 1 = handled (handed off; caller returns); 0 = proceed with the native launch (core is
// OPL, or any Neutrino-side failure -- ZSO, no install, preflight -- since HDL always boots natively).
static int hddTryNeutrinoLaunch(hdl_game_info_t *game, config_set_t *configSet)
{
    int coreLoader = gDefaultCoreLoader; // no per-game $CoreLoader key -> follow the global default core
    configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoader);
    if (!coreLoader)
        return 0;

    // ZSO probe (one sector): Neutrino's hdl backend can't run ZSO -- fall back to the native
    // core, which can. Same read the native path performs for its layer-1 setup.
    hddReadSectors(game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET, 1, IOBuffer);
    if (*(u32 *)IOBuffer == ZSO_MAGIC) {
        guiWarning(_l(_STR_NEUTRINO_BAD_FORMAT), 6);
        return 0;
    }

    const char *neutrinoPath = sbResolveNeutrinoPath(NULL); // #300: HDD keeps custom-path + mc0/mc1 + Device-picker resolution (raw APA isn't POSIX-reachable; pfs0 probe = shared-slot risk)
    if (neutrinoPath == NULL) {
        guiWarning(_l(_STR_NEUTRINO_NOT_FOUND), 6);
        return 0;
    }

    // Everything Neutrino needs, copied to THIS frame (deinit below frees `game`).
    int compatMode = 0, neutrinoVideo = gNeutrinoVideoDefault, neutrinoGsmComp = gNeutrinoGsmCompDefault; // absent per-game keys = follow the globals
    char neutrinoExtraArgs[256] = "";
    char apaPart[APA_IDMAX + 1];
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatMode);
    configGetStrCopy(configSet, CONFIG_ITEM_NEUTRINO_ARGS, neutrinoExtraArgs, sizeof(neutrinoExtraArgs));
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_VIDEO, &neutrinoVideo);
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_GSMCOMP, &neutrinoGsmComp);
    snprintf(apaPart, sizeof(apaPart), "%s", game->partition_name);

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    // Δ6 pre-teardown validation. On failure fall back to the native core (same contract as
    // bdmTryNeutrinoLaunch's non-udp legs): HDL always boots natively, and the native path owns
    // the autolaunch teardown -- aborting here instead would leak gAutoLaunchGame/configSet.
    if (sysNeutrinoPreflight("apa", neutrinoPath) < 0)
        return 0;

    // Honesty toast: the OPL core honors $VMC_N on HDD (mcemu over pfs0:VMC/), but Neutrino has no
    // APA/pfs backing store to open the .bin from post-reset -- its APA support is -bsd=ata
    // -bsdfs=hdl, game image only (NHDDL's HDL backend has the same no-VMC rule). No -mc args can
    // be emitted here; warn instead of silently booting without the card the user configured.
    // Runs pre-deinit so the toast still renders.
    {
        int slot;
        char vmcName[32];
        for (slot = 0; slot < NEUTRINO_VMC_SLOTS; slot++) {
            int slotDisabled = 0;
            vmcName[0] = '\0';
            configGetVMC(configSet, vmcName, sizeof(vmcName), slot);
            configGetVMCDisable(configSet, slot, &slotDisabled);
            if (vmcName[0] != '\0' && !slotDisabled) {
                LOG("[NEUTRINO] apa: VMC slot %d (%s) configured but unsupported -- launching without it\n", slot, vmcName);
                guiWarning(_l(_STR_NEUTRINO_VMC_HDD_UNSUPPORTED), 6);
                break;
            }
        }
    }

    // MMCE cross-device game-id (#261); HDD emits no -mc args (no APA/pfs backing store, above), mask 0.
    mmceSendGameID(game->startup, neutrinoPath, 0);

    // game->startup lives in hddGames / gAutoLaunchGame, freed below (deinitEx's itemCleanUp or the
    // explicit free). Copy it before the teardown so sysLaunchNeutrino's -elf build is not a UAF read.
    char apaStartup[sizeof(game->startup)];
    snprintf(apaStartup, sizeof(apaStartup), "%s", game->startup);

    if (gAutoLaunchGame == NULL) {
        // Keep-IOP handoff: keep the HDD stack up (NHDDL hands off with its full ATA stack
        // resident) AND the neutrino.elf device (-cwd config/module reads).
        int neutrinoDevMode = oplPath2Mode(neutrinoPath);
        deinitEx(UNMOUNT_EXCEPTION, HDD_MODE, neutrinoDevMode); // CAREFUL: itemCleanUp frees hddGames/game
    } else {
        miniDeinit(configSet);
        free(gAutoLaunchGame);
        gAutoLaunchGame = NULL;
        fileXioUmount("pfs0:");
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);
    }

    LOG("[NEUTRINO] apa partition_name=[%s]\n", apaPart);
    // gPS2Logo passes the preference straight through (Neutrino does its own logo work).
    sysLaunchNeutrino("apa", apaPart, apaStartup, compatMode, gPS2Logo, neutrinoPath, neutrinoExtraArgs, neutrinoVideo, neutrinoGsmComp, 0 /* #11 inert: APA is always -bsdfs=hdl */, NULL /* HDD VMC->neutrino deferred (APA/pfs) */);
    return 1;
}

void hddLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, size_irx = 0;
    int EnablePS2Logo = 0;
    int result;
    void *irx = NULL;
    char filename[32];
    hdl_game_info_t *game;
    struct cdvdman_settings_hdd *settings;

    // HDD VCD view: hand off to POPSTARTER instead of the HDL/Neutrino path below. Menu-launch only
    // (HDD-VCD autolaunch is out of scope). POPSTARTER's argv[0] is just the VCD name -- that name is
    // all it needs to find <name>.VCD; the __.POPS* partition the VCD lives on is passed OUT OF BAND so
    // POPSTARTER self-mounts it after the loader's IOP reset. HW-VALIDATE: POPSTARTER's exact HDD
    // selector/partition contract is hardware-testable -- POPSLoader proved the shape with a vendored
    // loader; we use the stock ps2sdk loader, the same one the shipping USB/MMCE/SMB VCD launch uses.
    if (gAutoLaunchGame == NULL && (hddGetItemView(itemList, id) == LIB_VIEW_PS1)) {
        char vcdName[VCD_NAME_MAX];
        char vcdPart[APA_IDMAX + 1];

        id = hddGetSourceId(itemList, id);

        guiLock();
        base_game_info_t *vcd = hddActiveVcd(id);
        if (vcd == &hddEmptyVcd) {
            guiUnlock();
            return; // stale/invalid id -> nothing to launch
        }
        snprintf(vcdName, sizeof(vcdName), "%s", vcd->name);
        snprintf(vcdPart, sizeof(vcdPart), "%s", hddVcdParts[id]);
        int isEmber = cueIsCueEntry(vcd); // read the row's kind while the list is still locked
        guiUnlock();

        if (isEmber)
            hddDoLaunchEmber(itemList, vcdName, vcdPart);
        else
            hddDoLaunchVcd(itemList, vcdName, vcdPart);
        return;
    }

    if (gAutoLaunchGame == NULL) {
        game = hddActiveHdl(id);
        if (game == &hddEmptyHdl)
            return; // stale/invalid id -> nothing to launch
    } else
        game = gAutoLaunchGame;

    // D8: Neutrino core gets its own lean path FIRST -- everything below is native-core prep it
    // neither needs nor should be able to die on (see hddTryNeutrinoLaunch).
    if (hddTryNeutrinoLaunch(game, configSet))
        return;

    apa_sub_t parts[APA_MAXSUB + 1];
    char vmc_name[2][32];
    int part_valid = 0, size_mcemu_irx = 0, nparts;
    hdd_vmc_infos_t hdd_vmc_infos;
    memset(&hdd_vmc_infos, 0, sizeof(hdd_vmc_infos_t));

    configGetVMC(configSet, vmc_name[0], sizeof(vmc_name[0]), 0);
    configGetVMC(configSet, vmc_name[1], sizeof(vmc_name[1]), 1);

    if (vmc_name[0][0] || vmc_name[1][0]) {
        nparts = hddGetPartitionInfo(gOPLPart, parts);
        if (nparts > 0 && nparts <= 5) {
            for (i = 0; i < nparts; i++) {
                hdd_vmc_infos.parts[i].start = parts[i].start;
                hdd_vmc_infos.parts[i].length = parts[i].length;
                LOG("HDDSUPPORT hdd_vmc_infos.parts[%d].start : 0x%X\n", i, hdd_vmc_infos.parts[i].start);
                LOG("HDDSUPPORT hdd_vmc_infos.parts[%d].length : 0x%X\n", i, hdd_vmc_infos.parts[i].length);
            }
            part_valid = 1;
        }
    }

    if (part_valid) {
        char vmc_path[256];
        int vmc_id, have_error = 0;
        vmc_superblock_t vmc_superblock;
        pfs_blockinfo_t blocks[11];

        for (vmc_id = 0; vmc_id < 2; vmc_id++) {
            if (vmc_name[vmc_id][0]) {
                have_error = 1;
                hdd_vmc_infos.active = 0;
                if (sysCheckVMC(gHDDPrefix, "/", vmc_name[vmc_id], 0, &vmc_superblock) > 0) {
                    hdd_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                    hdd_vmc_infos.flags |= 0x100;
                    hdd_vmc_infos.specs.page_size = vmc_superblock.page_size;
                    hdd_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                    hdd_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                    // Check vmc inode block chain (write operation can cause damage)
                    snprintf(vmc_path, sizeof(vmc_path), "%sVMC/%s.bin", gHDDPrefix, vmc_name[vmc_id]);
                    if ((nparts = hddGetFileBlockInfo(vmc_path, parts, blocks, 11)) > 0) {
                        have_error = 0;
                        hdd_vmc_infos.active = 1;
                        for (i = 0; i < nparts - 1; i++) {
                            hdd_vmc_infos.blocks[i].number = blocks[i + 1].number;
                            hdd_vmc_infos.blocks[i].subpart = blocks[i + 1].subpart;
                            hdd_vmc_infos.blocks[i].count = blocks[i + 1].count;
                            LOG("HDDSUPPORT hdd_vmc_infos.blocks[%d].number     : 0x%X\n", i, hdd_vmc_infos.blocks[i].number);
                            LOG("HDDSUPPORT hdd_vmc_infos.blocks[%d].subpart    : 0x%X\n", i, hdd_vmc_infos.blocks[i].subpart);
                            LOG("HDDSUPPORT hdd_vmc_infos.blocks[%d].count      : 0x%X\n", i, hdd_vmc_infos.blocks[i].count);
                        }
                    } else { // else VMC file is too fragmented
                        LOG("HDDSUPPORT Block Chain NG\n");
                        have_error = 2;
                    }
                }

                if (have_error) {
                    if (gAutoLaunchGame == NULL) {
                        char error[256];
                        if (have_error == 2) // VMC file is fragmented
                            snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTED_CONTINUE), vmc_name[vmc_id], (vmc_id + 1));
                        else
                            snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name[vmc_id], (vmc_id + 1));
                        if (!guiMsgBox(error, 1, NULL))
                            return;
                    } else
                        LOG("VMC error\n");
                }

                for (i = 0; i < size_hdd_mcemu_irx; i++) {
                    if (((u32 *)&hdd_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                        if (hdd_vmc_infos.active)
                            size_mcemu_irx = size_hdd_mcemu_irx;
                        memcpy(&((u32 *)&hdd_mcemu_irx)[i], &hdd_vmc_infos, sizeof(hdd_vmc_infos_t));
                        break;
                    }
                }
            }
        }
    }

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    char gid[5];
    configGetDiscIDBinary(configSet, gid);

    int dmaType = 0, dmaMode = 7, compatMode = 0;
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatMode);
    configGetInt(configSet, CONFIG_ITEM_DMA, &dmaMode);
    if (dmaMode < 3)
        dmaType = 0x20;
    else {
        dmaType = 0x40;
        dmaMode -= 3;
    }
    hddSetTransferMode(dmaType, dmaMode);
    // gHDDSpindown [0..20] -> spindown [0..240] -> seconds [0..1200]
    hddSetIdleTimeout(gHDDSpindown * 12);

    if (hddHDProKitDetected) {
        size_irx = size_hdd_hdpro_cdvdman_irx;
        irx = &hdd_hdpro_cdvdman_irx;
    } else {
        size_irx = size_hdd_cdvdman_irx;
        irx = &hdd_cdvdman_irx;
    }

    sbPrepare(NULL, configSet, size_irx, irx, &i);

    if (gHDDPrefix != NULL) {
#ifdef RETROACHIEVEMENTS
        // RA: this game's watch list, settled before sysLaunchLoaderElf reads
        // GetWatchCount() to decide whether the network modules travel with the
        // launch. Absent is normal -- the game is simply not tracked.
        sbLoadWatchList(gHDDPrefix, game->startup);
#endif
        if ((result = sbLoadCheats(gHDDPrefix, game->startup)) < 0) {
            // #265: let the user back out instead of sitting through the whole load. The helper does
            // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.
            // `settings` is not assigned until below, so derive the common block from the IRX base.
            if (!sbCheatsMissingContinue((u8 *)irx + i, result))
                return;
        }
        sbLoadImage(gHDDPrefix, game->startup);
    } else {
        LOG("HDDSUPPORT launch: no persistent PFS data home; skipping HDD cheats/image sidecars\n");
    }

    settings = (struct cdvdman_settings_hdd *)((u8 *)irx + i);

    // patch 48bit flag
    settings->common.media = hddIs48bit() & 0xff;

    // patch start_sector
    settings->lba_start = game->start_sector;

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    if (gPS2Logo)
        EnablePS2Logo = CheckPS2Logo(0, game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET);

    // Check for ZSO to correctly adjust layer1 start
    settings->common.layer1_start = 0; // cdvdman will read it from APA header
    hddReadSectors(game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET, 1, IOBuffer);
    if (*(u32 *)IOBuffer == ZSO_MAGIC) {
        probed_fd = 0;
        probed_lba = game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET;
        ziso_init((ZISO_header *)IOBuffer, *(u32 *)((u8 *)IOBuffer + sizeof(ZISO_header)));
        ziso_read_sector(IOBuffer, 16, 1);
        u32 maxLBA = *(u32 *)(IOBuffer + 80);
        if (maxLBA > 0 && maxLBA < ziso_total_block) {   // dual layer check
            settings->common.layer1_start = maxLBA - 16; // adjust second layer start
        }
    }

    // D8: Neutrino never reaches this point (hddTryNeutrinoLaunch handled it at the top) --
    // everything from here on is the NATIVE (embedded cdvdman) launch only.

    // MMCE cross-device game-id (#261): push the HDL disc id to a present MMCE card before the HDD
    // teardown (self-probes mmce0/mmce1; no-ops if no card / feature off). Read `game` before deinit.
    mmceSendGameID(game->startup, NULL, 0);

    if (gAutoLaunchGame == NULL) {
        deinit(NO_EXCEPTION, HDD_MODE); // CAREFUL: deinit will call hddCleanUp, so hddGames/game will be freed
    } else {
        miniDeinit(configSet);

        free(gAutoLaunchGame);
        gAutoLaunchGame = NULL;

        fileXioUmount("pfs0:");
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);
    }

    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_ATAD;

    // adjust ZSO cache
    settings->common.zso_cache = hddCacheSize;

    sysLaunchLoaderElf(filename, "HDD_MODE", size_irx, irx, size_mcemu_irx, hdd_mcemu_irx, EnablePS2Logo, compatMode);
}

static config_set_t *hddGetConfig(item_list_t *itemList, int id)
{
    char path[256];
    char vcdId[VCD_ID_MAX];
    config_set_t *config;

    // VCD (PS1) view: `id` indexes hddVcdGames, NOT hddGames. The list itself is discoverable from
    // APA even when the persistent config/art PFS home is temporarily unavailable. In that state
    // return a runtime-only config instead of dereferencing a NULL prefix or hiding the whole list.
    if (hddGetItemView(itemList, id) == LIB_VIEW_PS1) {
        base_game_info_t *g = hddActiveVcd(hddGetSourceId(itemList, id));
        if (gHDDPrefix != NULL)
            return sbPopulateConfig(g, gHDDPrefix, "/");

        config = configAlloc(0, NULL, NULL);
        if (config == NULL)
            return NULL;
        configSetStr(config, CONFIG_ITEM_NAME, g->name);
        configSetInt(config, CONFIG_ITEM_SIZE, 0);
        configSetStr(config, CONFIG_ITEM_FORMAT, "VCD");
        sbSetDiscAttributes(config, 1, 1);
        configSetStr(config, CONFIG_ITEM_STARTUP,
                     vcdExtractGameId(g->name, vcdId, sizeof(vcdId)) ? vcdId : g->name);
        return config;
    }

    hdl_game_info_t *game = hddActiveHdl(hddGetSourceId(itemList, id));

    if (gHDDPrefix != NULL) {
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, game->startup);
        config = configAlloc(0, NULL, path);
        if (config != NULL)
            configRead(config); // Does not matter if the config file exists or not.
    } else {
        config = configAlloc(0, NULL, NULL); // metadata only; no unsafe/imaginary save destination
    }
    if (config == NULL)
        return NULL;

    configSetStr(config, CONFIG_ITEM_NAME, game->name);
    configSetInt(config, CONFIG_ITEM_SIZE, game->total_size_in_kb >> 10);
    configSetStr(config, CONFIG_ITEM_FORMAT, "HDL");
    sbSetDiscAttributes(config, 0, game->disctype == SCECdPS2CD);
    configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);

    return config;
}

static int hddGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    // The APA/HDL list is allowed to exist without a persistent PFS data home, but relative ART is
    // not. Park the lookup cheaply instead of formatting a path through NULL; a later successful
    // data-home mount invalidates the miss generation above and re-arms artwork. Absolute theme
    // image requests remain independent and are still allowed.
    if (isRelative && gHDDPrefix == NULL)
        return ERR_BAD_FILE;

    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", gHDDPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    return texDiscoverLoad(resultTex, path, -1);
}

static int hddGetTextId(item_list_t *itemList)
{
    return _STR_HDD_GAMES;
}

static int hddGetIconId(item_list_t *itemList)
{
    return HDD_ICON;
}

// This may be called, even if hddInit() was not.
static void hddCleanUp(item_list_t *itemList, int exception)
{
    LOG("HDDSUPPORT CleanUp\n");

    // A THREAD IS STILL READING. cacheEnd() could not join the art worker before this teardown began,
    // so it is parked inside a device read right now. Unmounting pfs under it -- and above all
    // PDIOC_CLOSEALL, which closes EVERY pfs descriptor in the process, the BGM decoder's included --
    // is how a clean exit becomes a black-screen hang: the next read returns permanent EOF and the
    // thread that owns it never comes back, so whoever waits for it waits forever.
    //
    // Skipping both is safe either way. NOT because the handoff resets the IOP -- the keep-IOP
    // paths inherit whatever we leave behind -- but because unmounting under a live reader never is.
    if (gArtAbandoned) {
        LOG("HDDSUPPORT CleanUp: art worker abandoned mid-read -- leaving pfs mounted\n");
        if (hddGameList.enabled) {
            hddFreeHDLGamelist(&hddGames);
            hddFreeVcdGameList();
        }
        return;
    }

    if (hddGameList.enabled) {
        hddFreeHDLGamelist(&hddGames);
        hddFreeVcdGameList();
        fileXioUmount("pfs1:");

        if ((exception & UNMOUNT_EXCEPTION) == 0)
            fileXioUmount(hddPrefix);
    }

    // UI may have loaded modules outside of HDD mode, so deinitialize regardless of the enabled status.
    if (hddSupportModulesLoaded) {
        // PDIOC_CLOSEALL closes EVERY pfs descriptor in the IOP. That is free when the next thing to
        // run resets the IOP and reclaims them anyway -- the assumption stated above, and the one
        // every launch made until Ember. An Ember handoff keeps the IOP precisely so the child
        // inherits this pfs0: mount and reads its game through it; closing the descriptors out from
        // under it would leave Ember holding a mount it can no longer open anything on.
        if ((exception & KEEPIOP_EXCEPTION) == 0)
            fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);

        hddSupportModulesLoaded = 0;
        gHDDPrefix = NULL; // pfs0: is no longer a valid persistent data-home mount marker
    }
}

static int hddCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    return gHDDPrefix != NULL ? sysCheckVMC(gHDDPrefix, "/", name, createSize, NULL) : -1;
}

// This may be called, even if hddInit() was not.
static void hddShutdown(item_list_t *itemList)
{
    LOG("HDDSUPPORT Shutdown\n");

    if (hddGameList.enabled) {
        hddFreeHDLGamelist(&hddGames);
        hddFreeVcdGameList();
        fileXioUmount("pfs1:");
        fileXioUmount(hddPrefix);
    }

    // UI may have loaded modules outside of HDD mode, so deinitialize regardless of the enabled status.
    if (hddSupportModulesLoaded) {
        /* Close all files */
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);

        hddSupportModulesLoaded = 0;
        gHDDPrefix = NULL; // pfs0: is no longer a valid persistent data-home mount marker
    }

    if (hddModulesLoadCount > 0) {
        hddModulesLoadCount -= 1;
        if (hddModulesLoadCount == 0) {
            // DEV9 will remain active if ETH is in use, so put the HDD in IDLE state.
            // The HDD should still enter standby state after 21 minutes & 15 seconds, as per the ATAD defaults.
            hddSetIdleImmediate();
        }

        // Only shut down dev9 from here, if it was initialized from here before -- and only on a
        // TERMINAL teardown (exit/poweroff). On the launch path this shutdown runs for every
        // non-selected page, and powering DEV9 off here kills the ATA bus BEFORE bdmLaunchVcd's
        // post-deinit POPSTARTER.ELF read from the ATA-backed massN: mount -- the elf-loader then
        // returns into deinit'd OPL: the 4236edf6-class black-screen freeze (PCSX2 masks it; its
        // emulated DEV9 power-off is inert). ee_core/POPSTARTER reset the IOP right after, so the
        // launch path needs no power-off. Note the refcount asymmetry this also softens: N
        // hddLoadModules calls take ONE dev9 reference, but every hddShutdown used to drop it.
        if (gDeinitTerminal)
            sysShutdownDev9();
    }
}

static int hddLoadGameListCache(hdl_games_list_t *cache)
{
    char filename[256];
    FILE *file;
    hdl_game_info_t *games;
    int result, size, count;

    if (!gHDDGameListCache || gHDDPrefix == NULL)
        return 1; // cache is optional PFS data; live APA scanning does not depend on it

    hddFreeHDLGamelist(cache);

    sprintf(filename, "%sgames.bin", gHDDPrefix);
    file = fopen(filename, "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);

        count = size / sizeof(hdl_game_info_t);
        if (count > 0) {
            games = memalign(64, count * sizeof(hdl_game_info_t));
            if (games != NULL) {
                if (fread(games, sizeof(hdl_game_info_t), count, file) == count) {
                    cache->count = count;
                    cache->games = games;
                    LOG("hddLoadGameListCache: %d games loaded.\n", count);
                    result = 0;
                } else {
                    LOG("hddLoadGameListCache: I/O error.\n");
                    free(games);
                    result = EIO;
                }
            } else {
                LOG("hddLoadGameListCache: failed to allocate memory.\n");
                result = ENOMEM;
            }
        } else {
            result = -1; // Empty file
        }

        fclose(file);
    } else {
        result = ENOENT;
    }

    return result;
}

static int hddUpdateGameListCache(hdl_games_list_t *cache, hdl_games_list_t *game_list)
{
    char filename[256];
    FILE *file;
    int result, i, j, modified;

    if (!gHDDGameListCache || gHDDPrefix == NULL)
        return 1; // no persistent PFS home: keep the live list in RAM and skip games.bin writes

    if (cache->count > 0) {
        modified = 0;
        for (i = 0; i < cache->count; i++) {
            for (j = 0; j < game_list->count; j++) {
                if (strncmp(cache->games[i].partition_name, game_list->games[j].partition_name, APA_IDMAX + 1) == 0)
                    break;
            }

            if (j == game_list->count) {
                LOG("hddUpdateGameListCache: game added.\n");
                modified = 1;
                break;
            }
        }

        if ((!modified) && (game_list->count != cache->count)) {
            LOG("hddUpdateGameListCache: game removed.\n");
            modified = 1;
        }
    } else {
        modified = (game_list->count > 0) ? 1 : 0;
    }

    if (!modified)
        return 0;
    LOG("hddUpdateGameListCache: caching new game list.\n");

    sprintf(filename, "%sgames.bin", gHDDPrefix);
    if (game_list->count > 0) {
        file = fopen(filename, "wb");
        if (file != NULL) {
            result = (fwrite(game_list->games, sizeof(hdl_game_info_t), game_list->count, file) == game_list->count) ? 0 : EIO;
            fclose(file);
        } else {
            result = EIO;
        }
    } else {
        // Last game deleted.
        remove(filename);
        result = 0;
    }

    return result;
}

int hddIsPresent()
{
    // the only thing that currently uses ata_device_identify is ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, so this is the best method to check for presence via xhdd (for now anyways)
    // ideally, we'd only have ata_device_identify
    return fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, NULL, 0, NULL, 0) >= 0;
}

static char *hddGetPrefix(item_list_t *itemList)
{
    return gHDDPrefix;
}

int hddGetArtArchivePath(item_list_t *itemList, char *out, int outSize)
{
    (void)itemList;
    if (out == NULL || outSize <= 0)
        return -1;
    if (gHDDPrefix == NULL)
        return -1;
    int n = snprintf(out, outSize, "%sART/art.tar", gHDDPrefix);
    return (n > 0 && n < outSize) ? 1 : -1;
}

static item_list_t hddGameList = {
    HDD_MODE, 0, 0, MODE_FLAG_COMPAT_DMA, MENU_MIN_INACTIVE_FRAMES, HDD_MODE_UPDATE_DELAY, NULL, NULL, &hddGetTextId, &hddGetPrefix, &hddInit, &hddNeedsUpdate, &hddUpdateGameList,
    &hddGetGameCount, &hddGetGame, &hddGetGameName, &hddGetGameNameLength, &hddGetGameStartup, &hddDeleteGame, &hddRenameGame,
    &hddLaunchGame, &hddGetConfig, &hddGetImage, &hddCleanUp, &hddShutdown, &hddCheckVMC, &hddGetIconId, &hddLaunchVcd, 0, &hddGetArtArchivePath,
    &hddLaunchVcd, &hddGetItemView, &hddGetSourceId};
