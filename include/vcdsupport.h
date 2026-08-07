#ifndef __VCDSUPPORT_H
#define __VCDSUPPORT_H

// STUB (rebuild): the PS1/VCD subsystem returns with checklist item 12. The theme
// engine calls vcdDisplayName() on list rows; until VCD lists exist there is nothing
// to transform, so the identity mapping below is exact, not an approximation.
static inline const char *vcdDisplayName(int mode, const char *text)
{
    (void)mode;
    return text;
}

#endif
