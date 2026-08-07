#ifndef __MMCESUPPORT_H
#define __MMCESUPPORT_H

// STUB (rebuild): the MMCE subsystem returns with checklist item 1. No mmceman module is
// embedded in this tree, so loading is impossible; the BDMA equip's mmce0:/mmce1: probes
// then fail and its "no device" diagnostic is honest -- the no-op below is exact.
static inline void mmceLoadModules(void)
{
}

#endif
