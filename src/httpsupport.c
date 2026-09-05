// HTTP as a game source: a games.csv catalog on a plain static HTTP server, and (from phase 4)
// byte-range reads of the ISO behind each row.
//
// The catalog parser here and pc/http/catalog_reference.py are one contract, held to it by the
// fixtures in pc/http/fixtures/. Change one and you change all three, in the same commit.
//
// Compatibility with Docmine17's existing server and existing catalogs is a release gate, not a
// nicety: the legacy two- and three-field rows below are why an existing games.csv keeps working
// with no conversion step.

#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/httpsupport.h"
#include "include/httpcatalog.h"
#include "include/netsupport.h"
#include "include/libview.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/config.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/mmcesupport.h"
#include "modules/iopcore/common/cdvd_config.h"
#include "httpclient.h"

// One catalog row. base_game_info_t has nowhere to put the relative path -- it carries name[161],
// startup[13] and extension[5] and nothing else -- so the path lives here, indexed in step with
// httpGames. Overloading name for the path is how the donor ends up requesting the display title.
typedef struct
{
    char path[HTTP_CATALOG_PATH_MAX + 1];
    unsigned char supported; // 0 for a row we can list but must refuse to launch (.zso today)
} http_catalog_extra_t;

static base_game_info_t *httpGames = NULL;
static http_catalog_extra_t *httpExtras = NULL;
static int httpGameCount = 0;
static int httpRefreshPending = 1;
static int httpLastError = 0; // an HTTP status, or an HTTP_STREAM_ERR_*, or 0
static char httpLocalPrefix[256];

typedef struct
{
    int ip[4];
    int port;
    char base[HTTP_BASE_PATH_MAX];
} http_endpoint_t;
static http_endpoint_t httpEndpoint;

static item_list_t httpGameList;

// ---------------------------------------------------------------------------
// Endpoint helpers
// ---------------------------------------------------------------------------

void httpNormalizeBasePath(char *path, size_t size)
{
    size_t len;

    if (path == NULL || size == 0)
        return;

    if (path[0] == '\0') {
        snprintf(path, size, "/");
        return;
    }

    if (path[0] != '/') {
        char tmp[HTTP_BASE_PATH_MAX];
        snprintf(tmp, sizeof(tmp), "/%s", path);
        snprintf(path, size, "%s", tmp);
    }

    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

static int httpIsUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static int httpBuildPath(const http_endpoint_t *endpoint, const char *relative, char *out, size_t outSize)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    const char *p;

    if (out == NULL || outSize == 0)
        return 0;

    // Base path first, verbatim: the user typed it and it is already normalised.
    used = snprintf(out, outSize, "%s", endpoint->base);
    if (used >= outSize)
        return 0;
    if (used > 0 && out[used - 1] != '/') {
        if (used + 1 >= outSize)
            return 0;
        out[used++] = '/';
    }

    // Encode once. '/' stays a separator; everything else outside the unreserved set becomes %XX,
    // which is what makes a title containing a space, '#', '?' or a literal '%' resolve.
    for (p = relative; *p != '\0'; p++) {
        if (*p == '/') {
            if (used + 1 >= outSize)
                return 0;
            out[used++] = '/';
        } else if (httpIsUnreserved(*p)) {
            if (used + 1 >= outSize)
                return 0;
            out[used++] = *p;
        } else {
            unsigned char c = (unsigned char)*p;
            if (used + 3 >= outSize)
                return 0;
            out[used++] = '%';
            out[used++] = hex[(c >> 4) & 0xF];
            out[used++] = hex[c & 0xF];
        }
    }

    out[used] = '\0';
    return 1;
}

static void httpServerIpString(const http_endpoint_t *endpoint, char *out, size_t size)
{
    snprintf(out, size, "%d.%d.%d.%d", endpoint->ip[0], endpoint->ip[1], endpoint->ip[2], endpoint->ip[3]);
}

// ---------------------------------------------------------------------------
// Catalog parsing lives in src/httpcatalog.c, which depends on nothing but the C library so the
// host can compile it and diff it against pc/http/catalog_reference.py over the shared fixtures.
// ---------------------------------------------------------------------------

