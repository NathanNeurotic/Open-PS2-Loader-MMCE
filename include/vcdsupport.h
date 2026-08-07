#ifndef __VCDSUPPORT_H
#define __VCDSUPPORT_H

#define VCD_NAME_MAX 256 // VCD basename without ".VCD" (incl NUL); becomes the selector game name
#define VCD_ID_MAX   16  // optional extracted PS1 disc ID, e.g. "SCUS_123.45"

// STUB (rebuild): the PS1/VCD subsystem returns with checklist item 12. The theme
// engine calls vcdDisplayName() on list rows; until VCD lists exist there is nothing
// to transform, so the identity mapping below is exact, not an approximation.
static inline const char *vcdDisplayName(int mode, const char *text)
{
    (void)mode;
    return text;
}

// No VCD views exist, so no mode is ever showing one, no toggle can ever mark one
// dirty, and no list name can carry a PS1 game-ID prefix. All three are exact.
static inline int vcdViewActive(int mode)
{
    (void)mode;
    return 0;
}
static inline int vcdConsumeDirty(int mode)
{
    (void)mode;
    return 0;
}
static inline int vcdExtractGameId(const char *name, char *idOut, int idSize)
{
    (void)name;
    if (idOut != NULL && idSize > 0)
        idOut[0] = '\0';
    return 0;
}

#endif
