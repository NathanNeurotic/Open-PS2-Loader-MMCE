#ifndef __MMCESUPPORT_H
#define __MMCESUPPORT_H

// STUB (rebuild): the MMCE subsystem returns with checklist item 1. No mmceman module is
// embedded in this tree, so loading is impossible; the BDMA equip's mmce0:/mmce1: probes
// then fail and its "no device" diagnostic is honest -- the no-op below is exact.
static inline void mmceLoadModules(void)
{
}

// GameID transport stubs, matching the real implementation's feature-off paths exactly:
// mmceSendGameID returns 0 when the feature is off (real code: !gMMCEEnableGameID -> 0);
// with no send ever made there is nothing to settle (0 = settled); arming is a documented
// no-op when the feature is off.
static inline int mmceSendGameID(const char *startup, const char *protectMcPath, int vmcSlotMask)
{
    (void)startup;
    (void)protectMcPath;
    (void)vmcSlotMask;
    return 0;
}
static inline int mmceGameIdSettle(int timeoutMs)
{
    (void)timeoutMs;
    return 0;
}
static inline void mmceArmGameIDTransport(void)
{
}

#endif