// Parse a whole catalog buffer. Returns the accepted row count.
static int httpParseCatalog(char *buf, int len, base_game_info_t **outGames, http_catalog_extra_t **outExtras)
{
    int rows = 0, rejected = 0;
    char *line = buf;
    char *end = buf + len;

    base_game_info_t *httpGames = NULL;
    http_catalog_extra_t *httpExtras = NULL;
    *outGames = NULL;
    *outExtras = NULL;
    if (memchr(buf, 0, len) != NULL)
        return -1;

    httpGames = (base_game_info_t *)malloc(sizeof(base_game_info_t) * HTTP_CATALOG_ROWS_MAX);
    httpExtras = (http_catalog_extra_t *)malloc(sizeof(http_catalog_extra_t) * HTTP_CATALOG_ROWS_MAX);
    if (httpGames == NULL || httpExtras == NULL) {
        free(httpGames);
        free(httpExtras);
        httpGames = NULL;
        httpExtras = NULL;
        return -1;
    }

    while (line < end) {
        char *nl = line;
        char *trimmed;
        int i, duplicate = 0;

        while (nl < end && *nl != '\n')
            nl++;
        if (nl < end)
            *nl = '\0';
        else
            *nl = '\0'; // buffer is over-allocated by one for exactly this
        // Strip a single trailing CR so CRLF and LF both parse.
        if (nl > line && *(nl - 1) == '\r')
            *(nl - 1) = '\0';

        trimmed = httpCatalogSkipBlankOrComment(line);

        if (trimmed != NULL) {
            http_catalog_row_t parsed;

            if (httpCatalogParseLine(trimmed, &parsed) == HTTP_CAT_OK) {
                if (rows == HTTP_CATALOG_ROWS_MAX) {
                    free(httpGames);
                    free(httpExtras);
                    httpLastError = HTTP_STREAM_ERR_TRUNC;
                    return -1;
                }
                snprintf(httpGames[rows].startup, sizeof(httpGames[rows].startup), "%s", parsed.startup);
                snprintf(httpGames[rows].name, sizeof(httpGames[rows].name), "%s", parsed.title);
                snprintf(httpGames[rows].extension, sizeof(httpGames[rows].extension), "%s", parsed.extension);
                httpGames[rows].parts = 1;
                httpGames[rows].media = parsed.media;
                httpGames[rows].format = GAME_FORMAT_ISO;
                httpGames[rows].sizeMB = 0;
                snprintf(httpExtras[rows].path, sizeof(httpExtras[rows].path), "%s", parsed.path);
                httpExtras[rows].supported = parsed.supported;

                // First record wins, so a refresh is stable rather than order-dependent.
                for (i = 0; i < rows; i++) {
                    if (!strcmp(httpGames[i].startup, httpGames[rows].startup)) {
                        duplicate = 1;
                        break;
                    }
                }
                if (duplicate)
                    rejected++;
                else
                    rows++;
            } else {
                rejected++;
            }
        }

        line = nl + 1;
    }

    if (rejected > 0)
        LOG("HTTP catalog: %d row(s) rejected\n", rejected);

    if (rejected && !rows) {
        free(httpGames);
        free(httpExtras);
        return -1;
    }
    *outGames = httpGames;
    *outExtras = httpExtras;
    return rows;
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

// Opens a connection, issues one GET (ranged or not), and returns the socket with a live stream.
// Returns <0 on failure, and sets httpLastError to something the UI can name.
static int httpOpenRequest(const http_endpoint_t *endpoint, const char *relative, int useRange, u64 from, u64 to,
                           int *contentLen, u64 *total, int *hasTotal)
{
    char server[32];
    char uri[HTTP_CLIENT_STREAM_URI_MAX];
    int sock, status;

    httpServerIpString(endpoint, server, sizeof(server));

    if (!httpBuildPath(endpoint, relative, uri, sizeof(uri))) {
        httpLastError = HTTP_STREAM_ERR_SEND;
        return -1;
    }

    sock = HttpEstabConnection(server, (u16)endpoint->port);
    if (sock < 0) {
        httpLastError = HTTP_STREAM_ERR_SEND;
        return -1;
    }

    status = HttpStreamBegin(sock, server, uri, useRange, from, to, 1, contentLen, total, hasTotal);
    if (status != (useRange ? 206 : 200)) {
        httpLastError = status;
        HttpCloseConnection(sock);
        return -1;
    }

    httpLastError = 0;
    return sock;
}

// Fetch the catalog into a freshly allocated buffer. Returns the byte count, or <0 on failure.
// A partial transfer is a failure, never a shorter success: publishing half a library as if it
// were whole is the failure mode that makes a missing game look like a deleted one.
static int httpFetchCatalog(const http_endpoint_t *endpoint, char **outBuf)
{
    char *buf = NULL;
    int cap = 8192, len = 0;
    int contentLen, hasTotal, sock, r;
    u64 total;

    *outBuf = NULL;

    sock = httpOpenRequest(endpoint, "games.csv", 0, 0, 0, &contentLen, &total, &hasTotal);
    if (sock < 0)
        return -1;

    if (contentLen > HTTP_CATALOG_BYTES_MAX) {
        LOG("HTTP catalog: %d bytes exceeds the %d limit\n", contentLen, HTTP_CATALOG_BYTES_MAX);
        HttpCloseConnection(sock);
        httpLastError = HTTP_STREAM_ERR_TRUNC;
        return -1;
    }
    if (contentLen > 0)
        cap = contentLen;

    // One spare byte: httpParseCatalog terminates the final line in place.
    buf = (char *)malloc(cap + 1);
    if (buf == NULL) {
        HttpCloseConnection(sock);
        return -1;
    }

    for (;;) {
        int want = cap - len;
        if (contentLen >= 0 && len == contentLen)
            break;

        if (want <= 0) {
            char *grown;
            if (cap >= HTTP_CATALOG_BYTES_MAX) {
                LOG("HTTP catalog: exceeded the %d byte limit\n", HTTP_CATALOG_BYTES_MAX);
                free(buf);
                HttpCloseConnection(sock);
                httpLastError = HTTP_STREAM_ERR_TRUNC;
                return -1;
            }
            cap *= 2;
            if (cap > HTTP_CATALOG_BYTES_MAX)
                cap = HTTP_CATALOG_BYTES_MAX;
            grown = (char *)realloc(buf, cap + 1);
            if (grown == NULL) {
                free(buf);
                HttpCloseConnection(sock);
                return -1;
            }
            buf = grown;
            want = cap - len;
        }
        if (want > HTTP_CLIENT_STREAM_CHUNK)
            want = HTTP_CLIENT_STREAM_CHUNK;

        r = HttpStreamRead(&buf[len], want);
        if (r < 0) {
            LOG("HTTP catalog: read failed (%d)\n", r);
            httpLastError = r;
            free(buf);
            HttpCloseConnection(sock);
            return -1;
        }
        if (r == 0)
            break;
        len += r;
    }

    HttpCloseConnection(sock);

    buf[len] = '\0';
    *outBuf = buf;

    return len;
}

// ---------------------------------------------------------------------------
// Test server
// ---------------------------------------------------------------------------

void httpTestServer(const int *ip, int port, const char *base, char *msg, size_t msgSize)
{
    char *buf = NULL;
    int len, rows = 0;
    http_endpoint_t testEndpoint;
    memcpy(testEndpoint.ip, ip, sizeof(testEndpoint.ip));
    testEndpoint.port = port;
    snprintf(testEndpoint.base, sizeof(testEndpoint.base), "%s", base);
    int contentLen, hasTotal, sock, r;
    u64 total;

    if (netLoadInitModules() != 0) {
        snprintf(msg, msgSize, "%s", _l(_STR_HTTP_ERR_CONNECT));
        return;
    }

    len = httpFetchCatalog(&testEndpoint, &buf);
    if (len < 0) {
        snprintf(msg, msgSize, "%s", _l(_STR_HTTP_ERR_CATALOG));
        return;
    }

    base_game_info_t *testGames;
    http_catalog_extra_t *testExtras;
    char probePath[HTTP_CATALOG_PATH_MAX + 1] = "";
    rows = httpParseCatalog(buf, len, &testGames, &testExtras);
    free(buf);
    for (int i = 0; i < rows; i++) {
        if (testExtras[i].supported) {
            snprintf(probePath, sizeof(probePath), "%s", testExtras[i].path);
            break;
        }
    }
    free(testGames);
    free(testExtras);
    if (rows < 0) {
        snprintf(msg, msgSize, "%s", _l(_STR_HTTP_ERR_CATALOG));
        return;
    }

    if (rows == 0) {
        // Valid, and proves nothing about range support -- say both.
        snprintf(msg, msgSize, "%s", _l(_STR_HTTP_TEST_EMPTY));
        return;
    }

    // One small range against the first supported row. This is the half that actually matters:
    // a catalog can be served by anything, an ISO cannot.
    {
        if (!probePath[0]) {
            snprintf(msg, msgSize, "%s", _l(_STR_HTTP_TEST_NO_ISO));
            return;
        }
        sock = httpOpenRequest(&testEndpoint, probePath, 1, 0, 2047, &contentLen, &total, &hasTotal);
        if (sock < 0) {
            snprintf(msg, msgSize, "%s", _l(_STR_HTTP_ERR_RANGE));
            return;
        }

        r = 0;
        {
            static char probe[2048];
            int got = 0, n;
            while (got < 2048) {
                n = HttpStreamRead(&probe[got], 2048 - got);
                if (n <= 0)
                    break;
                got += n;
            }
            r = got;
        }
        HttpCloseConnection(sock);

        if (r != 2048 || !hasTotal) {
            snprintf(msg, msgSize, "%s", _l(_STR_HTTP_ERR_RANGE));
            return;
        }
    }

    snprintf(msg, msgSize, "%s: %d", _l(_STR_HTTP_TEST_OK), rows);
}

// ---------------------------------------------------------------------------
// item_list_t
// ---------------------------------------------------------------------------

const char *httpGetLocalPrefix(void)
{
    const char *home = configGetHomePath();
    size_t len;

    if (home == NULL || home[0] == '\0')
        home = "mc0:OPL";

    snprintf(httpLocalPrefix, sizeof(httpLocalPrefix), "%s", home);
    len = strlen(httpLocalPrefix);
    if (len > 0 && httpLocalPrefix[len - 1] != '/' && httpLocalPrefix[len - 1] != ':' &&
        len + 1 < sizeof(httpLocalPrefix)) {
        httpLocalPrefix[len] = '/';
        httpLocalPrefix[len + 1] = '\0';
    }

    return httpLocalPrefix;
}

static char *httpGetPrefix(item_list_t *itemList)
{
    (void)itemList;
    // Not a filesystem prefix -- there is no http: ioman device and there deliberately is not going
    // to be one in this release. Per-game data resolves through httpGetLocalPrefix() instead.
    return NULL;
}

static int httpGetTextId(item_list_t *itemList)
{
    (void)itemList;
    return _STR_HTTP_GAMES;
}

static int httpGetIconId(item_list_t *itemList)
{
    (void)itemList;
    return ETH_ICON; // a network source; it shares SMB's glyph until HTTP gets its own
}

void httpInit(item_list_t *itemList)
{
    (void)itemList;
    LOG("HTTPSUPPORT Init\n");

    httpNormalizeBasePath(gHttpBasePath, sizeof(gHttpBasePath));
    memcpy(httpEndpoint.ip, gHttpServerIp, sizeof(httpEndpoint.ip));
    httpEndpoint.port = gHttpPort;
    snprintf(httpEndpoint.base, sizeof(httpEndpoint.base), "%s", gHttpBasePath);
    httpGameList.enabled = 1;
    httpRefreshPending = 1;
}

static int httpNeedsUpdate(item_list_t *itemList)
{
    (void)itemList;

    if (gNetworkProtocol != NET_PROTO_HTTP)
        return 0;

    return httpRefreshPending || libViewConsumeDirty(HTTP_MODE);
}

static int httpUpdateGameList(item_list_t *itemList)
{
    char *buf = NULL;
    int len;

    (void)itemList;
    httpRefreshPending = 0;

    if (gNetworkProtocol != NET_PROTO_HTTP)
        return 0;

    if (netLoadInitModules() != 0) {
        LOG("HTTPSUPPORT: network stack unavailable\n");
        return 0;
    }

    len = httpFetchCatalog(&httpEndpoint, &buf);
    if (len < 0) {
        // Keep whatever was listed before rather than replacing a good library with an empty one
        // on a transient failure. A successful EMPTY catalog is different, and clears it below.
        LOG("HTTPSUPPORT: catalog fetch failed, keeping %d existing row(s)\n", httpGameCount);
        return httpGameCount;
    }

    base_game_info_t *newGames;
    http_catalog_extra_t *newExtras;
    int count = httpParseCatalog(buf, len, &newGames, &newExtras);
    free(buf);
    if (count < 0)
        return httpGameCount;
    free(httpGames);
    free(httpExtras);
    httpGames = newGames;
    httpExtras = newExtras;
    httpGameCount = count;
    LOG("HTTPSUPPORT: %d game(s)\n", httpGameCount);

    return httpGameCount;
}

static int httpGetGameCount(item_list_t *itemList)
{
    (void)itemList;
    return httpGameCount;
}

static void *httpGetGame(item_list_t *itemList, int id)
{
    (void)itemList;
    if (id < 0 || id >= httpGameCount || httpGames == NULL)
        return NULL;

    return &httpGames[id];
}

static char *httpGetGameName(item_list_t *itemList, int id)
{
    (void)itemList;
    if (id < 0 || id >= httpGameCount || httpGames == NULL)
        return NULL;

    return httpGames[id].name;
}

static int httpGetGameNameLength(item_list_t *itemList, int id)
{
    (void)itemList;
    if (id < 0 || id >= httpGameCount)
        return 0;

    return ISO_GAME_NAME_MAX + 1;
}

static char *httpGetGameStartup(item_list_t *itemList, int id)
{
    (void)itemList;
    if (id < 0 || id >= httpGameCount || httpGames == NULL)
        return NULL;

    return httpGames[id].startup;
}

// The catalog is the server's, and this profile is read-only. Both are deliberately inert rather
// than silently doing nothing somewhere the user cannot see.
static void httpDeleteGame(item_list_t *itemList, int id)
{
    (void)itemList;
    (void)id;
}

static void httpRenameGame(item_list_t *itemList, int id, char *newName)
{
    (void)itemList;
    (void)id;
    (void)newName;
}

// Read `length` bytes at `offset` of a catalog image into `buffer`, over one short-lived
// connection. Returns 1 on success. Used only for the launch-time metadata probes: the in-game
// driver does its own reads through device-http.c once the IOP has been reset.
static int httpProbeRead(const http_endpoint_t *endpoint, const char *relative,
                         u64 offset, int length, void *buffer, u64 *total)
{
    int contentLen, hasTotal, sock, got = 0, n;

    sock = httpOpenRequest(endpoint, relative, 1, offset, offset + (u64)length - 1,
                           &contentLen, total, &hasTotal);
    if (sock < 0)
        return 0;

    while (got < length) {
        n = HttpStreamRead((char *)buffer + got, length - got);
        if (n <= 0)
            break;
        got += n;
    }
    HttpCloseConnection(sock);

    // hasTotal is guaranteed by the stream layer for a 206 -- it refuses one without a complete
    // Content-Range -- but the caller depends on it, so do not take that on trust here.
    return (got == length && hasTotal);
}

// Total image size and, for a DVD, where layer 1 starts. This is the HTTP equivalent of what
// sbGetISO9660MaxLBA + sbProbeISO9660 do through a file descriptor, and it is why DVD9 needs more
// than wide offsets: the layer boundary lives in the image, not in the arithmetic.
static int httpProbeImage(const http_endpoint_t *endpoint, const char *relative,
                          u8 media, u64 *sizeOut, u32 *layer1StartOut)
{
    unsigned char sector[2048];
    unsigned char probe[8];
    u64 total = 0, ignored = 0;
    u32 maxLBA;

    *sizeOut = 0;
    *layer1StartOut = 0;

    // Primary volume descriptor, sector 16. Byte 80 is the volume space size in sectors.
    if (!httpProbeRead(endpoint, relative, 16 * 2048, sizeof(sector), sector, &total))
        return 0;
    if (total == 0)
        return 0;
    *sizeOut = total;

    maxLBA = (u32)sector[80] | ((u32)sector[81] << 8) | ((u32)sector[82] << 16) | ((u32)sector[83] << 24);

    // Only a DVD can be dual layer, and a second ISO9660 descriptor at maxLBA is what says it is.
    // Anything else -- including a probe we could not complete -- stays single layer, because
    // guessing a layer break wrong is a corrupt read halfway through a game rather than an error.
    if (media == SCECdPS2DVD && maxLBA > 16 && ((u64)maxLBA * 2048) + sizeof(probe) <= total) {
        if (httpProbeRead(endpoint, relative, (u64)maxLBA * 2048, sizeof(probe), probe, &ignored)) {
            if (probe[0] == 1 && !strncmp((char *)&probe[1], "CD001", 5))
                *layer1StartOut = maxLBA - 16;
        }
    }

    return 1;
}

static void httpLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    struct cdvdman_settings_http *settings;
    http_endpoint_t endpoint;
    base_game_info_t *game;
    char filename[32];
    char startup[GAME_STARTUP_MAX + 1];
    char relative[HTTP_CATALOG_PATH_MAX + 1];
    char uri[CDVDMAN_HTTP_URI_MAX];
    char server[16];
    u64 imageSize = 0;
    u32 layer1Start = 0;
    u8 media;
    int i, compatmask, result;

    (void)itemList;

    if (id < 0 || id >= httpGameCount || httpGames == NULL || httpExtras == NULL)
        return;

    game = &httpGames[id];
    media = game->media;

    // A .zso lists so an existing catalog is not silently short, but streaming compressed bytes as
    // raw sectors would corrupt without ever reporting an error. Refuse it by name.
    if (!httpExtras[id].supported) {
        guiMsgBox(_l(_STR_HTTP_ERR_COMPRESSED), 0, NULL);
        return;
    }

    // $CoreLoader honesty, exactly as SMB does it: there is no Neutrino launch leg here, so a
    // per-game or global Neutrino selection resolves to the OPL core. Say so rather than leaving
    // the setting looking honoured. Covers Favourites-origin launches, which the compat-dialog
    // lock in guigame.c cannot reach.
    {
        int coreLoader = gDefaultCoreLoader;
        configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoader);
        if (coreLoader == 2)
            coreLoader = gDefaultCoreLoader;
        if (coreLoader)
            guiWarning(_l(_STR_NEUTRINO_SMB_FALLBACK), 6);
    }

    // Everything the launch needs, copied into storage the launch owns, BEFORE deinit frees the
    // catalog. game and httpExtras[id] both die in httpCleanUp.
    memcpy(&endpoint, &httpEndpoint, sizeof(endpoint));
    snprintf(startup, sizeof(startup), "%s", game->startup);
    snprintf(relative, sizeof(relative), "%s", httpExtras[id].path);

    if (!httpBuildPath(&endpoint, relative, uri, sizeof(uri))) {
        guiMsgBox(_l(_STR_HTTP_ERR_CATALOG), 0, NULL);
        return;
    }
    httpServerIpString(&endpoint, server, sizeof(server));

    // The size is not optional: device-http.c bounds every read against it, so a launch without it
    // would either refuse every sector or trust the server's framing alone.
    if (!httpProbeImage(&endpoint, relative, media, &imageSize, &layer1Start)) {
        guiMsgBox(_l(_STR_HTTP_ERR_RANGE), 0, NULL);
        return;
    }

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", startup);
        saveConfig(CONFIG_LAST, 0);
    }

    compatmask = sbPrepare(game, configSet, size_http_cdvdman_irx, http_cdvdman_irx, &i);

