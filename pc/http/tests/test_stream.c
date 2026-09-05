/* Host test for the real modules/network/httpclient/httpclient.c streaming parser.
   The peer is scripted: tstData is served in tstChunk-sized pieces so header/body splits
   land wherever the test wants them. */
#include <stdio.h>
#include <string.h>
#include "stub/ps2ip.h"
#include "httpclient.h"

const unsigned char *tstData;
int tstLen, tstPos, tstChunk, tstSelectFail;

int stub_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{
    (void)n;
    (void)r;
    (void)w;
    (void)e;
    (void)t;
    if (tstSelectFail)
        return 0;
    return tstPos < tstLen ? 1 : 0;
}
int stub_recv(int s, void *b, int l, int f)
{
    (void)s;
    (void)f;
    int avail = tstLen - tstPos;
    if (avail <= 0)
        return 0;
    int give = l;
    if (tstChunk > 0 && give > tstChunk)
        give = tstChunk;
    if (give > avail)
        give = avail;
    memcpy(b, tstData + tstPos, give);
    tstPos += give;
    return give;
}
int stub_send(int s, const void *b, int l, int f)
{
    (void)s;
    (void)b;
    (void)f;
    return l;
}
int stub_closesocket(int s)
{
    (void)s;
    return 0;
}
int stub_shutdown(int s, int how)
{
    (void)s;
    (void)how;
    return 0;
}

static int fails = 0, checks = 0;
static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

static unsigned char scratch[65536];
static unsigned char body[8192];

static void feed(const char *hdr, int bodylen, int chunk)
{
    int hl = strlen(hdr);
    memcpy(scratch, hdr, hl);
    if (bodylen)
        memcpy(scratch + hl, body, bodylen);
    tstData = scratch;
    tstLen = hl + bodylen;
    tstPos = 0;
    tstChunk = chunk;
    tstSelectFail = 0;
}

