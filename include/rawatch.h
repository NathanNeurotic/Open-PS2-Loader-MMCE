/* RA: the watch list for the game being launched. See src/rawatch.c */
#ifndef __RAWATCH_H__
#define __RAWATCH_H__

/* Reads one specific watch-list file. Callers go through sbLoadWatchList(),
   which owns where a list may live. Returns the entry count, or negative on
   failure -- a missing file is not an error, the game simply has no set. */
int LoadWatchListFile(const char *file, const char *startup);

/* Takes the list from memory instead of a file: the network brought it and the
   file may not have reached the medium yet. Records which game it belongs to. */
int SetWatchList(const void *data, int len, const char *startup);

unsigned int *GetWatchList(void);
int GetWatchCount(void);
int GetWatchBytes(void);
void ClearWatchList(void);

/* One line in the launch log: what happened plus two numbers. Compiled away
   unless this is an __OPLDIAG build. */
void raLaunchNote(const char *what, int a, int b);

#endif