#ifdef RETROACHIEVEMENTS
    // Settled before sysLaunchLoaderElf reads GetWatchCount(). Local, like the rest of HTTP's
    // per-game data -- there is no writable server side to put a watch list on.
    sbLoadWatchList(httpGetLocalPrefix(), startup);
#endif
    if ((result = sbLoadCheats(httpGetLocalPrefix(), startup)) < 0) {
        if (!sbCheatsMissingContinue((u8 *)(&http_cdvdman_irx) + i, result))
            return;
    }
    sbLoadImage(httpGetLocalPrefix(), startup);

    settings = (struct cdvdman_settings_http *)((u8 *)(&http_cdvdman_irx) + i);

    snprintf(settings->server, sizeof(settings->server), "%s", server);
    settings->port = (u16)endpoint.port;
    snprintf(settings->uri, sizeof(settings->uri), "%s", uri);
    settings->size_lo = (u32)(imageSize & 0xFFFFFFFF);
    settings->size_hi = (u32)(imageSize >> 32);
    settings->common.media = media;
    settings->common.layer1_start = layer1Start;
    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_SMAP;

    if (layer1Start != 0)
        LOG("HTTP DVD-DL layer 1 @ sector 0x%lx.\n", layer1Start);

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        snprintf(filename, sizeof(filename), "%s", startup);

    mmceSendGameID(startup, NULL, 0);
    deinit(NO_EXCEPTION, HTTP_MODE); // frees httpGames/httpExtras -- everything above is a copy

    // EnablePS2Logo is 0 unconditionally: CheckPS2Logo needs a file descriptor to read the logo
    // out of the image, and HTTP has no filesystem to open. Passing gPS2Logo through unverified
    // would be claiming a check that never ran.
    sysLaunchLoaderElf(filename, "HTTP_MODE", size_http_cdvdman_irx, http_cdvdman_irx, 0, NULL, 0, compatmask);
}

