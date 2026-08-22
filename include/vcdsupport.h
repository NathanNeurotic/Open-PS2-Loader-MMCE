/*
  Copyright 2024, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  VCD (PS1-via-POPSTARTER) support. Scans a device's POPS/ folder for *.VCD images and resolves
  the per-device POPSTARTER.ELF + the boot selector. RiptOPL surfaces these as a per-device
  "VCD view" (no separate page; no "PS1"/"POPS" wording in our UI). POPSTARTER.ELF is the only
  externally-named tool. Launch + the BDMA-module equip live elsewhere (system.c / opl.c).
*/

#ifndef __VCDSUPPORT_H
#define __VCDSUPPORT_H

#include "include/iosupport.h"
#include "include/supportbase.h" // base_game_info_t (for vcdFillGameList)

#define VCD_NAME_MAX  256  // VCD basename without ".VCD" (incl NUL); becomes the selector game name
#define VCD_ID_MAX    16   // optional extracted PS1 disc ID, e.g. "SCUS_123.45"
#define VCD_MAX_ITEMS 2048 // hard cap on VCDs scanned from one folder

// POPStarter access-type prefix, prepended to the selector .ELF token per device class.
#define VCD_PREFIX_MASS "XX." // USB / MX4SIO / iLink (local block devices)
#define VCD_PREFIX_SMB  "SB." // SMB / ETH (network)
#define VCD_PREFIX_HDD  ""    // HDD / pfs (deferred)

typedef struct
{
    char name[VCD_NAME_MAX]; // VCD basename WITHOUT ".VCD" (the POPSTARTER selector game name)
} vcd_entry_t;

// Scan "<devPrefix>POPS/" for *.VCD (case-insensitive). Returns the count; *outList is a malloc'd
// vcd_entry_t array the caller frees (NULL/0 on none/error). POSIX dir IO only (newlib-port rule).
int vcdScanDir(const char *devPrefix, vcd_entry_t **outList);

// Like vcdScanDir but scans `dirPath` DIRECTLY (no POPS/ subfolder) -- for the APA/PFS HDD where each
// __.POPS* partition holds its .VCD at the mounted root (e.g. dirPath = "pfs1:/").
int vcdScanDirRoot(const char *dirPath, vcd_entry_t **outList);

// Extract a strict leading PS1 ID from "SXXX_NNN.NN.Title" or "XX.SXXX_NNN.NN.Title" for same-folder CFG/art fallback.
// Returns 1 and writes the 11-character ID on success; otherwise returns 0 and writes an empty string.
int vcdExtractGameId(const char *name, char *idOut, int idSize);

// Strict one-game POPS HDD partition label PP.<DISC-ID>.POPS.<NAME>: returns the offset of <NAME>
// inside the label, or 0 when the label is not a strict PP-POPS partition. Pure string parsing.
int vcdPopsPartitionTitleOffset(const char *label);

// Remember which directory a VCD was scanned from. String work only -- NO device access; the scan
// must never block on IO. Called per entry by the scan.
void vcdNoteScanDir(const char *name, const char *dirPath);

// DISPLAY-ONLY PS1 disc id, resolved lazily from the image only when the active theme family
// contains the ItemText element that consumes it. Returns 1 and writes the
// id, or 0 when there is none -- the caller then shows the filename, as before. NOT identity: art,
// CFG and the launch all key off the VCD filename.
int vcdResolveDisplayId(const char *name, char *idOut, int idSize);

// Render-thread half of the caption id (#380): returns the ALREADY-resolved id from the session
// memo, 0 if not yet resolved. Never touches the device, never queues -- safe to call every frame.
int vcdDisplayIdCached(const char *name, char *idOut, int idSize);

// Ask for a settled VCD row's id to be resolved off-thread (one queued resolve per row per
// session, memo-deduped, only under explicit ItemText demand). Called per frame from the
// menu render path; a cold memo with a request already in flight is a no-op.
void vcdRequestDisplayId(const char *name);

// Forget every remembered id and scan directory, so swapped media is not described by the previous
// disc. Cheap: ids re-resolve lazily as rows are settled on.
void vcdInvalidateGameIds(void);

