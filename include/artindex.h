#ifndef __ART_INDEX_H
#define __ART_INDEX_H

/* An in-RAM listing of an art directory, so that "this cover does not exist" can be answered without
 * asking the device.
 *
 * WHY THIS EXISTS. FAT and exFAT resolve a path by walking directory entries linearly; there is no
 * index on the volume. A lookup for a file that IS there can stop at its entry, but a lookup for one
 * that is NOT there cannot answer until it has read the whole directory. So a missing cover is the
 * worst case by construction, and it is the case a game library hits constantly -- most libraries
 * have art for some titles and not others.
 *
 * Hardware, this build, same USB stick, art loads timed end to end:
 *      512x725 cover ....  203 ms
 *      140x200 cover ....   82 ms
 *      140x200 cover ....  2922 ms
 * Thirteen times the pixels, fourteen times faster. Decode and transfer are therefore not the cost;
 * by elimination it is open(), and a linear directory walk is the mechanism that fits. Reported from
 * hardware as "as soon as it hits a missing artwork, it basically hangs like hell".
 *
 * ONE sequential sweep of the directory replaces every one of those walks. readdir returns entries in
 * storage order, so the sweep costs roughly what a SINGLE failed lookup costs -- and after it, every
 * miss for that directory is a binary search in RAM.
 *
 * THE SAFETY RULE, AND IT IS THE WHOLE DESIGN: this may only ever answer "definitely absent" or
 * "don't know, go and look". It must never answer "present" authoritatively, and it must never
 * answer "absent" on incomplete information. Every uncertainty -- no index, a sweep that failed, a
 * directory too large to hold, a hash collision -- resolves to "go and look", which is exactly the
 * behaviour that existed before this file. The worst a bug here can do is make things as slow as
 * they were; it cannot hide art that exists.
 */

/* Register the ONE thread allowed to touch the index (the art worker). Every other caller of
 * artIndexMayExist gets 1 = "go and look", which is the pre-index behaviour. Confinement rather than a
 * lock: texDiscoverLoad has callers on two threads, and a semaphore between the art path and the
 * render thread is a shape this codebase has already been burned by. */
void artIndexSetOwnerThread(int threadId);

/* 1 = this file may exist, probe it normally. 0 = it is definitely NOT there, skip the open.
 *
 * Takes the full path INCLUDING extension, e.g. "mass0:/ART/SLUS_123.45_COV.png". Lazily sweeps the
 * containing directory on first use. Returns 1 for anything it cannot answer with certainty, which
 * includes being called from any thread other than the registered owner.
 *
 * The sweep can take as long as one failed lookup used to, which is why it belongs on the art worker
 * and not on the GUI thread.
 */
int artIndexMayExist(const char *fullPath);

/* Drop every cached listing. Call wherever the "art may have appeared" epoch is bumped -- a device
 * generation change or a deliberate settings apply -- so a stick with new art on it is picked up.
 * Cheap, and safe from any thread. */
void artIndexInvalidate(void);

/* Debug HUD: directories currently held, and how many probes this has answered as "absent" without
 * touching the device. Either argument may be NULL. */
void artIndexDebug(int *dirsHeld, unsigned int *absentAnswered, unsigned int *sweepsFailed);

#endif
