/* RA: the watch list for the game being launched. See src/rawatch.c */
#ifndef __RAWATCH_H__
#define __RAWATCH_H__

int LoadWatchList(const char *path, const char *startup);
/* One line in the debug launch log: what happened plus two numbers.
   Independent of the hash log, which is already closed by launch time. */
void raLaunchNote(const char *what, int a, int b);
/* Takes the list from memory: the network brought it and the file may
   not have reached the medium yet. Records which game it belongs to. */
int SetWatchList(const void *data, int len, const char *startup);
unsigned int *GetWatchList(void);
int GetWatchCount(void);
int GetWatchBytes(void);
void ClearWatchList(void);

#endif
