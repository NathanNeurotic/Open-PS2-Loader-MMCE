/*
  IOP shim: the PS2 IOP has no separate BSD socket headers -- ps2ip.h carries the whole
  socket surface (types, constants, lwip_* calls) in one file. libsmb2 is portable C and
  includes the usual POSIX paths, so forward them here rather than editing vendored sources.
*/
#ifndef SMB2MAN_SHIM_in
#define SMB2MAN_SHIM_in
#include <ps2ip.h>
#endif
