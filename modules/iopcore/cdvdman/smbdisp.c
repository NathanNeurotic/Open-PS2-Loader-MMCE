/*
  SMB dialect dispatcher for the in-game reader.

  device-smb.c and mcemu call the smb_* entry points defined here; each one forwards to either
  smb.c (SMB1) or smb2.c (SMB2). The dialect is decided ONCE, in smb_NegotiateProtocol(), from the
  bits the EE side wrote into cdvdman_settings.common.flags, and latched in smb_active_dialect for
  every later call. It is never re-evaluated mid-session: a game that is streaming sectors must not
  have the transport change underneath it.

  SMB1 remains the default. An unset/unknown dialect field means SMB1, so an older config -- or an
  EE build that predates the picker -- behaves exactly as it always has.
*/

#include <stdio.h>
#include <errno.h>
#include <sysclib.h>
#include <tamtypes.h>

#include "oplsmb.h"
#include "smb.h"
#include "smb2.h"
#include "smbdisp.h"
#include "cdvd_config.h"

extern struct cdvdman_settings_smb cdvdman_settings;

int smb_active_dialect = SMB_DIALECT_V1;

//-------------------------------------------------------------------------
int smb_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, u32 *capabilities, OplSmbPwHashFunc_t hash_callback)
{
    /*
      Latch the dialect for the whole session. Read it here rather than in each wrapper so there is
      exactly one place where the choice is made, and so a caller can never see two different
      dialects across two calls.
    */
    switch (cdvdman_settings.common.flags & IOPCORE_SMB_DIALECT_MASK) {
        case IOPCORE_SMB_DIALECT_V2:
            smb_active_dialect = SMB_DIALECT_V2;
            break;
        default:
            // V1, and any value this build does not recognise -- fall back to the dialect that has
            // always worked rather than failing to boot.
            smb_active_dialect = SMB_DIALECT_V1;
            break;
    }

    if (smb_active_dialect == SMB_DIALECT_V2) {
        /*
          SMB2 has no equivalent of SMB1's capabilities word -- the fields it would carry
          (large read/write, unicode, 64-bit offsets) are all mandatory in SMB2. Zero it so the
          value device-smb.c threads into smb_SessionSetupAndX() is well-defined and unused.
        */
        *capabilities = 0;
        return smb2_NegotiateProtocol(SMBServerIP, SMBServerPort, Username, Password, hash_callback);
    }

    return smb1_NegotiateProtocol(SMBServerIP, SMBServerPort, Username, Password, capabilities, hash_callback);
}

//-------------------------------------------------------------------------
int smb_SessionSetupAndX(u32 capabilities)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_SessionSetup();

    return smb1_SessionSetupAndX(capabilities);
}

//-------------------------------------------------------------------------
int smb_TreeConnectAndX(char *ShareName)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_TreeConnect(ShareName);

    return smb1_TreeConnectAndX(ShareName);
}

//-------------------------------------------------------------------------
/*
  Note the FID pointer type: SMB1's smb1_OpenAndX() takes u8* and memcpy()s 2 bytes into it, while
  smb2_Open() takes a u16*. Both callers actually pass the address of a u16 (cdvdman_settings.FIDs[]
  entries, or vmcSpec[].fid), so the u8* is a historical quirk rather than a real byte-pointer. We
  keep the u8* signature to avoid touching call sites and cast on the SMB2 side.
*/
int smb_OpenAndX(char *filename, u8 *FID, int Write)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_Open(filename, (u16 *)FID, Write);

    return smb1_OpenAndX(filename, FID, Write);
}

//-------------------------------------------------------------------------
int smb_Close(int FID)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_Close(FID);

    return smb1_Close(FID);
}

//-------------------------------------------------------------------------
int smb_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_ReadFile(FID, offsetlow, offsethigh, readbuf, nbytes);

    return smb1_ReadFile(FID, offsetlow, offsethigh, readbuf, nbytes);
}

//-------------------------------------------------------------------------
int smb_WriteFile(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_WriteFile(FID, offsetlow, offsethigh, writebuf, nbytes);

    return smb1_WriteFile(FID, offsetlow, offsethigh, writebuf, nbytes);
}

//-------------------------------------------------------------------------
// Dialect-independent glue, previously duplicated at the bottom of smb.c. Both dialects hand out a
// u16 handle stored in cdvdman_settings.FIDs[], so one implementation serves both.
int smb_ReadCD(unsigned int lsn, unsigned int nsectors, void *buf, int part_num)
{
    return smb_ReadFile(cdvdman_settings.FIDs[part_num], lsn * 2048, lsn >> 21, buf, (int)(nsectors * 2048));
}

void smb_CloseAll(void)
{
    int i, fd;

    for (i = 0; i < cdvdman_settings.common.NumParts; i++) {
        fd = cdvdman_settings.FIDs[i];
        if (fd >= 0) {
            smb_Close(fd);
            cdvdman_settings.FIDs[i] = -1;
        }
    }
}

//-------------------------------------------------------------------------
int smb_Disconnect(void)
{
    if (smb_active_dialect == SMB_DIALECT_V2)
        return smb2_Disconnect();

    return smb1_Disconnect();
}