// Build "<devPrefix>POPS/POPSTARTER.ELF" into out; returns 1 if that file exists, else 0.
int vcdResolvePopstarter(const char *devPrefix, char *out, int outSize);

// Build the POPSTARTER argv[0] selector "<devPrefix>POPS/<prefix><name>.ELF" into out.
void vcdBuildSelector(const char *devPrefix, const char *prefix, const char *name, char *out, int outSize);

// VCD (PS1) cover FALLBACK to the POPSLoader-style "<scanPrefix>POPS/<value>.png" (suffixless, next to
// the .VCD). Cover/icon suffixes only; a device getImage calls this ONLY after its own <dev>ART/<name>_
// <suffix>.png misses, and only in the VCD view. `scanPrefix` = the prefix passed to vcdFillGameList.
// Returns texDiscoverLoad's result (>= 0 hit, negative miss).
int vcdLoadPopsCover(const char *scanPrefix, const char *value, const char *suffix, GSTEXTURE *resultTex);

// ---- per-device VCD view (L3 toggle) ----------------------------------------------
// Does this device class get a VCD view? (BDM range, MMCE, ETH, and the APA/PFS HDD.)
int vcdModeSupported(int mode);
// Is the given device mode currently showing its VCD list (vs its disc list)?
int vcdViewActive(int mode);
// Same query for an item-list instance. A normal source delegates to vcdViewActive(mode); a
// Favourites shallow proxy may force ISO or VCD without changing the source page's own L3 state.
int vcdListViewActive(const item_list_t *itemList);
// Display-only: strip a leading PS1 game-ID prefix from a VCD list name when the gVcdHideGameId
// setting is on and `mode` is a VCD view; returns `text` unchanged otherwise. COSMETIC -- the
// result is for on-screen text only, never for launch/art/favourites/config lookups.
const char *vcdDisplayName(int mode, const char *text);
// Flip the VCD view for a mode + mark it dirty so the owning support's NeedsUpdate forces a rescan.
void vcdToggleView(int mode);
// Returns 1 exactly once after a toggle (and clears the flag) -- call from the support's NeedsUpdate.
int vcdConsumeDirty(int mode);
// Mark all VCD-capable modes dirty (one rescan each) -- used when the global default-view setting changes.
void vcdMarkAllDirty(void);

// Fill a base_game_info_t list (memalign'd like sbReadList; frees *outGames first) from
// <devPrefix>POPS/*.VCD. Returns the count. name/startup = VCD basename without the extension.
int vcdFillGameList(const char *devPrefix, base_game_info_t **outGames);
// #118: 1 if a .VCD filename is disc 2+ of a multi-disc PS1 set ("(Disc N)"/"(CD N)"/"(Disk N)", N>=2,
// case-insensitive). Callers hide it from the device lists when gVcdFirstDiscOnly is on.
int vcdIsHiddenDisc(const char *name);

// ---- safe memory-card copy (free-space gated) -------------------------------------
// Used by the BDMA/SMB module equip + the POPSTARTER config writers so a full or interrupted
// write can never wreck the card. Every write onto mc?:/POPSTARTER/ must go through these.

// Room for `needBytes` (+ safety margin) on `path`'s card? 1 = yes, 0 = no, -1 = not an MC / can't tell.
int vcdMcHasSpace(const char *path, int needBytes);
// Copy srcPath -> dstPath: 0 ok, -1 src missing, -2 MC too full (nothing written), -3 IO error (partial removed).
int vcdSafeCopyFile(const char *srcPath, const char *dstPath);
// Write a buffer to dstPath under the same gate: 0 ok, -2 MC too full, -3 IO error (partial removed).
int vcdSafeWriteFile(const char *dstPath, const void *buf, int len);

