/*
  SMB2 wire-protocol definitions for the in-game (cdvdman) reader.

  Companion to smb.h, which stays SMB1-only. SMB2 is NOT an extension of SMB1: the header is 64
  bytes instead of 32, there is no AndX chaining, and every command has its own fixed-size request
  and response structure. So rather than bolting dialect switches onto the SMB1 structures, this is
  a parallel definition set and smb2.c is a parallel implementation. smb.c is left untouched.

  Only the subset the in-game reader needs is defined here: NEGOTIATE, SESSION_SETUP, TREE_CONNECT,
  CREATE, READ, WRITE, CLOSE. Directory enumeration lives on the browse side (smbman), not here.

  Endianness: SMB2 is little-endian on the wire and the IOP is little-endian, so the packed structs
  below map directly with no byte-swapping. The ONLY big-endian field in the whole exchange is the
  4-byte direct-TCP transport length, which is handled the same way smb.c handles it.
*/

#ifndef __SMB2_H__
#define __SMB2_H__

#include <tamtypes.h>
#include "oplsmb.h"

// Protocol id: 0xFE 'S' 'M' 'B' read as a little-endian u32.
#define SMB2_MAGIC 0x424d53fe

#define SMB2_HDR_SIZE 64

// Commands (subset).
#define SMB2_NEGOTIATE       0x0000
#define SMB2_SESSION_SETUP   0x0001
#define SMB2_LOGOFF          0x0002
#define SMB2_TREE_CONNECT    0x0003
#define SMB2_TREE_DISCONNECT 0x0004
#define SMB2_CREATE          0x0005
#define SMB2_CLOSE           0x0006
#define SMB2_READ            0x0008
#define SMB2_WRITE           0x0009

// Dialect revisions.
#define SMB2_DIALECT_202 0x0202
#define SMB2_DIALECT_210 0x0210
#define SMB2_DIALECT_300 0x0300
#define SMB2_DIALECT_302 0x0302
#define SMB2_DIALECT_311 0x0311

// Header Flags.
#define SMB2_FLAGS_SERVER_TO_REDIR 0x00000001
#define SMB2_FLAGS_ASYNC_COMMAND   0x00000002
#define SMB2_FLAGS_SIGNED          0x00000008

// NEGOTIATE / SESSION_SETUP SecurityMode.
#define SMB2_NEGOTIATE_SIGNING_ENABLED  0x0001
#define SMB2_NEGOTIATE_SIGNING_REQUIRED 0x0002

// NT status codes we act on. STATUS_SUCCESS is shared with smb.h's definition; guard so this header
// can be included alongside it without a redefinition warning.
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000
#endif
#define STATUS_MORE_PROCESSING_REQUIRED 0xc0000016
#define STATUS_LOGON_FAILURE_NT         0xc000006d

// CREATE: DesiredAccess bits.
#define SMB2_FILE_READ_DATA              0x00000001
#define SMB2_FILE_WRITE_DATA             0x00000002
#define SMB2_FILE_READ_ATTR              0x00000080
// CREATE: ShareAccess bits.
#define SMB2_FILE_SHARE_READ             0x00000001
#define SMB2_FILE_SHARE_WRITE            0x00000002
// CREATE: CreateDisposition.
#define SMB2_FILE_OPEN                   0x00000001
// CREATE: CreateOptions.
#define SMB2_FILE_NON_DIRECTORY_FILE     0x00000040
// CREATE: ImpersonationLevel.
#define SMB2_IMPERSONATION_IMPERSONATION 0x00000002

/*
  SMB2 packet header (MS-SMB2 2.2.1.2, SYNC form).

  The 4 bytes at offset 32 are Reserved in the sync form (they are the upper half of AsyncId in the
  async form). We only ever issue synchronous requests, so Reserved it is.
*/
typedef struct
{
    u32 ProtocolId; // 0xFE 'S' 'M' 'B'
    u16 StructureSize;
    u16 CreditCharge;
    u32 Status; // request: ChannelSequence+Reserved; response: NTSTATUS
    u16 Command;
    u16 CreditRequest;
    u32 Flags;
    u32 NextCommand;
    u32 MessageIdLow;
    u32 MessageIdHigh;
    u32 Reserved;
    u32 TreeId;
    u32 SessionIdLow;
    u32 SessionIdHigh;
    u8 Signature[16];
} __attribute__((packed)) SMB2_Header_t;

