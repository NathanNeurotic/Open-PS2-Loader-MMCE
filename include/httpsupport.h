#ifndef __HTTP_SUPPORT_H
#define __HTTP_SUPPORT_H

#include "include/iosupport.h"

// A network catalog, so the same slow-poll cadence SMB and UDPFS use.
#define HTTP_MODE_UPDATE_DELAY 300

// Longest relative ISO path a catalog row may carry. Matches CATALOG_PATH_MAX in
// pc/http/catalog_reference.py -- the two parsers are one contract and the fixtures under
// pc/http/fixtures/ hold them to it.
#define HTTP_CATALOG_PATH_MAX 255

// Bound on catalog size and row count. An over-limit catalog is REPORTED, never silently clipped:
// a library that is quietly missing its last hundred games is worse than one that says so.
#define HTTP_CATALOG_BYTES_MAX 262144
#define HTTP_CATALOG_ROWS_MAX  2048

void httpInit(item_list_t *itemList);
item_list_t *httpGetObject(int initOnly);

// Normalise a user-entered base path in place: force a leading '/', drop any trailing one, collapse
// an empty string to "/". Called on load and after the settings dialog writes the value.
void httpNormalizeBasePath(char *path, size_t size);

// Percent-encode one catalog-relative path into a request target under the configured base path.
// '/' separators are preserved; everything outside the unreserved set is encoded exactly once.
// Returns 0 when the result would not fit.
int httpBuildRequestPath(const char *relative, char *out, size_t outSize);

// The Test server action. Fetches the catalog, parses it, and -- when there is a game to try --
// verifies one small byte range and the total size. Writes a human-readable outcome into msg.
void httpTestServer(const int *ip, int port, const char *base, char *msg, size_t msgSize);

// Where HTTP's per-game CFG and art live. HTTP has no filesystem of its own, so this is the
// settings home rather than a device root; see docs/HTTP-INTEGRATION-PLAN.md section 2.2.
const char *httpGetLocalPrefix(void);

#endif
