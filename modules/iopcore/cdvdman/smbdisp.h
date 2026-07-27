/*
  Dialect-agnostic SMB API for the in-game reader.

  This is the ONLY SMB surface the rest of cdvdman (device-smb.c) and mcemu should call. Each entry
  point routes to the SMB1 implementation in smb.c or the SMB2 one in smb2.c, according to the
  dialect bits the EE side put in cdvdman_settings.common.flags at launch.

  The names here are deliberately the historical smb_* names, because mcemu binds smb_OpenAndX,
  smb_ReadFile, smb_WriteFile and smb_Close by ORDINAL out of the oplutils export table
  (see exports.tab and mcemu/mcemu_utils.h). Keeping the names means the export table, the
  ordinals, and every existing caller stay exactly as they were.
*/

#ifndef __SMBDISP_H__
#define __SMBDISP_H__

#include <tamtypes.h>
#include "oplsmb.h"

// Which implementation is live. Resolved once, at negotiate time, from cdvdman_settings.
#define SMB_DIALECT_V1 0
#define SMB_DIALECT_V2 1

extern int smb_active_dialect;

int smb_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, u32 *capabilities, OplSmbPwHashFunc_t hash_callback);
int smb_SessionSetupAndX(u32 capabilities);
int smb_TreeConnectAndX(char *ShareName);
int smb_OpenAndX(char *filename, u8 *FID, int Write);
int smb_Close(int FID);
int smb_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes);
int smb_WriteFile(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes);
int smb_ReadCD(unsigned int lsn, unsigned int nsectors, void *buf, int part_num);
void smb_CloseAll(void);
int smb_Disconnect(void);

#endif
