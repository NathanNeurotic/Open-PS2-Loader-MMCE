#ifndef STUB_THBASE_H
#define STUB_THBASE_H
#include "ps2ip.h"
typedef struct
{
    u32 lo, hi;
} iop_sys_clock_t;
void GetSystemTime(iop_sys_clock_t *clock);
void SysClock2USec(iop_sys_clock_t *clock, u32 *seconds, u32 *useconds);
#endif