// ---- BDMA (BDMAssault exFAT driver) equip -----------------------------------------
// "BDMA MODE": which block-device driver variant POPStarter loads from mc?:/POPSTARTER/.
enum {
    VCD_BDMA_FAT32 = 0, // none -- remove the exFAT modules (POPStarter's built-in FAT32 driver)
    VCD_BDMA_USBEXFAT,  // USB exFAT
    VCD_BDMA_MX4SIO,    // MX4SIO exFAT
    VCD_BDMA_MMCE,      // MMCE exFAT
    VCD_BDMA_ATA,       // internal ATA HDD exFAT (BDMAssault)
    VCD_BDMA_MODE_COUNT
};
// "BDMA SOURCE": which device holds the user-provided variant files in its POPS/ folder. The equip
// resolves each BDM source to the mounted device whose DRIVER matches (USB / MX4SIO / internal-ATA-HDD),
// reading from that device's typed root -- so they are differentiated, not blindly scanned. New values
// are APPENDED so persisted gBdmaSource ints stay stable.
enum {
    VCD_BDMA_SRC_USB = 0, // BDM "usb" driver
    VCD_BDMA_SRC_MX4SIO,  // BDM "mx4sio"/sdc driver
    VCD_BDMA_SRC_MMCE,    // mmce0-1
    VCD_BDMA_SRC_HDD,     // internal exFAT HDD, BDM "ata" driver
    VCD_BDMA_SRC_COUNT
};
// Equip the chosen variant (copy from SOURCE's POPS/, or remove for FAT32) + write the marker.
//   0 ok, -1 bad args, -2 MC too full, -3 IO error, -4 = no source variant files on any seek-path
//   device AND the embedded built-in pair could not be installed either (the embedded fallback makes
//   plain "files not found" self-healing; -4 now means the fallback itself failed).
// On -4, if diag != NULL it is filled with a human-readable summary (needed files + which source
// devices were actually mounted) so the failure can be shown on screen / screenshotted to diagnose.
int vcdEquipBdma(int source, int mode, char *diag, int diagSize);
// Read the equipped variant from mc?:/POPSTARTER/bdma_config.txt (VCD_BDMA_FAT32 if absent).
int vcdReadBdmaMode(void);
// Validate whether the MC environment matches the requested BDMA family (marker + complete pair signature).
int vcdBdmaEnvironmentValid(int mode);
// Auto-equip the BDMA variant matching the game's device (source/mode) before a VCD launch.
// BDMA prep is card preparation, never a POPSTARTER launch gate: failure toasts in passing, never blocks.
void vcdEnsureBdmaForLaunch(int source, int mode);
// Explicit per-launch USB mode pick (the fat32/exFAT dialog on USB VCD launches).
// BDMA prep is card preparation, never a POPSTARTER launch gate: failure toasts in passing, never blocks.
void vcdApplyUsbModeForLaunch(int mode);

// Install POPSTARTER's missing MC-side externals from the VCD device's direct POPS/ folder.
// Existing card files always win. This copies only the SMB/utility IRX, .icn and icon.sys files;
// IPCONFIG.DAT, SMBCONFIG.DAT and bdma_config.txt remain owned by their dedicated settings flows.
// Returns 0 when every file was already present or copied, otherwise the first copy error (-1/-2/-3).
int vcdInstallPopstarterMc(const char *devPrefix);

// Are POPSTARTER's SMB network modules (smbman/ps2ip/ps2smap/ps2dev9) present on a card? 1 = yes.
// Gate SMB/ETH VCD launches on this after the direct-POPS/ installer gets one chance to self-heal.
int vcdSmbModulesPresent(void);

