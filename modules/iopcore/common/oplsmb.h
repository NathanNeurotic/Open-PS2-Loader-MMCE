
#ifndef __OPLSMB__
#define __OPLSMB__

/*
  Values for server_specs_t.SecurityMode and .PasswordType below.

  These describe the shared cdvdman <-> smbinit contract, so they live with the struct rather than
  in any one dialect's header. They were previously defined twice -- once in cdvdman's smb.h and
  again locally in smbinit's smbauth.c -- which meant the two modules that must agree on
  PasswordType each carried their own copy. Consolidated here when SMB2 became a third consumer.
*/
#define SERVER_SHARE_SECURITY_LEVEL   0
#define SERVER_USER_SECURITY_LEVEL    1
#define SERVER_USE_PLAINTEXT_PASSWORD 0
#define SERVER_USE_ENCRYPTED_PASSWORD 1

typedef struct
{
    u32 MaxBufferSize;
    u32 SessionKey;
    char PrimaryDomainServerName[32];
    u8 EncryptionKey[8];
    u32 Capabilities;
    u16 MaxMpxCount;
    u8 SecurityMode; // 0 = share level, 1 = user level
    u8 PasswordType; // 0 = PlainText passwords, 1 = use challenge/response
    char Username[36];
    char Password[48]; // either PlainText, either hashed
    int PasswordLen;
    int HashedFlag;
    void *IOPaddr;
} server_specs_t;

typedef void (*OplSmbPwHashFunc_t)(server_specs_t *ss);

#define oplsmb_IMPORTS_start DECLARE_IMPORT_TABLE(oplsmb, 1, 1)
#define oplsmb_IMPORTS_end   END_IMPORT_TABLE

void smb_NegotiateProt(OplSmbPwHashFunc_t hash_callback);
#define I_smb_NegotiateProt DECLARE_IMPORT(4, smb_NegotiateProt)

#endif
