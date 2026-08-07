#ifndef __FOLDERBROWSE_H
#define __FOLDERBROWSE_H

// Lazy per-device folder navigation for the game list (opt-in: gEnableFolderNav).
//
// A subdirectory found under <prefix>CD/ or <prefix>DVD/ is listed as a GAME_FORMAT_FOLDER row.
// Selecting it (the select button) DESCENDS -- the list rescans one level deeper; the cancel
// button ASCENDS back up. Each folder view is the same flat game list repopulated in place, so
// covers / favourites / coverflow / per-game settings all keep working unchanged.
//
// Only the loose-file tree scanners participate (BDM/USB/MX4SIO/iLink/exFAT-HDD-BDM, MMCE and
// UDPFS-Files) -- every device that reaches sbReadList/scanForISO with a "/" separator. APA-HDD
// (HDL partitions), the UDPFS block device and the VCD/POPS view are structurally excluded: they
// never scan a CD/DVD directory tree. SMB/ETH is deferred (it uses a "\\" separator).
//
// The state model deliberately mirrors the proven VCD-view machinery (vcdsupport.c): a per-mode
// subpath + a dirty flag consumed in each device's NeedsUpdate to force the deferred rescan.

#define FOLDER_SUB_MAX   128 // joined subpath cap; fits <prefix>CD/<sub> inside the 256-byte path buffers
#define FOLDER_DEPTH_MAX 8   // max nesting levels below the CD/DVD root

// True only for the loose-file tree device modes that can browse folders.
int folderModeSupported(int mode);

// The current subpath BELOW the CD/DVD root, "/"-joined (e.g. "RPGs/SNES"); "" at the device root.
// Never returns NULL. Returns "" when folder-nav is off or the mode is not folder-capable.
const char *folderGetSub(int mode);

// Nesting depth (0 == at the device root).
int folderDepth(int mode);

// Push a folder level (descend). Returns 1 on success, 0 if refused (disabled, at depth cap, or the
// joined path would overflow). Marks the mode dirty so its next NeedsUpdate forces a rescan.
int folderDescend(int mode, const char *name);

// Pop a folder level (ascend). Returns 1 if it actually ascended, 0 if already at the root.
int folderAscend(int mode);

// Force the mode back to the device root (marks dirty only if it was not already at root).
void folderReset(int mode);

// One-shot: returns 1 once after a descend/ascend/reset, so NeedsUpdate can force the rescan even
// when the device's own change-detection would otherwise short-circuit (mirrors vcdConsumeDirty).
int folderConsumeDirty(int mode);

#endif