// ---- POPStarter network files (IPCONFIG.DAT / SMBCONFIG.DAT) --------------------------------
// POPSLoader-parity flow: READ existing values when available, otherwise stay blank until the
// user explicitly enters or imports values. Absence means "unknown/unconfigured", never "use
// OPL defaults". Locations: mc0:/POPSTARTER -> mc1:/POPSTARTER, in that precedence order --
// POPSTARTER reads its network files from the memory card ONLY (OPL's own settings live in the
// boot dir/cwd, but that is OUR convention, not POPSTARTER's).
//
// Formats (POPStarter):
//   IPCONFIG.DAT: one line "<PS2 IP> <NETMASK> <GATEWAY>"; a BLANK file = DHCP. The file should
//                 exist even for DHCP -- it is never deleted, only overwritten/created.
//   SMBCONFIG.DAT: line 1 "<SERVER IP>[:PORT] <SHARE NAME>" (port optional, default 445),
//                 line 2 username, line 3 plain-text password; empty lines 2/3 = guest.
typedef struct
{
    int smbExists; // SMBCONFIG.DAT was found (smbDir names where)
    int ipExists;  // IPCONFIG.DAT was found (ipDir names where)
    int ipDhcp;    // 1 = DHCP (file blank or absent); 0 = static triple below is valid
    int ps2Ip[4];
    int ps2Mask[4];
    int ps2Gw[4];
    int smbIp[4];
    int smbPort; // 0 or 445 = default (written bare, no ":PORT")
    char smbShare[32];
    char smbUser[32];
    char smbPass[32];
    char smbDir[96];    // dir SMBCONFIG.DAT was read from ("" when absent)
    char ipDir[96];     // dir IPCONFIG.DAT was read from ("" when absent)
    char createDir[96]; // where files that don't exist yet get created ("" = nowhere available)
} vcd_popsnet_t;

// Scan the candidate dirs and fill *out. Absence of both files is DATA (all fields blank, the
// exists-flags 0, ipDhcp 1), not an error. Returns 0, or -3 on a genuine mid-read IO error.
int vcdReadPopstarterNet(vcd_popsnet_t *out);

// Change detection vs the read-time snapshot, for the save matrix: bit 0 = SMB fields differ,
// bit 1 = IP fields differ. Compares semantic content only (never the dirs/exists flags).
int vcdPopsNetChanged(const vcd_popsnet_t *orig, const vcd_popsnet_t *cur);

// Write SMBCONFIG.DAT (writeSmb) and/or IPCONFIG.DAT (writeIp). Each file goes to its origin dir
// when it existed, else to createDir (mkdir best-effort). A DHCP ipconfig is written BLANK --
// the file must exist either way. Returns 0, or the first vcdSafeWriteFile error (-2/-3).
int vcdWritePopstarterNetFiles(const vcd_popsnet_t *cfg, int writeSmb, int writeIp);

// ---- POPStarter SMB auto-provisioning for mc0:/POPSTARTER/ (RiptOPL launch helper) ----
// Ensures the two required network files for SMB VCD launches exist on mc0:/POPSTARTER/.
// Presence wins: an existing file is never parsed or overwritten.
// Only missing files are generated, using current RiptOPL network/SMB globals when derivable.
// DHCP / missing static PS2 triple -> NEED_STATIC (no bogus file). SMB share/IP validation -> INVALID.
// Destination is fixed to mc0:/POPSTARTER/ (no mc1 fallback) - POPSTARTER module provisioning
// (vcdInstallPopstarterMc) stays dual-slot.
typedef enum {
    VCD_POPSNET_READY = 0,       // both DAT files exist, or were successfully created
    VCD_POPSNET_NEED_STATIC = 1, // missing file requires static PS2 IP / mask / gateway or resolved SMB IP
    VCD_POPSNET_IO_ERROR = 2,    // MC full (-2) or IO error (-3) - partial removed
    VCD_POPSNET_INVALID = 3,     // SMB share/IP etc invalid and not derivable
    VCD_POPSNET_SMB_MISSING = 4  // required SMB IRX modules not present on MC
} vcd_popsnet_ensure_t;

vcd_popsnet_ensure_t vcdEnsurePopstarterSmbConfigMc0(void);

// Shared SMB POPSTARTER preparation for both ETH VCD and APPS SB.*.ELF launches.
// Installs missing MC support from smbPrefix (ethPrefix), verifies SMB modules,
// and ensures mc0:/POPSTARTER/*.DAT. Returns VCD_POPSNET_READY on success.
vcd_popsnet_ensure_t vcdPreparePopstarterSmbLaunch(const char *smbPrefix);

// Render the RetroGEM Game ID optical barcode immediately prior to a POPStarter VCD launch.
void vcdPrepareRetroGemBarcode(const char *vcdPath);

#endif
