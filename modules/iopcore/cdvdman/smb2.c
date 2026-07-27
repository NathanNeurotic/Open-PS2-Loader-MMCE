/*
  SMB2 client for the in-game (cdvdman) disc reader.

  Parallel implementation to smb.c (SMB1), selected at launch time via the dialect bits in
  cdvdman_settings.common.flags. smb.c is deliberately left untouched: SMB1 remains the default and
  its behaviour must not shift by so much as a byte, because it is the only path with years of
  hardware validation behind it.

  Scope: exactly what the in-game reader needs -- connect, authenticate, open the ISO part files,
  read sectors, and (for VMC) write. Directory enumeration is a browse-side concern and lives in
  smbman, not here.

  Two deliberate limitations, both of which fail LOUDLY rather than silently misbehaving:

  1. No packet signing. SMB2 signing needs HMAC-SHA256, and there is no SHA/HMAC implementation
     anywhere in this tree (smbinit provides only DES + MD4, for NTLM). If a server answers
     NEGOTIATE with SIGNING_REQUIRED we abort with -SMB2_ERR_SIGNING_REQUIRED instead of sending
     unsigned traffic that the server would reject in a far more confusing way. Adding signing is
     the prerequisite for SMB3, which mandates it.

  2. NTLMv1 authentication only, via the existing smbinit hash callback. NTLMv2 needs HMAC-MD5,
     which is likewise not in-tree. Note that recent Samba defaults to refusing NTLMv1
     ("ntlm auth = disabled"); such a server reports STATUS_LOGON_FAILURE and we surface it as
     -SMB2_ERR_LOGON_FAILURE.

  Endianness: SMB2 is little-endian and so is the IOP, so the packed structs in smb2.h map straight
  onto the wire. The lone big-endian field is the 4-byte direct-TCP transport length.
*/

#include <stdio.h>
#include <errno.h>
#include <sysclib.h>
#include "smstcpip.h"
#include <limits.h>
#include <thbase.h>
#include <thsemap.h>
#include <intrman.h>
#include <sifman.h>

#include "oplsmb.h"
#include "smb2.h"
#include "cdvd_config.h"

#include "smsutils.h"

// Round up the erasure amount so memset can clear word-by-word (mirrors smb.c).
#define ZERO_PKT_ALIGNED(hdr, hdrSize) memset((hdr), 0, ((hdrSize) + 3) & ~3)

/*
  Cap receive chunks well below the lwIP TCP window (10240 per lwipopts.h) for the same reason
  smb.c does: the IOP cannot drain frames fast enough, and letting bytes-in-flight grow trips the
  server's congestion-avoidance, which costs far more than the smaller chunks save.
*/
#define SMB2_MAX_RECV_SIZE 8192
#define SMB2_MAX_XMIT_SIZE 8192
#define SMB2_BUF_SIZE      1024 // headers + NTLMSSP blobs + UTF-16 paths; bulk data never lands here

// Error codes, returned negated. Distinct values so a hardware report can name the failure.
#define SMB2_ERR_TRANSPORT         1
#define SMB2_ERR_PROTOCOL          2
#define SMB2_ERR_SIGNING_REQUIRED  3
#define SMB2_ERR_LOGON_FAILURE     4
#define SMB2_ERR_NO_DIALECT        5
#define SMB2_ERR_HANDLES_EXHAUSTED 6
#define SMB2_ERR_BAD_HANDLE        7

