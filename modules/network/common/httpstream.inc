// Included by the menu RPC client and the HTTP cdvdman driver. Callers serialize access
// and supply SendData and RecvBounded using their own stack ABI.
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
static int ParseDecimal(const char **cursor, u64 *value)
{
    const char *s = *cursor;
    u64 v = 0;
    if (*s < '0' || *s > '9')
        return 0;
    while (*s >= '0' && *s <= '9') {
        unsigned int digit = *s++ - '0';
        if (v > 1844674407370955161ULL || (v == 1844674407370955161ULL && digit > 5))
            return 0;
        v = (v << 3) + (v << 1) + digit;
    }
    *cursor = s;
    *value = v;
    return 1;
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
    static char request[HTTP_CLIENT_STREAM_URI_MAX + 256];
    static char hdr[HTTP_HDR_MAX + 1];
    int length, hlen, hdrEnd, i, status;
    int sawChunked = 0, sawEncoding = 0, sawMultipart = 0;
    int bodyLen = -1;
    u64 rangeFirst = 0, rangeLast = 0, rangeTotal = 0;
    int sawRange = 0, sawRangeTotal = 0;
    int sawLength = 0;

    HttpStreamEnd();

    *contentLen = -1;
    *total = 0;
    *hasTotal = 0;

    if (strlen(uri) >= HTTP_CLIENT_STREAM_URI_MAX || strlen(host) >= HTTP_CLIENT_SERVER_NAME_MAX)
        return HTTP_STREAM_ERR_HEADERS;
    for (i = 0; uri[i]; i++)
        if ((unsigned char)uri[i] <= 32 || (unsigned char)uri[i] == 127)
            return HTTP_STREAM_ERR_HEADERS;
    for (i = 0; host[i]; i++)
        if ((unsigned char)host[i] <= 32 || (unsigned char)host[i] == 127)
            return HTTP_STREAM_ERR_HEADERS;
    if (useRange && (rangeEnd < rangeStart || rangeEnd - rangeStart >= 0x7fffffffULL))
        return HTTP_STREAM_ERR_RANGE;

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
    if ((hdr[7] != '0' && hdr[7] != '1') || hdr[8] != ' ' ||
        hdr[9] < '1' || hdr[9] > '5' || hdr[10] < '0' || hdr[10] > '9' ||
        hdr[11] < '0' || hdr[11] > '9' || (hdr[12] != ' ' && hdr[12] != '\r'))
        return HTTP_STREAM_ERR_STATUS;
    status = (hdr[9] - '0') * 100 + (hdr[10] - '0') * 10 + hdr[11] - '0';
    if (memchr(hdr, 0, hdrEnd) != NULL)
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
                const char *v = SkipBlanks(line + 15);
                u64 lengthValue;
                if (sawLength++ || !ParseDecimal(&v, &lengthValue) || *SkipBlanks(v) || lengthValue > 0x7fffffffULL)
                    return HTTP_STREAM_ERR_HEADERS;
                bodyLen = (int)lengthValue;
            } else if (HeaderIs(line, "Transfer-Encoding:")) {
                const char *v = SkipBlanks(line + 18);
                (void)v;
                sawChunked = 1;
            } else if (HeaderIs(line, "Content-Encoding:")) {
                const char *v = SkipBlanks(line + 17);
                if (!HeaderIs(v, "identity") || *SkipBlanks(v + 8))
                    sawEncoding = 1;
            } else if (HeaderIs(line, "Content-Type:")) {
                const char *v = SkipBlanks(line + 13);
                if (HeaderIs(v, "multipart/"))
                    sawMultipart = 1;
            } else if (HeaderIs(line, "Content-Range:")) {
                const char *v = SkipBlanks(line + 14);
                if (sawRange++ || !HeaderIs(v, "bytes "))
                    return HTTP_STREAM_ERR_RANGE;
                v = SkipBlanks(v + 6);
                if (!ParseDecimal(&v, &rangeFirst) || *v++ != '-' ||
                    !ParseDecimal(&v, &rangeLast) || *v++ != '/' ||
                    !ParseDecimal(&v, &rangeTotal) || *SkipBlanks(v))
                    return HTTP_STREAM_ERR_RANGE;
                sawRangeTotal = 1;
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
        if (!sawRange || !sawRangeTotal || rangeFirst != rangeStart || rangeLast != rangeEnd ||
            rangeLast >= rangeTotal || bodyLen != (int)(rangeEnd - rangeStart + 1))
            return HTTP_STREAM_ERR_RANGE;
    }

    if ((useRange && status == 200) || (!useRange && status == 206))
        return HTTP_STREAM_ERR_RANGE;
    if (status >= 300)
        return status;
    // A persistent body with no length cannot be distinguished from a stalled peer.
    if (bodyLen < 0)
        return HTTP_STREAM_ERR_HEADERS;
    if (hlen - hdrEnd > bodyLen)
        return HTTP_STREAM_ERR_TRUNC;

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
