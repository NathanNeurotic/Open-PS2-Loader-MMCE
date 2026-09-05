/*
  RA: the low-level transmit export of the smap-ingame driver.

  Index 4 in the driver's export table (modules/network/smap-ingame/exports.tab):
  0 = _start, 1..3 = _retonly, 4 = SMAPSendPacket.

  Takes a complete Ethernet frame, header included, and its length. A
  mutex inside the driver serializes it with the stack's own sends, so
  any thread may call it.
*/

#ifndef __SMAP_TX_H__
#define __SMAP_TX_H__

#define smap_driver_IMPORTS_start DECLARE_IMPORT_TABLE(smap_driver, 1, 1)
#define smap_driver_IMPORTS_end   END_IMPORT_TABLE

int SMAPSendPacket(const void *data, unsigned int length);
#define I_SMAPSendPacket DECLARE_IMPORT(4, SMAPSendPacket)

#endif /* __SMAP_TX_H__ */