/*
  A FileId is a 16-byte opaque handle (Persistent + Volatile). Unlike SMB1's 16-bit FID it does not
  fit in cdvdman_settings.FIDs[], which is why smb2.c keeps its own handle table indexed by the
  small integer the rest of cdvdman passes around. See smb2.c "handle table".
*/
typedef struct
{
    u8 id[16];
} __attribute__((packed)) SMB2_FileId_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 36
    u16 DialectCount;
    u16 SecurityMode;
    u16 Reserved;
    u32 Capabilities;
    u8 ClientGuid[16];
    u32 ClientStartTimeLow;
    u32 ClientStartTimeHigh;
    u16 Dialects[4]; // variable-length in the spec; we offer at most 4
} __attribute__((packed)) SMB2_NegotiateRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 65
    u16 SecurityMode;
    u16 DialectRevision;
    u16 Reserved;
    u8 ServerGuid[16];
    u32 Capabilities;
    u32 MaxTransactSize;
    u32 MaxReadSize;
    u32 MaxWriteSize;
    u32 SystemTimeLow;
    u32 SystemTimeHigh;
    u32 ServerStartTimeLow;
    u32 ServerStartTimeHigh;
    u16 SecurityBufferOffset;
    u16 SecurityBufferLength;
    u32 Reserved2;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_NegotiateResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 25 (24 fixed + 1 buffer byte)
    u8 Flags;
    u8 SecurityMode;
    u32 Capabilities;
    u32 Channel;
    u16 SecurityBufferOffset;
    u16 SecurityBufferLength;
    u32 PreviousSessionIdLow;
    u32 PreviousSessionIdHigh;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_SessionSetupRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 9
    u16 SessionFlags;
    u16 SecurityBufferOffset;
    u16 SecurityBufferLength;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_SessionSetupResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 9 (8 fixed + 1 buffer byte)
    u16 Reserved;
    u16 PathOffset;
    u16 PathLength;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_TreeConnectRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 16
    u8 ShareType;
    u8 Reserved;
    u32 ShareFlags;
    u32 Capabilities;
    u32 MaximalAccess;
} __attribute__((packed)) SMB2_TreeConnectResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 57 (56 fixed + 1 buffer byte)
    u8 SecurityFlags;
    u8 RequestedOplockLevel;
    u32 ImpersonationLevel;
    u32 SmbCreateFlagsLow;
    u32 SmbCreateFlagsHigh;
    u32 ReservedLow;
    u32 ReservedHigh;
    u32 DesiredAccess;
    u32 FileAttributes;
    u32 ShareAccess;
    u32 CreateDisposition;
    u32 CreateOptions;
    u16 NameOffset;
    u16 NameLength;
    u32 CreateContextsOffset;
    u32 CreateContextsLength;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_CreateRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 89
    u8 OplockLevel;
    u8 Flags;
    u32 CreateAction;
    u32 CreationTimeLow;
    u32 CreationTimeHigh;
    u32 LastAccessTimeLow;
    u32 LastAccessTimeHigh;
    u32 LastWriteTimeLow;
    u32 LastWriteTimeHigh;
    u32 ChangeTimeLow;
    u32 ChangeTimeHigh;
    u32 AllocationSizeLow;
    u32 AllocationSizeHigh;
    u32 EndOfFileLow;
    u32 EndOfFileHigh;
    u32 FileAttributes;
    u32 Reserved2;
    SMB2_FileId_t FileId;
    u32 CreateContextsOffset;
    u32 CreateContextsLength;
} __attribute__((packed)) SMB2_CreateResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 49 (48 fixed + 1 buffer byte)
    u8 Padding;
    u8 Flags;
    u32 Length;
    u32 OffsetLow;
    u32 OffsetHigh;
    SMB2_FileId_t FileId;
    u32 MinimumCount;
    u32 Channel;
    u32 RemainingBytes;
    u16 ReadChannelInfoOffset;
    u16 ReadChannelInfoLength;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_ReadRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 17
    u8 DataOffset;
    u8 Reserved;
    u32 DataLength;
    u32 DataRemaining;
    u32 Reserved2;
} __attribute__((packed)) SMB2_ReadResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 49 (48 fixed + 1 buffer byte)
    u16 DataOffset;
    u32 Length;
    u32 OffsetLow;
    u32 OffsetHigh;
    SMB2_FileId_t FileId;
    u32 Channel;
    u32 RemainingBytes;
    u16 WriteChannelInfoOffset;
    u16 WriteChannelInfoLength;
    u32 Flags;
    u8 Buffer[1];
} __attribute__((packed)) SMB2_WriteRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 17
    u16 Reserved;
    u32 Count;
    u32 Remaining;
    u16 WriteChannelInfoOffset;
    u16 WriteChannelInfoLength;
} __attribute__((packed)) SMB2_WriteResponse_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 24
    u16 Flags;
    u32 Reserved;
    SMB2_FileId_t FileId;
} __attribute__((packed)) SMB2_CloseRequest_t;

typedef struct
{
    SMB2_Header_t hdr;
    u16 StructureSize; // 60
    u16 Flags;
    u32 Reserved;
    u8 Fill[52];
} __attribute__((packed)) SMB2_CloseResponse_t;

/*
  Public SMB2 client API. Mirrors the SMB1 entry points in smb.h one-for-one so smbdisp.c can
  dispatch between them without the callers (device-smb.c, mcemu) knowing which dialect is live.

  Return convention matches smb.c: > 0 / 0 on success as noted per function, negative on failure.
*/
int smb2_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, OplSmbPwHashFunc_t hash_callback);
int smb2_SessionSetup(void);
int smb2_TreeConnect(char *ShareName);
int smb2_Open(char *filename, u16 *FID, int Write);
int smb2_Close(int FID);
int smb2_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes);
int smb2_WriteFile(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes);
int smb2_Disconnect(void);

// Negotiated dialect, 0 until smb2_NegotiateProtocol succeeds. Used by smbdisp.c for reporting.
extern u16 smb2_dialect;

#endif
