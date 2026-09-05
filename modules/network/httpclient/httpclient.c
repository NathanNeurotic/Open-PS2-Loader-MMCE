/*  Simple HTTP client for retrieving files.
    Support for the "chunked" encoding has not been implemented.    */

#include <stdio.h>
#include <errno.h>
#include <sysclib.h>
#include <ps2ip.h>

#include "httpclient.h"

void HttpCloseConnection(s32 HttpSocket)
{
    shutdown(HttpSocket, SHUT_RDWR);
    closesocket(HttpSocket);
}

static int EstablishConnection(struct in_addr *server, unsigned short int port)
{
    struct sockaddr_in SockAddr;
    int HostSocket;

    HostSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    SockAddr.sin_family = AF_INET;
    SockAddr.sin_addr.s_addr = server->s_addr;
    SockAddr.sin_port = htons(port);
    if (connect(HostSocket, (struct sockaddr *)&SockAddr, sizeof(SockAddr)) != 0) {
        HttpCloseConnection(HostSocket);
        HostSocket = -1;
    }

    return HostSocket;
}

static int SendData(int socket, char *buffer, int length)
{
    char *pointer;
    int remaining, result;

    for (remaining = length, pointer = buffer; remaining > 0; remaining -= result, pointer += result) {
        if ((result = send(socket, pointer, remaining, 0)) < 1)
            break;
    }

    return length - remaining;
}

static int GetData(int socket, char *buffer, int length)
{
    struct timeval timeout;
    fd_set readfds;
    char *pointer;
    int remaining, ToRead, result;

    for (remaining = length, pointer = buffer; remaining > 0;) {
        ToRead = remaining;

        // This safeguards against a deadlock, if the TCP connection gets broken for long enough. Long enough for the RST packet from the other side gets lost.
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        if (select(socket + 1, &readfds, NULL, NULL, &timeout) <= 0) {
            break;
        }

        if ((result = recv(socket, pointer, ToRead, 0)) < 1)
            break;
        remaining -= result;
        pointer += result;
        if (result != ToRead) // No further data at the moment.
            break;
    }

    return length - remaining;
}

#define HTTP_WORK_BUFFER_SIZE 256 // Not a really great design, but this must be long enough for the longest line in the HTTP entity.

enum TRANFER_ENCODING {
    TRANFER_ENCODING_PLAIN,
    TRANFER_ENCODING_CHUNKED
};

static int ContentLength;
static short int StatusCode;
static unsigned short int HeaderLineNumber;
// static char TransferEncoding;
static char ConnectionMode;

static void HttpParseEntityLine(const char *line)
{
    char *pColon;

    if ((pColon = strchr(line, ':')) != NULL) {
        for (pColon++; *pColon != '\0'; pColon++) {
            if (look_ctype_table(*pColon) & 2)
                *pColon = tolower(*pColon);
        }
    }

    // printf("%u\t%s\n", HeaderLineNumber, line);

    if (HeaderLineNumber == 0 && strncmp(line, "HTTP/1.1 ", 9) == 0)
        StatusCode = strtoul(line + 9, NULL, 10);

    if (strncmp(line, "Content-Length: ", 16) == 0)
        ContentLength = strtoul(line + 15, NULL, 10);
    /*
    if (strncmp(line, "Transfer-Encoding: ", 19) == 0) {
        ContentLength = -1;
        if (strcmp(line + 19, "chunked") == 0)
            TransferEncoding = TRANFER_ENCODING_CHUNKED;
    }
    */
    if (strcmp(line, "Connection: close") == 0)
        ConnectionMode = HTTP_CMODE_CLOSED;

    HeaderLineNumber++;
}