// ps2ip exported function pointers, set up by device-smb.c's ps2ip_init().
extern int (*plwip_close)(int s);
extern int (*plwip_connect)(int s, struct sockaddr *name, socklen_t namelen);
extern int (*plwip_recv)(int s, void *mem, int len, unsigned int flags);
extern int (*plwip_send)(int s, void *dataptr, int size, unsigned int flags);
extern int (*plwip_socket)(int domain, int type, int protocol);
extern int (*plwip_setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
extern u32 (*pinet_addr)(const char *cp);

// Shared with smb.c / device-smb.c (DeviceLock waits on it). Exactly one dialect runs per boot, so
// whichever negotiate path executes is the one that creates it.
extern int smb_io_sema;

u16 smb2_dialect = 0;

static int main_socket = -1;
static u32 session_id_low, session_id_high;
static u32 tree_id;
static u32 message_id_low, message_id_high;
static server_specs_t server_specs;
static OplSmbPwHashFunc_t stored_hash_callback;

/*
  Handle table.

  SMB1 hands callers a 16-bit FID, and cdvdman stores those in cdvdman_settings.FIDs[]. An SMB2
  FileId is 16 opaque bytes and does not fit. Rather than reshape cdvdman_settings (its SMB fields
  share a union with FIDs[], so the layout is not ours to grow), we keep the real FileIds here and
  hand out small indices that look exactly like SMB1 FIDs to every caller.
*/
#define SMB2_MAX_HANDLES (ISO_MAX_PARTS + 2) // ISO parts, plus headroom for the two VMC files

static struct
{
    SMB2_FileId_t fid;
    int used;
} handle_table[SMB2_MAX_HANDLES];

static struct
{
    u32 sessionHeader; // direct-TCP: 1 zero byte + 24-bit big-endian payload length
    u8 buf[SMB2_BUF_SIZE];
} __attribute__((packed)) SMB2_buf;

//-------------------------------------------------------------------------
// Direct-TCP transport framing. Identical scheme to smb.c: raw TCP, not NBT.
static void nb_SetSessionMessage(u32 size)
{
    SMB2_buf.sessionHeader = ((size & 0xff0000) >> 8) | ((size & 0xff00) << 8) | ((size & 0xff) << 24);
}

static int nb_GetSessionMessageLength(void)
{
    u32 size = ((SMB2_buf.sessionHeader << 8) & 0xff0000) | ((SMB2_buf.sessionHeader >> 8) & 0xff00) | ((SMB2_buf.sessionHeader >> 24) & 0xff);
    return (int)size;
}

static u8 nb_GetPacketType(void)
{
    return ((u8)(SMB2_buf.sessionHeader & 0xff));
}

//-------------------------------------------------------------------------
static int SendData(int sock, char *buf, int size)
{
    int remaining, result;
    char *ptr;

    ptr = buf;
    remaining = size;
    while (remaining > 0) {
        result = plwip_send(sock, ptr, remaining, 0);
        if (result <= 0)
            return result;

        ptr += result;
        remaining -= result;
    }

    return size;
}

static int RecvData(int sock, char *buf, int size)
{
    int remaining, result;
    char *ptr;

    ptr = buf;
    remaining = size;
    while (remaining > 0) {
        result = plwip_recv(sock, ptr, remaining, 0);
        if (result <= 0)
            return result;

        ptr += result;
        remaining -= result;
    }

    return size;
}

//-------------------------------------------------------------------------
static int OpenTCPSession(struct in_addr dst_IP, u16 dst_port)
{
    int sock, ret, opt, retries;
    struct sockaddr_in sock_addr;

    sock = plwip_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
        return -1;

    opt = 1;
    plwip_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));

    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_addr = dst_IP;
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(dst_port);

    /*
      smb.c retries forever here. That is survivable for SMB1 because it is the boot-time default
      and a wedge looks like "OPL is still starting", but a bounded retry gives a real error instead
      of an unexplained hang -- the same reasoning as the bounded MMCE wait. 20 x 500ms = 10s.
    */
    for (retries = 0; retries < 20; retries++) {
        ret = plwip_connect(sock, (struct sockaddr *)&sock_addr, sizeof(sock_addr));
        if (ret >= 0)
            return sock;
        DelayThread(500);
    }

    plwip_close(sock);
    return -1;
}

//-------------------------------------------------------------------------
// Fill the 64-byte SMB2 header and bump MessageId. Every request is synchronous and asks for 1
// credit, which is all a strictly serialised single-request-at-a-time client needs.
static void smb2_SetHeader(SMB2_Header_t *hdr, u16 command)
{
    ZERO_PKT_ALIGNED(hdr, sizeof(SMB2_Header_t));

    hdr->ProtocolId = SMB2_MAGIC;
    hdr->StructureSize = SMB2_HDR_SIZE;
    hdr->CreditCharge = 1;
    hdr->Command = command;
    hdr->CreditRequest = 1;
    hdr->MessageIdLow = message_id_low;
    hdr->MessageIdHigh = message_id_high;
    hdr->TreeId = tree_id;
    hdr->SessionIdLow = session_id_low;
    hdr->SessionIdHigh = session_id_high;

    // 64-bit increment on a 32-bit split field.
    message_id_low++;
    if (message_id_low == 0)
        message_id_high++;
}

/*
  Send the request currently staged in SMB2_buf and read the reply back into it.

  rhdrlen == 0 pulls the whole reply; otherwise only that many bytes are read and the caller drains
  the remainder itself (used by the READ path, which streams payload straight into the game's
  buffer rather than bouncing it through SMB2_buf).

  Returns the total reply payload length, or negative on transport failure.
*/
static int smb2_Exchange(int reqlen, int rhdrlen)
{
    int rcv_size, totalpkt_size, size;

    nb_SetSessionMessage(reqlen);

    rcv_size = SendData(main_socket, (char *)&SMB2_buf, reqlen + 4);
    if (rcv_size <= 0)
        return -SMB2_ERR_TRANSPORT;

    // Drop keep-alives (type 0x85, no body); process session messages (type 0x00).
    do {
        rcv_size = RecvData(main_socket, (char *)&SMB2_buf.sessionHeader, sizeof(SMB2_buf.sessionHeader));
        if (rcv_size <= 0)
            return -SMB2_ERR_TRANSPORT;
    } while (nb_GetPacketType() != 0);

    totalpkt_size = nb_GetSessionMessageLength();

    /*
      Clamp to what the server actually sent. This matters: an ERROR reply is far shorter than the
      success reply the caller sized rhdrlen for (an SMB2 error response is 64 + 9 bytes, while the
      READ path asks for 80), and without the clamp RecvData would block forever waiting for bytes
      that are never coming -- turning any mid-game read error into a console hang instead of a
      clean failure. With the clamp we consume exactly the packet, and the caller's status check
      sees the error with the socket still in sync.
    */
    size = (rhdrlen == 0 || rhdrlen > totalpkt_size) ? totalpkt_size : rhdrlen;
    // Never let a hostile or confused server overrun SMB2_buf.
    if (size > (int)sizeof(SMB2_buf.buf))
        return -SMB2_ERR_PROTOCOL;

    rcv_size = RecvData(main_socket, (char *)SMB2_buf.buf, size);
    if (rcv_size <= 0)
        return -SMB2_ERR_TRANSPORT;

    return totalpkt_size;
}

