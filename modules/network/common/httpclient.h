#define HTTP_CMODE_CLOSED     0
#define HTTP_CMODE_PERSISTENT 1

// EE-side only
int HttpInit(void);
void HttpDeinit(void);

int HttpEstabConnection(char *server, u16 port);
void HttpCloseConnection(s32 HttpSocket);

/*  mtime[0] = Years since year 2000
    mtime[1] = Month, 0-11
    mtime[2] = day in month, 0-30
    mtime[3] = Hour (0-23)
    mtime[4] = Minute (0-59)
    mtime[5] = Second (0-59)
*/

int HttpSendGetRequest(s32 HttpSocket, const char *HttpUserAgent, const char *host, s8 *mode, const u8 *mtime, const char *uri, char *output, u16 *out_len);

// Streaming GET. See HTTP_CLIENT_CMD_STREAM_BEGIN. begin() returns the HTTP status (or a negative
// HTTP_STREAM_ERR_*) and fills in what the response said about size; read() then returns payload
// until it returns 0. Only one stream may be live at a time.
int HttpStreamBegin(s32 HttpSocket, const char *host, const char *uri,
                    int useRange, u64 rangeStart, u64 rangeEnd, int keepAlive,
                    int *contentLen, u64 *total, int *hasTotal);
int HttpStreamRead(void *buffer, int wanted);
void HttpStreamEnd(void);

#define HTTP_CLIENT_SERVER_NAME_MAX 30
#define HTTP_CLIENT_USER_AGENT_MAX  16
#define HTTP_CLIENT_URI_MAX         128

// The streaming path's own URI budget. Separate from HTTP_CLIENT_URI_MAX because widening that
// would change the size of HttpClientSendGetArgs, which the compatibility updater has been using
// unchanged for years. A catalog path is percent-encoded once before it gets here, and encoding can
// multiply a byte by three, so this is deliberately generous.
#define HTTP_CLIENT_STREAM_URI_MAX 768

// Payload delivered per HTTP_CLIENT_CMD_STREAM_READ. The EE drives the loop and calls again for
// more, so this bounds IOP memory rather than the transfer.
#define HTTP_CLIENT_STREAM_CHUNK 2048

enum HTTP_CLIENT_CMD {
    HTTP_CLIENT_CMD_CONN_ESTAB,
    HTTP_CLIENT_CMD_CONN_CLOSE,
    HTTP_CLIENT_CMD_SEND_GET_REQ,
    // Bounded streaming GET, optionally ranged. BEGIN sends the request and parses the response
    // headers; READ pulls the body out in HTTP_CLIENT_STREAM_CHUNK-sized pieces until it is spent.
    //
    // This is a second path rather than a widening of SEND_GET_REQ on purpose. SEND_GET_REQ
    // silently truncates anything past its 512-byte DMA buffer (a printf and a success return) and
    // its body read gives up on the first short TCP read, so it cannot carry a catalog or a sector
    // and cannot be fixed without touching the compatibility updater's ABI.
    HTTP_CLIENT_CMD_STREAM_BEGIN,
    HTTP_CLIENT_CMD_STREAM_READ,
};

// Negative results from the streaming path. Kept distinct so the menu can say which thing went
// wrong instead of showing one generic network error.
#define HTTP_STREAM_ERR_SEND     -1 // could not put the request on the wire
#define HTTP_STREAM_ERR_HEADERS  -2 // no complete header block arrived
#define HTTP_STREAM_ERR_STATUS   -3 // no parseable HTTP/1.x status line
#define HTTP_STREAM_ERR_ENCODING -4 // chunked / content-encoded / multipart: outside this profile
#define HTTP_STREAM_ERR_RANGE    -5 // 206 whose Content-Range is not the range that was asked for
#define HTTP_STREAM_ERR_NOSTREAM -6 // READ without a live BEGIN
#define HTTP_STREAM_ERR_TRUNC    -7 // the peer stopped before Content-Length was satisfied

struct HttpClientStreamBeginArgs
{
    s32 socket;
    // Inclusive byte interval. Sent as a Range header only when useRange is set; a plain GET
    // otherwise, because the catalog must not require range support of the server.
    u32 rangeStartLo;
    u32 rangeStartHi;
    u32 rangeEndLo;
    u32 rangeEndHi;
    u8 useRange;
    u8 keepAlive;
    u8 padding[2];
    char host[HTTP_CLIENT_SERVER_NAME_MAX];
    char uri[HTTP_CLIENT_STREAM_URI_MAX];
};

struct HttpClientStreamBeginResult
{
    s32 result;     // HTTP status code, or one of HTTP_STREAM_ERR_*
    s32 contentLen; // body length, or -1 when the server did not say
    u32 totalLo;    // total entity size from Content-Range; 0 when absent
    u32 totalHi;
    u8 hasTotal;
    u8 padding[3];
};

struct HttpClientStreamReadArgs
{
    s32 wanted; // <= HTTP_CLIENT_STREAM_CHUNK
    void *output;
};

struct HttpClientStreamReadResult
{
    s32 result; // bytes delivered, 0 at end of body, or HTTP_STREAM_ERR_*
};

struct HttpClientConnEstabArgs
{
    char server[HTTP_CLIENT_SERVER_NAME_MAX];
    u16 port;
};

struct HttpClientConnCloseArgs
{
    s32 socket;
};

struct HttpClientSendGetArgs
{
    s32 socket;
    char UserAgent[HTTP_CLIENT_USER_AGENT_MAX];
    char host[HTTP_CLIENT_SERVER_NAME_MAX];
    s8 mode;
    u8 hasMtime;
    u8 mtime[6];
    char uri[HTTP_CLIENT_URI_MAX];
    u16 out_len;
    void *output;
};

struct HttpClientSendGetResult
{
    s32 result;
    s8 mode;
    u8 padding;
    u16 out_len;
};

#ifdef _IOP
#define httpc_IMPORTS_start DECLARE_IMPORT_TABLE(httpc, 1, 1)
#define httpc_IMPORTS_end   END_IMPORT_TABLE

#define I_HttpEstabConnection DECLARE_IMPORT(4, HttpEstabConnection)
#define I_HttpCloseConnection DECLARE_IMPORT(5, HttpCloseConnection)
#define I_HttpSendGetRequest  DECLARE_IMPORT(6, HttpSendGetRequest)
// Ordinals must match modules/network/httpclient/exports.tab position for position. Nothing on the
// IOP imports httpc today -- the menu reaches it over RPC -- but keeping these in step means the
// next module that does will not silently bind the wrong function.
#define I_HttpStreamBegin     DECLARE_IMPORT(7, HttpStreamBegin)
#define I_HttpStreamRead      DECLARE_IMPORT(8, HttpStreamRead)
#define I_HttpStreamEnd       DECLARE_IMPORT(9, HttpStreamEnd)
#endif