static config_set_t *httpGetConfig(item_list_t *itemList, int id)
{
    (void)itemList;
    if (id < 0 || id >= httpGameCount || httpGames == NULL)
        return NULL;

    // Local, because HTTP has no writable filesystem of its own. See the plan, section 2.2.
    return sbPopulateConfig(&httpGames[id], httpGetLocalPrefix(), "/");
}

static int httpGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    (void)itemList;
    (void)psm;
    // Art lives in the ART folder and nowhere else -- one location per type, only the key varies.
    // For HTTP the folder is under the LOCAL prefix, because the server is read-only and there is
    // no http: filesystem to open through. Adding a second lookup path here is the mistake the
    // standing rule exists to prevent.
    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", httpGetLocalPrefix(), folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);

    return texDiscoverLoad(resultTex, path, -1);
}

static void httpCleanUp(item_list_t *itemList, int exception)
{
    (void)itemList;
    (void)exception;

    if (httpGames != NULL) {
        free(httpGames);
        httpGames = NULL;
    }
    if (httpExtras != NULL) {
        free(httpExtras);
        httpExtras = NULL;
    }
    httpGameCount = 0;
}

static void httpShutdown(item_list_t *itemList)
{
    httpCleanUp(itemList, NO_EXCEPTION);
}

static int httpCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    (void)itemList;
    (void)name;
    (void)createSize;

    // No VMC over HTTP: this profile never writes to the server, and the donor passes no VMC
    // module either. Physical cards are the save path.
    return 0;
}

item_list_t *httpGetObject(int initOnly)
{
    if (initOnly && !httpGameList.enabled)
        return &httpGameList;

    return &httpGameList;
}

static item_list_t httpGameList = {
    HTTP_MODE, 0, MODE_FLAG_NO_UPDATE, 0, MENU_MIN_INACTIVE_FRAMES, HTTP_MODE_UPDATE_DELAY, NULL, NULL, &httpGetTextId, &httpGetPrefix, &httpInit, &httpNeedsUpdate,
    &httpUpdateGameList, &httpGetGameCount, &httpGetGame, &httpGetGameName, &httpGetGameNameLength, &httpGetGameStartup, &httpDeleteGame, &httpRenameGame,
    &httpLaunchGame, &httpGetConfig, &httpGetImage, &httpCleanUp, &httpShutdown, &httpCheckVMC, &httpGetIconId,
    /* itemLaunchVcd */ NULL, /* viewOverride */ 0, /* itemGetArtArchivePath */ NULL, /* itemLaunchCue */ NULL,
    /* itemGetView */ NULL, /* itemGetSourceId */ NULL};