// Common reply validation: right protocol, right command, no NT error.
static int smb2_CheckReply(u16 expected_cmd)
{
    SMB2_Header_t *hdr = (SMB2_Header_t *)SMB2_buf.buf;

    if (hdr->ProtocolId != SMB2_MAGIC)
        return -SMB2_ERR_PROTOCOL;
    if (hdr->Command != expected_cmd)
        return -SMB2_ERR_PROTOCOL;
    if (hdr->Status != STATUS_SUCCESS)
        return -SMB2_ERR_PROTOCOL;

    return 0;
}

//-------------------------------------------------------------------------
// ASCII -> UTF-16LE, byte-wise so it is safe at any alignment. Returns bytes written. SMB2 path and
// name fields are counted, never NUL-terminated, so no terminator is emitted.
static int asciiToUtf16(u8 *out, const char *in)
{
    int len = 0;

    while (*in != '\0') {
        out[len] = (u8)*in;
        out[len + 1] = '\0';
        len += 2;
        in++;
    }

    return len;
}

//-------------------------------------------------------------------------
// NTLMSSP. Raw tokens, not SPNEGO-wrapped: both Windows and Samba accept a bare NTLMSSP blob in the
// SESSION_SETUP security buffer, and skipping the GSS-API/DER layer avoids an ASN.1 encoder on the
// IOP for no functional gain.

static const char ntlmssp_signature[8] = {'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0'};

// cdvdman's IOP import table does not carry memcmp (only memcpy/memset are pulled in), and adding
// an import would touch every cdvdman variant's import table for one 8-byte compare. Local helper
// instead. Returns 0 when equal, matching memcmp's convention for the only case we test.
static int smb2_memcmp(const void *a, const void *b, int len)
{
    const u8 *pa = (const u8 *)a, *pb = (const u8 *)b;
    int i;

    for (i = 0; i < len; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }

    return 0;
}

#define NTLMSSP_NEGOTIATE_UNICODE     0x00000001
#define NTLMSSP_REQUEST_TARGET        0x00000004
#define NTLMSSP_NEGOTIATE_NTLM        0x00000200
#define NTLMSSP_NEGOTIATE_ALWAYS_SIGN 0x00008000

// Type 1 (NEGOTIATE): 32-byte fixed message, no payloads.
static int ntlmssp_BuildNegotiate(u8 *out)
{
    memset(out, 0, 32);
    memcpy(out, ntlmssp_signature, 8);
    *(u32 *)(out + 8) = 1; // MessageType
    *(u32 *)(out + 12) = NTLMSSP_NEGOTIATE_UNICODE | NTLMSSP_REQUEST_TARGET | NTLMSSP_NEGOTIATE_NTLM | NTLMSSP_NEGOTIATE_ALWAYS_SIGN;
    // Domain and Workstation fields stay zero-length; the server does not require them here.
    return 32;
}

// Type 2 (CHALLENGE): we only need the 8-byte ServerChallenge at offset 24.
static int ntlmssp_ParseChallenge(const u8 *in, int len, u8 *challenge_out)
{
    if (len < 32)
        return -SMB2_ERR_PROTOCOL;
    if (smb2_memcmp(in, ntlmssp_signature, 8) != 0)
        return -SMB2_ERR_PROTOCOL;
    if (*(u32 *)(in + 8) != 2)
        return -SMB2_ERR_PROTOCOL;

    memcpy(challenge_out, in + 24, 8);
    return 0;
}

