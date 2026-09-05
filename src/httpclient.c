#include <string.h>
#include <kernel.h>
#include <sifrpc.h>

#include "httpclient.h"
#include "ioman.h"

static SifRpcClientData_t SifRpcClient;
// Must hold the largest argument struct; HttpClientStreamBeginArgs is the one that sets the size.
static unsigned char RpcTxBuffer[1024] ALIGNED(64);
static unsigned char RpcRxBuffer[64] ALIGNED(64);

int HttpInit(void)
{
    while (SifBindRpc(&SifRpcClient, 0x00001B14, 0) < 0 || SifRpcClient.server == NULL) {
        LOG("libhttpclient: bind failed\n");
        nopdelay();
    }

    return 0;
}

void HttpDeinit(void)
{
    memset(&SifRpcClient, 0, sizeof(SifRpcClientData_t));
}

int HttpEstabConnection(char *server, u16 port)
{
    int result;

    strncpy(((struct HttpClientConnEstabArgs *)RpcTxBuffer)->server, server, HTTP_CLIENT_SERVER_NAME_MAX - 1);
    ((struct HttpClientConnEstabArgs *)RpcTxBuffer)->server[HTTP_CLIENT_SERVER_NAME_MAX - 1] = '\0';
    ((struct HttpClientConnEstabArgs *)RpcTxBuffer)->port = port;

    if ((result = SifCallRpc(&SifRpcClient, HTTP_CLIENT_CMD_CONN_ESTAB, 0, RpcTxBuffer, sizeof(struct HttpClientConnEstabArgs), RpcRxBuffer, sizeof(s32), NULL, NULL)) >= 0)
        result = *(s32 *)RpcRxBuffer;

    return result;
}

void HttpCloseConnection(s32 HttpSocket)
{
    ((struct HttpClientConnCloseArgs *)RpcTxBuffer)->socket = HttpSocket;
    SifCallRpc(&SifRpcClient, HTTP_CLIENT_CMD_CONN_CLOSE, 0, RpcTxBuffer, sizeof(struct HttpClientConnCloseArgs), NULL, 0, NULL, NULL);
}

int HttpSendGetRequest(s32 HttpSocket, const char *UserAgent, const char *host, s8 *mode, const u8 *mtime, const char *uri, char *output, u16 *out_len)
{
    int result;

    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->socket = HttpSocket;
    strncpy(((struct HttpClientSendGetArgs *)RpcTxBuffer)->UserAgent, UserAgent, HTTP_CLIENT_USER_AGENT_MAX - 1);
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->UserAgent[HTTP_CLIENT_USER_AGENT_MAX - 1] = '\0';
    strncpy(((struct HttpClientSendGetArgs *)RpcTxBuffer)->host, host, HTTP_CLIENT_SERVER_NAME_MAX - 1);
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->host[HTTP_CLIENT_SERVER_NAME_MAX - 1] = '\0';
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->mode = *mode;
    if (mtime != NULL) {
        memcpy(((struct HttpClientSendGetArgs *)RpcTxBuffer)->mtime, mtime, sizeof(((struct HttpClientSendGetArgs *)RpcTxBuffer)->mtime));
        ((struct HttpClientSendGetArgs *)RpcTxBuffer)->hasMtime = 1;
    } else {
        memset(((struct HttpClientSendGetArgs *)RpcTxBuffer)->mtime, 0, sizeof(((struct HttpClientSendGetArgs *)RpcTxBuffer)->mtime));
        ((struct HttpClientSendGetArgs *)RpcTxBuffer)->hasMtime = 0;
    }
    strncpy(((struct HttpClientSendGetArgs *)RpcTxBuffer)->uri, uri, HTTP_CLIENT_URI_MAX - 1);
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->uri[HTTP_CLIENT_URI_MAX - 1] = '\0';
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->output = output;
    ((struct HttpClientSendGetArgs *)RpcTxBuffer)->out_len = *out_len;

    if (!IS_UNCACHED_SEG(output))
        SifWriteBackDCache(output, *out_len);

    if ((result = SifCallRpc(&SifRpcClient, HTTP_CLIENT_CMD_SEND_GET_REQ, 0, RpcTxBuffer, sizeof(struct HttpClientSendGetArgs), RpcRxBuffer, sizeof(struct HttpClientSendGetResult), NULL, NULL)) >= 0) {
        result = ((struct HttpClientSendGetResult *)RpcRxBuffer)->result;
        *mode = ((struct HttpClientSendGetResult *)RpcRxBuffer)->mode;
        *out_len = ((struct HttpClientSendGetResult *)RpcRxBuffer)->out_len;
    }

    return result;
}

int HttpStreamBegin(s32 HttpSocket, const char *host, const char *uri,
                    int useRange, u64 rangeStart, u64 rangeEnd, int keepAlive,
                    int *contentLen, u64 *total, int *hasTotal)
{
    struct HttpClientStreamBeginArgs *args = (struct HttpClientStreamBeginArgs *)RpcTxBuffer;
    struct HttpClientStreamBeginResult *res = (struct HttpClientStreamBeginResult *)RpcRxBuffer;
    int result;

    memset(args, 0, sizeof(*args));
    args->socket = HttpSocket;
    args->rangeStartLo = (u32)(rangeStart & 0xFFFFFFFF);
    args->rangeStartHi = (u32)(rangeStart >> 32);
    args->rangeEndLo = (u32)(rangeEnd & 0xFFFFFFFF);
    args->rangeEndHi = (u32)(rangeEnd >> 32);
    args->useRange = useRange ? 1 : 0;
    args->keepAlive = keepAlive ? 1 : 0;
    strncpy(args->host, host, sizeof(args->host) - 1);
    strncpy(args->uri, uri, sizeof(args->uri) - 1);

    *contentLen = -1;
    *total = 0;
    *hasTotal = 0;

    if ((result = SifCallRpc(&SifRpcClient, HTTP_CLIENT_CMD_STREAM_BEGIN, 0, RpcTxBuffer, sizeof(*args), RpcRxBuffer, sizeof(*res), NULL, NULL)) < 0)
        return result;

    *contentLen = res->contentLen;
    *total = ((u64)res->totalHi << 32) | res->totalLo;
    *hasTotal = res->hasTotal;

    return res->result;
}

int HttpStreamRead(void *buffer, int wanted)
{
    struct HttpClientStreamReadArgs *args = (struct HttpClientStreamReadArgs *)RpcTxBuffer;
    struct HttpClientStreamReadResult *res = (struct HttpClientStreamReadResult *)RpcRxBuffer;
    int result;

    if (wanted > HTTP_CLIENT_STREAM_CHUNK)
        wanted = HTTP_CLIENT_STREAM_CHUNK;
    if (wanted <= 0)
        return 0;

    args->wanted = wanted;
    args->output = buffer;

    // The IOP DMAs straight into this buffer, so anything the EE still holds in cache for it would
    // survive the transfer and be read back as stale bytes.
    if (!IS_UNCACHED_SEG(buffer))
        SifWriteBackDCache(buffer, wanted);

    if ((result = SifCallRpc(&SifRpcClient, HTTP_CLIENT_CMD_STREAM_READ, 0, RpcTxBuffer, sizeof(*args), RpcRxBuffer, sizeof(*res), NULL, NULL)) < 0)
        return result;

    return res->result;
}

void HttpStreamEnd(void)
{
    // Nothing to say to the IOP: the stream is torn down by HttpCloseConnection, and a BEGIN
    // always resets it. This exists so callers can pair Begin/End symmetrically.
}