int main(void)
{
    unsigned char out[8192];
    int cl, has, st, n, i;
    u64 total;

    for (i = 0; i < (int)sizeof(body); i++)
        body[i] = (unsigned char)(i * 7 + 3);

    printf("1 plain 200\n");
    feed("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", 100, 0);
    st = HttpStreamBegin(3, "h", "/games.csv", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == 200, "status 200");
    ck(cl == 100, "content-length 100");
    ck(has == 0, "no total");
    n = HttpStreamRead(out, 100);
    ck(n == 100, "read 100");
    ck(memcmp(out, body, 100) == 0, "bytes match");
    ck(HttpStreamRead(out, 10) == 0, "eof after length");

    printf("2 one-byte-at-a-time split\n");
    feed("HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n", 64, 1);
    st = HttpStreamBegin(3, "h", "/x", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == 200, "status");
    ck(cl == 64, "len");
    n = HttpStreamRead(out, 64);
    ck(n == 64, "read 64");
    ck(memcmp(out, body, 64) == 0, "bytes");

    printf("3 206 matching range\n");
    feed("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2048-4095/8547991552\r\nContent-Length: 2048\r\n\r\n", 2048, 0);
    st = HttpStreamBegin(3, "h", "/g.iso", 1, 2048, 4095, 1, &cl, &total, &has);
    ck(st == 206, "status 206");
    ck(has == 1, "has total");
    ck(total == 8547991552ULL, "dvd9 total exact");
    n = HttpStreamRead(out, 2048);
    ck(n == 2048, "read sector");
    ck(memcmp(out, body, 2048) == 0, "sector bytes");

    printf("4 206 shifted range refused\n");
    feed("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4096-6143/8547991552\r\nContent-Length: 2048\r\n\r\n", 2048, 0);
    st = HttpStreamBegin(3, "h", "/g.iso", 1, 2048, 4095, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_RANGE, "shifted range rejected");

    printf("5 206 without Content-Range refused\n");
    feed("HTTP/1.1 206 Partial Content\r\nContent-Length: 2048\r\n\r\n", 2048, 0);
    st = HttpStreamBegin(3, "h", "/g.iso", 1, 0, 2047, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_RANGE, "missing Content-Range rejected");

    printf("6 case-insensitive headers\n");
    feed("HTTP/1.1 206 Partial Content\r\ncOnTeNt-RaNgE: BYTES 0-1023/2048\r\ncontent-length: 1024\r\n\r\n", 1024, 0);
    st = HttpStreamBegin(3, "h", "/g.iso", 1, 0, 1023, 1, &cl, &total, &has);
    ck(st == 206, "odd case accepted");
    ck(total == 2048, "total parsed");
    ck(cl == 1024, "len parsed");

    printf("7 chunked refused\n");
    feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 16, 0);
    st = HttpStreamBegin(3, "h", "/x", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_ENCODING, "chunked rejected");

    printf("8 gzip refused\n");
    feed("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 16\r\n\r\n", 16, 0);
    st = HttpStreamBegin(3, "h", "/x", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_ENCODING, "gzip rejected");

    printf("9 multipart refused\n");
    feed("HTTP/1.1 206 Partial Content\r\nContent-Type: multipart/byteranges; boundary=x\r\nContent-Length: 16\r\n\r\n", 16, 0);
    st = HttpStreamBegin(3, "h", "/x", 1, 0, 15, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_ENCODING, "multipart rejected");

    printf("10 truncation is an error\n");
    feed("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n", 1000, 0);
    st = HttpStreamBegin(3, "h", "/x", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == 200, "status");
    n = HttpStreamRead(out, 2048);
    ck(n == HTTP_STREAM_ERR_TRUNC, "truncated read reported as error");

    printf("11 416 without Content-Range\n");
    feed("HTTP/1.1 416 Requested Range Not Satisfiable\r\nContent-Length: 0\r\n\r\n", 0, 0);
    st = HttpStreamBegin(3, "h", "/x", 1, 999999, 1000000, 1, &cl, &total, &has);
    ck(st == 416, "416 surfaces as a status");

    printf("12 404\n");
    feed("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0, 0);
    st = HttpStreamBegin(3, "h", "/nope.iso", 1, 0, 2047, 1, &cl, &total, &has);
    ck(st == 404, "404 surfaces as a status");

    printf("13 NUL bytes in body\n");
    memset(body, 0, 512);
    feed("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-511/512\r\nContent-Length: 512\r\n\r\n", 512, 0);
    st = HttpStreamBegin(3, "h", "/g.iso", 1, 0, 511, 1, &cl, &total, &has);
    ck(st == 206, "status with NUL body");
    n = HttpStreamRead(out, 512);
    ck(n == 512, "read all-NUL sector");
    {
        int allzero = 1;
        for (i = 0; i < 512; i++)
            if (out[i])
                allzero = 0;
        ck(allzero, "NUL bytes intact");
    }

    printf("14 chunked draining\n");
    for (i = 0; i < (int)sizeof(body); i++)
        body[i] = (unsigned char)(i * 13 + 1);
    feed("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", 5000, 700);
    st = HttpStreamBegin(3, "h", "/big.csv", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == 200, "status");
    ck(cl == 5000, "len 5000");
    {
        int got = 0, r;
        while ((r = HttpStreamRead(out + got, HTTP_CLIENT_STREAM_CHUNK)) > 0) {
            got += r;
            if (got > 6000)
                break;
        }
        ck(r >= 0, "no error while draining");
        ck(got == 5000, "drained exactly 5000");
        ck(memcmp(out, body, 5000) == 0, "5000 bytes match");
    }

    printf("15 header block never completes\n");
    feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 0, 0);
    st = HttpStreamBegin(3, "h", "/x", 0, 0, 0, 1, &cl, &total, &has);
    ck(st == HTTP_STREAM_ERR_HEADERS, "incomplete headers rejected");

    printf("16 read without begin\n");
    HttpStreamEnd();
    ck(HttpStreamRead(out, 16) == HTTP_STREAM_ERR_NOSTREAM, "read without begin refused");

    printf("\n%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