/*
  Type 3 (AUTHENTICATE): 64-byte fixed header followed by the payloads it points at.

  Each of the six variable fields is a security-buffer triple {Len u16, MaxLen u16, Offset u32}
  where Offset is measured from the start of the NTLMSSP message. We lay the payloads out in the
  order LM, NT, Domain, User, Workstation and leave the session key empty (we negotiate no sealing).

  The LM/NT responses are the 24-byte NTLMv1 values that smbinit's hash callback already produced
  into server_specs.Password: LM at [0..23], NT at [24..47]. That is the identical layout SMB1's
  session setup consumes, so both dialects share one auth implementation.
*/
static int ntlmssp_BuildAuthenticate(u8 *out, int outmax)
{
    int payload, domain_len, user_len, ws_len;
    const int fixed = 64;

    // Worst case: 64 fixed + 24 + 24 + UTF-16 user + UTF-16 workstation.
    user_len = strlen(server_specs.Username) * 2;
    ws_len = strlen("PLAYSTATION2") * 2;
    domain_len = 0;

    if (fixed + 48 + user_len + ws_len > outmax)
        return -SMB2_ERR_PROTOCOL;

    memset(out, 0, fixed);
    memcpy(out, ntlmssp_signature, 8);
    *(u32 *)(out + 8) = 3; // MessageType

    payload = fixed;

    // LmChallengeResponse (24 bytes) at offset 12.
    memcpy(out + payload, &server_specs.Password[0], 24);
    *(u16 *)(out + 12) = 24;
    *(u16 *)(out + 14) = 24;
    *(u32 *)(out + 16) = payload;
    payload += 24;

    // NtChallengeResponse (24 bytes) at offset 20.
    memcpy(out + payload, &server_specs.Password[24], 24);
    *(u16 *)(out + 20) = 24;
    *(u16 *)(out + 22) = 24;
    *(u32 *)(out + 24) = payload;
    payload += 24;

    // DomainName at offset 28 -- empty; the server resolves the account against its own domain.
    *(u16 *)(out + 28) = domain_len;
    *(u16 *)(out + 30) = domain_len;
    *(u32 *)(out + 32) = payload;

    // UserName at offset 36.
    user_len = asciiToUtf16(out + payload, server_specs.Username);
    *(u16 *)(out + 36) = user_len;
    *(u16 *)(out + 38) = user_len;
    *(u32 *)(out + 40) = payload;
    payload += user_len;

    // Workstation at offset 44.
    ws_len = asciiToUtf16(out + payload, "PLAYSTATION2");
    *(u16 *)(out + 44) = ws_len;
    *(u16 *)(out + 46) = ws_len;
    *(u32 *)(out + 48) = payload;
    payload += ws_len;

    // EncryptedRandomSessionKey at offset 52 -- empty (no sealing negotiated).
    *(u16 *)(out + 52) = 0;
    *(u16 *)(out + 54) = 0;
    *(u32 *)(out + 56) = payload;

    // NegotiateFlags at offset 60, echoing what Type 1 asked for.
    *(u32 *)(out + 60) = NTLMSSP_NEGOTIATE_UNICODE | NTLMSSP_REQUEST_TARGET | NTLMSSP_NEGOTIATE_NTLM | NTLMSSP_NEGOTIATE_ALWAYS_SIGN;

    return payload;
}

//-------------------------------------------------------------------------
int smb2_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, OplSmbPwHashFunc_t hash_callback)
{
    SMB2_NegotiateRequest_t *req = (SMB2_NegotiateRequest_t *)SMB2_buf.buf;
    SMB2_NegotiateResponse_t *rsp = (SMB2_NegotiateResponse_t *)SMB2_buf.buf;
    struct in_addr dst_addr;
    iop_sema_t smp;
    int r, reqlen, i;

    smp.initial = 1;
    smp.max = 1;
    smp.option = 0;
    smp.attr = 1;
    smb_io_sema = CreateSema(&smp);

    for (i = 0; i < SMB2_MAX_HANDLES; i++)
        handle_table[i].used = 0;

    session_id_low = session_id_high = 0;
    tree_id = 0;
    message_id_low = message_id_high = 0;
    smb2_dialect = 0;
    stored_hash_callback = hash_callback;

    dst_addr.s_addr = pinet_addr(SMBServerIP);
    main_socket = OpenTCPSession(dst_addr, SMBServerPort);
    if (main_socket < 0)
        return -SMB2_ERR_TRANSPORT;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_NegotiateRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_NEGOTIATE);

    req->StructureSize = 36;
    req->DialectCount = 3;
    req->SecurityMode = SMB2_NEGOTIATE_SIGNING_ENABLED; // capable of signing, but do not require it
    req->Capabilities = 0;
    // Offer 2.0.2 / 2.1 / 3.0. We cannot actually satisfy 3.x yet (it mandates signing), but a
    // server that picks 3.0 is caught by the SIGNING_REQUIRED check below and reported cleanly.
    req->Dialects[0] = SMB2_DIALECT_202;
    req->Dialects[1] = SMB2_DIALECT_210;
    req->Dialects[2] = SMB2_DIALECT_300;

    // Fixed part is 36 bytes; Dialects[] is variable, so size the packet by the count actually sent
    // rather than by sizeof(the struct), whose array is padded to its 4-entry maximum.
    reqlen = SMB2_HDR_SIZE + 36 + (req->DialectCount * sizeof(u16));

    r = smb2_Exchange(reqlen, 0);
    if (r < 0)
        return r;

    r = smb2_CheckReply(SMB2_NEGOTIATE);
    if (r < 0)
        return r;

    if (rsp->SecurityMode & SMB2_NEGOTIATE_SIGNING_REQUIRED)
        return -SMB2_ERR_SIGNING_REQUIRED;

    smb2_dialect = rsp->DialectRevision;
    if (smb2_dialect != SMB2_DIALECT_202 && smb2_dialect != SMB2_DIALECT_210)
        return -SMB2_ERR_NO_DIALECT;

    /*
      Seed server_specs for the hash callback. SMB1 can fill EncryptionKey here because the
      challenge rides in its NEGOTIATE reply; under SMB2 the challenge does not arrive until the
      NTLMSSP CHALLENGE inside SESSION_SETUP, so the callback is deferred to smb2_SessionSetup().
    */
    memset(&server_specs, 0, sizeof(server_specs));
    server_specs.MaxBufferSize = rsp->MaxReadSize;
    server_specs.SecurityMode = SERVER_USER_SECURITY_LEVEL;
    server_specs.PasswordType = SERVER_USE_ENCRYPTED_PASSWORD;
    strncpy(server_specs.Username, Username, sizeof(server_specs.Username) - 1);
    strncpy(server_specs.Password, Password, sizeof(server_specs.Password) - 1);
    server_specs.IOPaddr = (void *)&server_specs;
    server_specs.HashedFlag = 0;

    return 1;
}