static int HttpGetResponse(s32 socket, s8 *mode, char *buffer, u16 *length)
{
    char work[HTTP_WORK_BUFFER_SIZE + 1], EndOfEntity, *ptr, *next_ptr, *PayloadPtr;
    int result, DataAvailable, PayloadAmount;

    // TransferEncoding = TRANFER_ENCODING_PLAIN;
    ConnectionMode = *mode;
    ContentLength = -1;
    StatusCode = -1;
    HeaderLineNumber = 0;
    PayloadPtr = buffer;
    PayloadAmount = 0;
    EndOfEntity = 0;
    ptr = work;
    DataAvailable = 0;
    work[HTTP_WORK_BUFFER_SIZE] = '\0';
    do {
        if ((result = GetData(socket, ptr, HTTP_WORK_BUFFER_SIZE - DataAvailable)) > 0) {
            DataAvailable += result;
            ptr = work;
            while ((next_ptr = strstr(ptr, "\r\n")) != NULL) {
                *next_ptr = '\0';
                // Parse line
                HttpParseEntityLine(ptr);
                DataAvailable -= (next_ptr + 2 - ptr);
                ptr = next_ptr + 2; // skip CRLN
                if (strncmp(ptr, "\r\n", 2) == 0) {
                    EndOfEntity = 1;
                    DataAvailable -= 2;
                    ptr += 2;
                    break;
                }
            }

            // At this point, the final line (preceding NULL terminator) has been reached. Move any outstanding characters to the start of the buffer.
            if (!EndOfEntity) {
                if (DataAvailable > 0) {
                    memmove(work, ptr, DataAvailable);
                    work[DataAvailable] = '\0';
                    ptr = &work[DataAvailable];
                } else
                    ptr = work;
            }
        } else {
            // No more data. Connection lost?
            //             printf("DEBUG: connection lost?\n");
            break;
        }
    } while (!EndOfEntity);

    // Receive data normally (plain).
    if (DataAvailable > 0) // Move leftover data into the output buffer.
    {
        memcpy(buffer, ptr, DataAvailable);
        buffer[DataAvailable] = '\0';
        PayloadPtr = buffer + DataAvailable;
        PayloadAmount = DataAvailable;
    }

    if (ContentLength < 0 || PayloadAmount < ContentLength) {
        if (ContentLength > *length)
            ContentLength = *length;
        if ((result = GetData(socket, PayloadPtr, ContentLength - PayloadAmount)) > 0)
            *length = PayloadAmount + result;
        else
            *length = PayloadAmount;
    } else
        *length = PayloadAmount;

    result = StatusCode;
    if (ContentLength > 0 && ContentLength > *length) {
        result = -EPIPE; // Incomplete transfer.
                         // printf("Pipe broken: %d/%d\n", *length, ContentLength);
    }

    *mode = ConnectionMode;

    return result;
}

static int ResolveHostname(char *hostname, struct in_addr *ip)
{
    struct hostent *HostEntry;
    struct in_addr **addr_list;

    if ((HostEntry = gethostbyname(hostname)) == NULL)
        return 1;

    for (addr_list = (struct in_addr **)HostEntry->h_addr_list; addr_list != NULL; addr_list++) {
        ip->s_addr = (*addr_list)->s_addr;
        return 0;
    }

    return 1;
}

int HttpEstabConnection(char *server, u16 port)
{
    struct in_addr ip;
    int result;

    if (ResolveHostname(server, &ip) == 0) {
        result = EstablishConnection(&ip, port);
    } else {
        result = -ENXIO;
    }

    return result;
}

