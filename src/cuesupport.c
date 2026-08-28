/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  CUE (PS1-via-Ember) path resolution and argument validation. See include/cuesupport.h for the
  measured Ember contract this implements, and docs/EMBER-INTEGRATION-PLAN.md for its derivation.

  POSIX IO only -- the newlib port rejects direct fileXio use here, same rule as vcdsupport.c.
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "include/opl.h"
#include "include/cuesupport.h"

// Path separator for a device prefix: '\\' for SMB (its prefix ends in a backslash), else '/'.
// Auto-detected from the trailing character so one code path serves mass/mmce/pfs and SMB alike.
// Same rule as vcdsupport.c's vcdSep(); kept local rather than shared so neither file's separator
// behaviour can be changed from under the other.
static char cueSep(const char *devPrefix)
{
    int n = (devPrefix != NULL) ? (int)strlen(devPrefix) : 0;
    return (n > 0 && devPrefix[n - 1] == '\\') ? '\\' : '/';
}

const char *cueEmberFolder(void)
{
    // A configurable folder name arrives with the Ember settings page. Resolving it through this
    // accessor from the start keeps that change to one line and keeps every caller honest about
    // the fact that the folder is not a fixed part of the contract -- the ember.elf's DIRECTORY is.
    return EMBER_FOLDER_DEFAULT;
}

// Shared body for the two file probes: compose "<devPrefix><folder><sep><file>" and open() it.
static int cueResolveEmberFile(const char *devPrefix, const char *fileName, char *out, int outSize)
{
    int fd;

    if (out == NULL || outSize <= 0 || devPrefix == NULL || fileName == NULL)
        return 0;

    snprintf(out, outSize, "%s%s%c%s", devPrefix, cueEmberFolder(), cueSep(devPrefix), fileName);

    fd = open(out, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int cueResolveEmber(const char *devPrefix, char *out, int outSize)
{
    return cueResolveEmberFile(devPrefix, EMBER_ELF_NAME, out, outSize);
}

int cueResolveEmberBios(const char *devPrefix, char *out, int outSize)
{
    return cueResolveEmberFile(devPrefix, EMBER_BIOS_NAME, out, outSize);
}

void cueBuildGamesDir(const char *devPrefix, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;
    if (devPrefix == NULL) {
        out[0] = '\0';
        return;
    }

    char sep = cueSep(devPrefix);
    snprintf(out, outSize, "%s%s%c%s", devPrefix, cueEmberFolder(), sep, EMBER_GAMES_FOLDER);
}

int cueNameLaunchable(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;

    // Ember refuses any argument STARTING with ".." -- it compares buf[0] and buf[1] and never looks
    // further, so "..", "../x" and "..foo" are all refused. Match that, not the tidier "== ..".
    if (name[0] == '.' && name[1] == '.')
        return 0;

    // Scanner artefacts: the current/parent directory entries are never games.
    if (!strcmp(name, "."))
        return 0;

    // Ember's char scan refuses '/', ':' and '\\' ANYWHERE in the argument. On FAT/exFAT these
    // cannot appear in a directory entry, so in practice this fires only on a hand-typed argument
    // -- but it is the difference between a clear message and Ember dropping to the PS1 BIOS shell.
    if (strpbrk(name, "/:\\") != NULL)
        return 0;

    // Longer than Ember's own buffers can carry (see CUE_NAME_LAUNCH_MAX).
    if ((int)strlen(name) > CUE_NAME_LAUNCH_MAX)
        return 0;

    return 1;
}