//-------------------------------------------------------------------------
int smb2_SessionSetup(void)
{
    SMB2_SessionSetupRequest_t *req = (SMB2_SessionSetupRequest_t *)SMB2_buf.buf;
    SMB2_SessionSetupResponse_t *rsp = (SMB2_SessionSetupResponse_t *)SMB2_buf.buf;
    SMB2_Header_t *hdr = (SMB2_Header_t *)SMB2_buf.buf;
    u8 challenge[8];
    int r, blob_len, reqlen, sec_off, sec_len;

    // --- Round 1: send NTLMSSP NEGOTIATE, expect CHALLENGE -------------------------------------
    ZERO_PKT_ALIGNED(req, sizeof(SMB2_SessionSetupRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_SESSION_SETUP);

    req->StructureSize = 25;
    req->Flags = 0;
    req->SecurityMode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->Capabilities = 0;
    req->Channel = 0;
    req->SecurityBufferOffset = SMB2_HDR_SIZE + 24; // security blob follows the 24-byte fixed part
    blob_len = ntlmssp_BuildNegotiate(&req->Buffer[0]);
    req->SecurityBufferLength = blob_len;

    reqlen = SMB2_HDR_SIZE + 24 + blob_len;

    r = smb2_Exchange(reqlen, 0);
    if (r < 0)
        return r;

    if (hdr->ProtocolId != SMB2_MAGIC || hdr->Command != SMB2_SESSION_SETUP)
        return -SMB2_ERR_PROTOCOL;

    // The server MUST answer round 1 with MORE_PROCESSING_REQUIRED; anything else is a failure.
    if (hdr->Status != STATUS_MORE_PROCESSING_REQUIRED) {
        if (hdr->Status == STATUS_LOGON_FAILURE_NT)
            return -SMB2_ERR_LOGON_FAILURE;
        return -SMB2_ERR_PROTOCOL;
    }

    // SessionId is assigned now and must be echoed on every later request, including round 2.
    session_id_low = hdr->SessionIdLow;
    session_id_high = hdr->SessionIdHigh;

    sec_off = rsp->SecurityBufferOffset;
    sec_len = rsp->SecurityBufferLength;
    // Offsets are server-supplied: bounds-check before dereferencing into our own buffer.
    if (sec_off < SMB2_HDR_SIZE || sec_len < 0 || sec_off + sec_len > (int)sizeof(SMB2_buf.buf))
        return -SMB2_ERR_PROTOCOL;

    r = ntlmssp_ParseChallenge(SMB2_buf.buf + sec_off, sec_len, challenge);
    if (r < 0)
        return r;

    /*
      Hand the challenge to smbinit, which owns DES/MD4 and rewrites server_specs.Password in place
      into {LM response, NTLM response}. Same contract SMB1 uses -- just invoked a round later.
    */
    memcpy(server_specs.EncryptionKey, challenge, 8);
    server_specs.HashedFlag = 0;
    stored_hash_callback(&server_specs);

    // --- Round 2: send NTLMSSP AUTHENTICATE, expect success ------------------------------------
    ZERO_PKT_ALIGNED(req, sizeof(SMB2_SessionSetupRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_SESSION_SETUP);

    req->StructureSize = 25;
    req->Flags = 0;
    req->SecurityMode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->Capabilities = 0;
    req->Channel = 0;
    req->SecurityBufferOffset = SMB2_HDR_SIZE + 24;

    blob_len = ntlmssp_BuildAuthenticate(&req->Buffer[0], sizeof(SMB2_buf.buf) - (SMB2_HDR_SIZE + 24));
    if (blob_len < 0)
        return blob_len;
    req->SecurityBufferLength = blob_len;

    reqlen = SMB2_HDR_SIZE + 24 + blob_len;

    r = smb2_Exchange(reqlen, 0);
    if (r < 0)
        return r;

    if (hdr->ProtocolId != SMB2_MAGIC || hdr->Command != SMB2_SESSION_SETUP)
        return -SMB2_ERR_PROTOCOL;
    if (hdr->Status == STATUS_LOGON_FAILURE_NT)
        return -SMB2_ERR_LOGON_FAILURE;
    if (hdr->Status != STATUS_SUCCESS)
        return -SMB2_ERR_PROTOCOL;

    return 1;
}

//-------------------------------------------------------------------------
int smb2_TreeConnect(char *ShareName)
{
    SMB2_TreeConnectRequest_t *req = (SMB2_TreeConnectRequest_t *)SMB2_buf.buf;
    SMB2_Header_t *hdr = (SMB2_Header_t *)SMB2_buf.buf;
    int r, path_len, reqlen;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_TreeConnectRequest_t));
    // Tree id is per-connect; clear it so the request does not carry a stale one.
    tree_id = 0;
    smb2_SetHeader(&req->hdr, SMB2_TREE_CONNECT);

    req->StructureSize = 9;
    req->PathOffset = SMB2_HDR_SIZE + 8;

    // \\server\share as counted UTF-16LE, no terminator.
    path_len = asciiToUtf16(&req->Buffer[0], ShareName);
    req->PathLength = path_len;

    reqlen = SMB2_HDR_SIZE + 8 + path_len;

    r = smb2_Exchange(reqlen, 0);
    if (r < 0)
        return r;

    r = smb2_CheckReply(SMB2_TREE_CONNECT);
    if (r < 0)
        return r;

    tree_id = hdr->TreeId;

    return 1;
}