static const char *GetDayInWeek(const unsigned char *mtime)
{
    static const unsigned char daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const char *dayLabels[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    unsigned short int DaysInYear;
    unsigned char LeapDays, month;
    unsigned int days;

    LeapDays = mtime[0] / 4; // Number of leap days, in the years elasped.
    for (month = 0, DaysInYear = 0; month < mtime[1]; DaysInYear += daysInMonth[month], month++)
        ; // Number of days, within the months elasped within the past year.
    if (mtime[0] % 4 == 0) {
        if (mtime[1] > 1)
            DaysInYear++; // Account for this year's leap day, if applicable.
    } else
        LeapDays++; // Account for the leap day, of the leap year that just passed.
    days = mtime[0] * 365 + LeapDays + DaysInYear + mtime[2];

    return dayLabels[(5 + days) % 7]; // 2000/1/1 was a Saturday (5).
}

int HttpSendGetRequest(s32 HttpSocket, const char *UserAgent, const char *host, s8 *mode, const u8 *mtime, const char *uri, char *output, u16 *out_len)
{
    const char *months[] = {
        "Jan",
        "Feb",
        "Mar",
        "Apr",
        "May",
        "Jun",
        "Jul",
        "Aug",
        "Sep",
        "Oct",
        "Nov",
        "Dec"};
    char buffer[512];
    int result, length;

    sprintf(buffer, "GET %s HTTP/1.1\r\n"
                    "Accept: text/html, */*\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n",
            uri, UserAgent, host);

    if (*mode == HTTP_CMODE_PERSISTENT)
        strcat(buffer, "Proxy-Connection: Keep-Alive\r\n");
    if (mtime != NULL)
        sprintf(&buffer[strlen(buffer)], "If-Modified-Since: %s, %02u %s %04u %02u:%02u:%02u GMT\r\n", GetDayInWeek(mtime), mtime[2] + 1, months[mtime[1]], 2000 + mtime[0], mtime[3], mtime[4], mtime[5]);
    strcat(buffer, "\r\n");

    length = strlen(buffer);

    if (SendData(HttpSocket, buffer, length) == length) {
        result = HttpGetResponse(HttpSocket, mode, output, out_len);
    } else {
        result = -1;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Streaming GET, optionally ranged.
//
// Separate from everything above. HttpGetResponse()'s body read stops at the first short TCP read
// ("No further data at the moment"), which is invisible for the sub-512-byte compatibility files
// it was written for and wrong for anything that spans segments -- a catalog, a disc sector. This
// path loops until Content-Length is satisfied or the peer stops, and says which of those happened.
// ---------------------------------------------------------------------------

#define HTTP_HDR_MAX 1024

static struct
{
    int socket;
    int live;
    int remaining; // body bytes still owed by Content-Length; -1 when the server did not say
    int stageLen;
    int stageOff;
} gStream;

static char gStage[HTTP_CLIENT_STREAM_CHUNK];

// sysclib's sprintf has no %llu, and a DVD9 offset does not fit in 32 bits. Writes the decimal
// form of `value` at `out` and returns its length.
//
// The IOP links no 64-bit division runtime -- a plain `value % 10` on a u64 pulls in __umoddi3 and
// __udivdi3 and the module simply fails to link. So this is long division in base 2^16, using only
// 32-bit divides. Each step's dividend is (rem << 16) | part with rem < 10, so it is at most
// 655359 and its quotient at most 65535: both fit a u32, and the digit is exact.
static int u64ToDec(u64 value, char *out)
{
    u32 parts[4];
    char tmp[24];
    int i, j = 0, n = 0;

    parts[0] = (u32)((value >> 48) & 0xFFFF);
    parts[1] = (u32)((value >> 32) & 0xFFFF);
    parts[2] = (u32)((value >> 16) & 0xFFFF);
    parts[3] = (u32)(value & 0xFFFF);

    do {
        u32 rem = 0;
        int nonzero = 0;

        for (i = 0; i < 4; i++) {
            u32 cur = (rem << 16) | parts[i];
            parts[i] = cur / 10;
            rem = cur % 10;
            if (parts[i] != 0)
                nonzero = 1;
        }
        tmp[n++] = (char)('0' + (int)rem);
        if (!nonzero)
            break;
    } while (n < 20);

    while (n > 0)
        out[j++] = tmp[--n];
    out[j] = '\0';

    return j;
}

// Likewise multiply-free of anything libgcc has to supply: v*10 as (v<<3)+(v<<1), which the
// compiler expands inline on a 32-bit target.
static u64 DecToU64(const char *s, const char **end)
{
    u64 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = (v << 3) + (v << 1) + (u64)(u32)(*s - '0');
        s++;
    }
    if (end != NULL)
        *end = s;

    return v;
}

static char LowerChar(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Header names are case-insensitive (RFC 7230). Compares `prefix` against the head of `line`.
static int HeaderIs(const char *line, const char *prefix)
{
    while (*prefix != '\0') {
        if (LowerChar(*line) != LowerChar(*prefix))
            return 0;
        line++;
        prefix++;
    }

    return 1;
}

static const char *SkipBlanks(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;

    return p;
}

// One bounded recv. Ten seconds, matching GetData()'s existing guard against a peer that went away
// without its RST arriving.
static int RecvBounded(int socket, void *buffer, int length)
{
    struct timeval timeout;
    fd_set readfds;

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    FD_ZERO(&readfds);
    FD_SET(socket, &readfds);
    if (select(socket + 1, &readfds, NULL, NULL, &timeout) <= 0)
        return -1;

    return recv(socket, buffer, length, 0);
}

void HttpStreamEnd(void)
{
    gStream.live = 0;
    gStream.socket = -1;
    gStream.remaining = 0;
    gStream.stageLen = 0;
    gStream.stageOff = 0;
}

int HttpStreamBegin(s32 HttpSocket, const char *host, const char *uri,
                    int useRange, u64 rangeStart, u64 rangeEnd, int keepAlive,
                    int *contentLen, u64 *total, int *hasTotal)
{
    char request[HTTP_CLIENT_STREAM_URI_MAX + 256];
    char hdr[HTTP_HDR_MAX + 1];
    int length, hlen, hdrEnd, i, status;
    int sawChunked = 0, sawEncoding = 0, sawMultipart = 0;
    int bodyLen = -1;
    u64 rangeFirst = 0, rangeLast = 0, rangeTotal = 0;
    int sawRange = 0, sawRangeTotal = 0;

    HttpStreamEnd();

    *contentLen = -1;
    *total = 0;
    *hasTotal = 0;

    // Accept-Encoding: identity because this profile does not implement any content coding, and a
    // server that honours it will not send one.
    strcpy(request, "GET ");
    strcat(request, uri);
    strcat(request, " HTTP/1.1\r\nHost: ");
    strcat(request, host);
    strcat(request, "\r\nUser-Agent: RiptOPL\r\nAccept: */*\r\nAccept-Encoding: identity\r\n");
    if (useRange) {
        length = strlen(request);
        strcpy(&request[length], "Range: bytes=");
        length += 13;
        length += u64ToDec(rangeStart, &request[length]);
        request[length++] = '-';
        length += u64ToDec(rangeEnd, &request[length]);
        request[length++] = '\r';
        request[length++] = '\n';
        request[length] = '\0';
    }
    strcat(request, keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");

    length = strlen(request);
    if (SendData(HttpSocket, request, length) != length)
        return HTTP_STREAM_ERR_SEND;

    // Read until the end of the header block. Scanned byte by byte rather than with strstr because
    // the same read can already hold binary body bytes, and those can contain a NUL.
    hlen = 0;
    hdrEnd = -1;
    while (hlen < HTTP_HDR_MAX) {
        int r = RecvBounded(HttpSocket, &hdr[hlen], HTTP_HDR_MAX - hlen);
        if (r <= 0)
            break;
        hlen += r;
        for (i = (hlen - r >= 3) ? hlen - r - 3 : 0; i + 3 < hlen; i++) {
            if (hdr[i] == '\r' && hdr[i + 1] == '\n' && hdr[i + 2] == '\r' && hdr[i + 3] == '\n') {
                hdrEnd = i + 4;
                break;
            }
        }
        if (hdrEnd >= 0)
            break;
    }
    if (hdrEnd < 0)
        return HTTP_STREAM_ERR_HEADERS;

    // Status line.
    if (hdrEnd < 12 || !HeaderIs(hdr, "HTTP/1."))
        return HTTP_STREAM_ERR_STATUS;
    status = (int)DecToU64(SkipBlanks(&hdr[8]), NULL);
    if (status <= 0)
        return HTTP_STREAM_ERR_STATUS;

    // Header lines. Terminating each in place is safe: everything up to hdrEnd is the header block.
    i = 0;
    while (i < hdrEnd) {
        int start = i;
        while (i + 1 < hdrEnd && !(hdr[i] == '\r' && hdr[i + 1] == '\n'))
            i++;
        hdr[i] = '\0';
        if (start != 0) { // skip the status line
            const char *line = &hdr[start];
            if (HeaderIs(line, "Content-Length:")) {
                bodyLen = (int)DecToU64(SkipBlanks(line + 15), NULL);
            } else if (HeaderIs(line, "Transfer-Encoding:")) {
                const char *v = SkipBlanks(line + 18);
                if (HeaderIs(v, "chunked"))
                    sawChunked = 1;
            } else if (HeaderIs(line, "Content-Encoding:")) {
                const char *v = SkipBlanks(line + 17);
                if (!HeaderIs(v, "identity"))
                    sawEncoding = 1;
            } else if (HeaderIs(line, "Content-Type:")) {
                const char *v = SkipBlanks(line + 13);
                if (HeaderIs(v, "multipart/"))
                    sawMultipart = 1;
            } else if (HeaderIs(line, "Content-Range:")) {
                const char *v = SkipBlanks(line + 14);
                if (HeaderIs(v, "bytes")) {
                    v = SkipBlanks(v + 5);
                    rangeFirst = DecToU64(v, &v);
                    if (*v == '-') {
                        v++;
                        rangeLast = DecToU64(v, &v);
                        sawRange = 1;
                        if (*v == '/') {
                            v++;
                            if (*v != '*') {
                                rangeTotal = DecToU64(v, NULL);
                                sawRangeTotal = 1;
                            }
                        }
                    }
                }
            }
        }
        i += 2; // step over the CRLF we just split on
    }

    // Nothing below is implemented, and guessing at a body we cannot frame is how wrong bytes reach
    // a sector buffer. Refuse cleanly instead.
    if (sawChunked || sawEncoding || sawMultipart)
        return HTTP_STREAM_ERR_ENCODING;

    // A 206 must be the range that was asked for. Without this check a server that answers a
    // different interval -- or the same interval shifted -- is indistinguishable from a correct one.
    if (useRange && status == 206) {
        if (!sawRange || rangeFirst != rangeStart || rangeLast != rangeEnd)
            return HTTP_STREAM_ERR_RANGE;
    }

    if (sawRangeTotal) {
        *total = rangeTotal;
        *hasTotal = 1;
    }
    *contentLen = bodyLen;

    gStream.socket = HttpSocket;
    gStream.live = 1;
    gStream.remaining = bodyLen;
    gStream.stageOff = 0;
    gStream.stageLen = hlen - hdrEnd;
    if (gStream.stageLen > HTTP_CLIENT_STREAM_CHUNK)
        gStream.stageLen = HTTP_CLIENT_STREAM_CHUNK;
    if (gStream.stageLen > 0)
        memcpy(gStage, &hdr[hdrEnd], gStream.stageLen);

    return status;
}

int HttpStreamRead(void *buffer, int wanted)
{
    char *out = (char *)buffer;
    int done = 0;

    if (!gStream.live)
        return HTTP_STREAM_ERR_NOSTREAM;
    if (wanted > HTTP_CLIENT_STREAM_CHUNK)
        wanted = HTTP_CLIENT_STREAM_CHUNK;
    if (wanted <= 0)
        return 0;

    // Body bytes that arrived alongside the headers come first.
    if (gStream.stageOff < gStream.stageLen) {
        int avail = gStream.stageLen - gStream.stageOff;
        if (avail > wanted)
            avail = wanted;
        memcpy(out, &gStage[gStream.stageOff], avail);
        gStream.stageOff += avail;
        done += avail;
        if (gStream.remaining > 0)
            gStream.remaining -= avail;
    }

    while (done < wanted) {
        int chunk = wanted - done;
        int r;

        if (gStream.remaining == 0)
            break; // Content-Length satisfied
        if (gStream.remaining > 0 && chunk > gStream.remaining)
            chunk = gStream.remaining;

        r = RecvBounded(gStream.socket, &out[done], chunk);
        if (r <= 0) {
            // Short of what Content-Length promised is a truncated transfer, and must never be
            // reported as a successful read of fewer bytes.
            if (gStream.remaining > 0) {
                gStream.live = 0;
                return HTTP_STREAM_ERR_TRUNC;
            }
            break; // length was unknown; the peer closing is how it ends
        }
        done += r;
        if (gStream.remaining > 0)
            gStream.remaining -= r;
    }

    return done;
}