//-------------------------------------------------------------------------
static int smb2_AllocHandle(void)
{
    int i;

    for (i = 0; i < SMB2_MAX_HANDLES; i++) {
        if (!handle_table[i].used)
            return i;
    }

    return -1;
}

int smb2_Open(char *filename, u16 *FID, int Write)
{
    SMB2_CreateRequest_t *req = (SMB2_CreateRequest_t *)SMB2_buf.buf;
    SMB2_CreateResponse_t *rsp = (SMB2_CreateResponse_t *)SMB2_buf.buf;
    int r, name_len, reqlen, slot;
    const char *name;

    WaitSema(smb_io_sema);

    slot = smb2_AllocHandle();
    if (slot < 0) {
        SignalSema(smb_io_sema);
        return -SMB2_ERR_HANDLES_EXHAUSTED;
    }

    /*
      SMB1's OPEN_ANDX takes a share-absolute path with a leading backslash ("\CD\FOO.ISO"), and
      that is the shape every caller in cdvdman builds. SMB2's CREATE Name is relative to the share
      root and MUST NOT have one, so strip it here rather than touching the call sites -- keeping
      device-smb.c dialect-agnostic is the whole point of the dispatcher.
    */
    name = filename;
    while (*name == '\\')
        name++;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_CreateRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_CREATE);

    req->StructureSize = 57;
    req->RequestedOplockLevel = 0;
    req->ImpersonationLevel = SMB2_IMPERSONATION_IMPERSONATION;
    req->DesiredAccess = Write ? (SMB2_FILE_READ_DATA | SMB2_FILE_WRITE_DATA | SMB2_FILE_READ_ATTR) : (SMB2_FILE_READ_DATA | SMB2_FILE_READ_ATTR);
    req->FileAttributes = 0;
    req->ShareAccess = Write ? (SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE) : SMB2_FILE_SHARE_READ;
    req->CreateDisposition = SMB2_FILE_OPEN; // never create: the ISO/VMC must already exist
    req->CreateOptions = SMB2_FILE_NON_DIRECTORY_FILE;
    req->NameOffset = SMB2_HDR_SIZE + 56;

    name_len = asciiToUtf16(&req->Buffer[0], name);
    req->NameLength = name_len;
    req->CreateContextsOffset = 0;
    req->CreateContextsLength = 0;

    reqlen = SMB2_HDR_SIZE + 56 + name_len;

    r = smb2_Exchange(reqlen, 0);
    if (r < 0) {
        SignalSema(smb_io_sema);
        return r;
    }

    r = smb2_CheckReply(SMB2_CREATE);
    if (r < 0) {
        SignalSema(smb_io_sema);
        return r;
    }

    memcpy(&handle_table[slot].fid, &rsp->FileId, sizeof(SMB2_FileId_t));
    handle_table[slot].used = 1;
    *FID = (u16)slot;

    SignalSema(smb_io_sema);

    return 1;
}

//-------------------------------------------------------------------------
// Mirrors smb_Close(): the caller already holds smb_io_sema, so do not take it here.
int smb2_Close(int FID)
{
    SMB2_CloseRequest_t *req = (SMB2_CloseRequest_t *)SMB2_buf.buf;
    int r;

    if (FID < 0 || FID >= SMB2_MAX_HANDLES || !handle_table[FID].used)
        return -SMB2_ERR_BAD_HANDLE;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_CloseRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_CLOSE);

    req->StructureSize = 24;
    req->Flags = 0;
    memcpy(&req->FileId, &handle_table[FID].fid, sizeof(SMB2_FileId_t));

    // Free the slot regardless of how the server answers: the handle is gone from our side either
    // way, and leaking slots would exhaust the table across a retry.
    handle_table[FID].used = 0;

    r = smb2_Exchange(SMB2_HDR_SIZE + 24, 0);
    if (r < 0)
        return -EIO;

    if (smb2_CheckReply(SMB2_CLOSE) < 0)
        return -EIO;

    return 0;
}

//-------------------------------------------------------------------------
/*
  One READ round-trip.

  smb.c has a hand-rolled fast path built on the custom recvfrom() in smsutils, which splits the
  reply between a header buffer and the caller's buffer in one call. That trick hard-codes SMB1's
  header geometry (it assumes data begins at a fixed offset), so it does not carry over to SMB2's
  64-byte header as-is. This uses the plain, obviously-correct path instead: read the fixed
  response, skip to DataOffset, then stream the payload straight into the caller's buffer. Porting
  the fast path is a worthwhile follow-up once this is validated on hardware -- but not before,
  because getting it subtly wrong shows up as corrupted sectors mid-game rather than a clean error.
*/
/*
  Consume `remaining` unread bytes of the current reply, then hand back `rv`.

  Used by the READ path so an error return still leaves the socket positioned at the start of the
  next reply. Discards into SMB2_buf in chunks, since the bytes are by definition unwanted.
*/
static int smb2_DrainAndFail(int remaining, int rv)
{
    int chunk, r;

    while (remaining > 0) {
        chunk = (remaining > (int)sizeof(SMB2_buf.buf)) ? (int)sizeof(SMB2_buf.buf) : remaining;
        r = RecvData(main_socket, (char *)SMB2_buf.buf, chunk);
        if (r <= 0)
            return -SMB2_ERR_TRANSPORT; // connection is gone; nothing left to resynchronise
        remaining -= chunk;
    }

    return rv;
}

static int smb2_ReadOnce(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes)
{
    SMB2_ReadRequest_t *req = (SMB2_ReadRequest_t *)SMB2_buf.buf;
    SMB2_ReadResponse_t *rsp = (SMB2_ReadResponse_t *)SMB2_buf.buf;
    int r, padding, DataLength, totalpkt, consumed, remaining;

    if (FID >= SMB2_MAX_HANDLES || !handle_table[FID].used)
        return -SMB2_ERR_BAD_HANDLE;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_ReadRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_READ);

    req->StructureSize = 49;
    req->Padding = SMB2_HDR_SIZE + 16; // conventional: where the server should place the data
    req->Flags = 0;
    req->Length = nbytes;
    req->OffsetLow = offsetlow;
    req->OffsetHigh = offsethigh;
    memcpy(&req->FileId, &handle_table[FID].fid, sizeof(SMB2_FileId_t));
    req->MinimumCount = 0;
    req->Channel = 0;
    req->RemainingBytes = 0;

    // Read header + the fixed 16-byte response body (80 bytes total) only; the payload is drained
    // straight into the caller's buffer below rather than through SMB2_buf.
    r = smb2_Exchange(SMB2_HDR_SIZE + 48 + 1, sizeof(SMB2_ReadResponse_t));
    if (r < 0)
        return r;

    /*
      Everything past this point must leave the socket byte-aligned with the stream, because the
      next request reuses the same connection. smb2_Exchange consumed min(80, totalpkt) bytes; the
      rest of this reply is still queued. Bailing out without consuming it would desynchronise every
      later exchange -- which surfaces as garbage sectors, not as an error. So each failure path
      below drains first via `remaining`.
    */
    totalpkt = r;
    consumed = (totalpkt < (int)sizeof(SMB2_ReadResponse_t)) ? totalpkt : (int)sizeof(SMB2_ReadResponse_t);
    remaining = totalpkt - consumed;

    if (smb2_CheckReply(SMB2_READ) < 0)
        return smb2_DrainAndFail(remaining, -EIO);

    DataLength = (int)rsp->DataLength;
    if (DataLength < 0 || DataLength > nbytes || DataLength > remaining)
        return smb2_DrainAndFail(remaining, -EIO);

    // Skip any gap between the end of the fixed response and DataOffset.
    padding = (int)rsp->DataOffset - (int)sizeof(SMB2_ReadResponse_t);
    if (padding < 0 || padding > remaining - DataLength)
        return smb2_DrainAndFail(remaining, -EIO);

    if (padding > 0) {
        // Drain into the tail of our own buffer; it is scratch at this point.
        if (padding > (int)sizeof(SMB2_buf.buf) - (int)sizeof(SMB2_ReadResponse_t))
            return smb2_DrainAndFail(remaining, -EIO);
        r = RecvData(main_socket, (char *)(SMB2_buf.buf + sizeof(SMB2_ReadResponse_t)), padding);
        if (r <= 0)
            return -SMB2_ERR_TRANSPORT;
        remaining -= padding;
    }

    if (DataLength > 0) {
        r = RecvData(main_socket, readbuf, DataLength);
        if (r <= 0)
            return -SMB2_ERR_TRANSPORT;
        remaining -= DataLength;
    }

    // Any trailer the server appended past the data (none in practice, but the length field is the
    // server's to choose) still has to come off the socket.
    if (remaining > 0)
        smb2_DrainAndFail(remaining, 0);

    return DataLength;
}

int smb2_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes)
{
    int result, remaining, toRead;
    char *ptr;

    remaining = nbytes;
    ptr = readbuf;

    WaitSema(smb_io_sema);

    while (remaining > 0) {
        toRead = remaining > SMB2_MAX_RECV_SIZE ? SMB2_MAX_RECV_SIZE : remaining;

        result = smb2_ReadOnce(FID, offsetlow, offsethigh, ptr, toRead);
        if (result <= 0) {
            // smb.c leaks the semaphore on this path; do not copy that. A read error mid-game must
            // not also deadlock every later access behind DeviceLock().
            SignalSema(smb_io_sema);
            return result;
        }

        if (offsetlow + result < offsetlow)
            offsethigh++;
        offsetlow += result;
        ptr += result;
        remaining -= result;
    }

    SignalSema(smb_io_sema);

    return nbytes;
}

//-------------------------------------------------------------------------
static int smb2_WriteOnce(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes)
{
    SMB2_WriteRequest_t *req = (SMB2_WriteRequest_t *)SMB2_buf.buf;
    SMB2_WriteResponse_t *rsp = (SMB2_WriteResponse_t *)SMB2_buf.buf;
    int r;

    if (FID >= SMB2_MAX_HANDLES || !handle_table[FID].used)
        return -SMB2_ERR_BAD_HANDLE;

    ZERO_PKT_ALIGNED(req, sizeof(SMB2_WriteRequest_t));
    smb2_SetHeader(&req->hdr, SMB2_WRITE);

    req->StructureSize = 49;
    req->DataOffset = SMB2_HDR_SIZE + 48;
    req->Length = nbytes;
    req->OffsetLow = offsetlow;
    req->OffsetHigh = offsethigh;
    memcpy(&req->FileId, &handle_table[FID].fid, sizeof(SMB2_FileId_t));
    req->Channel = 0;
    req->RemainingBytes = 0;
    req->Flags = 0;

    /*
      Send header+fixed body first, then the payload, so the caller's buffer is never copied into
      SMB2_buf (which is only 1KB and could not hold a VMC page anyway). Same two-part send smb.c
      uses for WRITE_ANDX.
    */
    nb_SetSessionMessage(SMB2_HDR_SIZE + 48 + nbytes);

    r = SendData(main_socket, (char *)&SMB2_buf, 4 + SMB2_HDR_SIZE + 48);
    if (r <= 0)
        return -SMB2_ERR_TRANSPORT;

    r = SendData(main_socket, writebuf, nbytes);
    if (r <= 0)
        return -SMB2_ERR_TRANSPORT;

    do {
        r = RecvData(main_socket, (char *)&SMB2_buf.sessionHeader, sizeof(SMB2_buf.sessionHeader));
        if (r <= 0)
            return -SMB2_ERR_TRANSPORT;
    } while (nb_GetPacketType() != 0);

    r = nb_GetSessionMessageLength();
    if (r > (int)sizeof(SMB2_buf.buf))
        return -SMB2_ERR_PROTOCOL;

    r = RecvData(main_socket, (char *)SMB2_buf.buf, r);
    if (r <= 0)
        return -SMB2_ERR_TRANSPORT;

    if (smb2_CheckReply(SMB2_WRITE) < 0)
        return -EIO;

    return (int)rsp->Count;
}

int smb2_WriteFile(u16 FID, u32 offsetlow, u32 offsethigh, void *writebuf, int nbytes)
{
    int result, remaining, toWrite;
    char *ptr;

    remaining = nbytes;
    ptr = writebuf;

    WaitSema(smb_io_sema);

    while (remaining > 0) {
        toWrite = remaining > SMB2_MAX_XMIT_SIZE ? SMB2_MAX_XMIT_SIZE : remaining;

        result = smb2_WriteOnce(FID, offsetlow, offsethigh, ptr, toWrite);
        if (result <= 0) {
            SignalSema(smb_io_sema);
            return result;
        }

        if (offsetlow + result < offsetlow)
            offsethigh++;
        offsetlow += result;
        ptr += result;
        remaining -= result;
    }

    SignalSema(smb_io_sema);

    return nbytes;
}

//-------------------------------------------------------------------------
int smb2_Disconnect(void)
{
    if (main_socket >= 0) {
        plwip_close(main_socket);
        main_socket = -1;
    }

    return 1;
}
