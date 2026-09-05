#ifndef __HTTP_CATALOG_H
#define __HTTP_CATALOG_H

// The games.csv contract, on its own, depending on nothing but the C library.
//
// It lives apart from httpsupport.c so the host can compile it unchanged and run it against the
// same fixtures as pc/http/catalog_reference.py. The two parsers are one contract; that claim is
// only worth something if something checks it, and pc/http/tests/test_catalog.c is what does.
//
// Section 4 of docs/HTTP-INTEGRATION-PLAN.md is the prose version.

#include <stddef.h>

#define HTTP_CAT_STARTUP_MAX 12  // GAME_STARTUP_MAX
#define HTTP_CAT_TITLE_MAX   160 // ISO_GAME_NAME_MAX
#define HTTP_CAT_PATH_MAX    255

#define HTTP_CAT_MEDIA_CD  0x12
#define HTTP_CAT_MEDIA_DVD 0x14

// Why a line was refused. The menu turns these into distinct messages rather than one generic
// "bad catalog", because a user with a typo needs to know which row and what about it.
enum HTTP_CAT_REJECT {
    HTTP_CAT_OK = 0,
    HTTP_CAT_REJ_EMPTY_STARTUP,
    HTTP_CAT_REJ_STARTUP_TOO_LONG,
    HTTP_CAT_REJ_CONTROL_CHAR,
    HTTP_CAT_REJ_TITLE_TOO_LONG,
    HTTP_CAT_REJ_BAD_MEDIA,
    HTTP_CAT_REJ_TOO_MANY_FIELDS,
    HTTP_CAT_REJ_UNTERMINATED_QUOTE,
    HTTP_CAT_REJ_TEXT_AFTER_QUOTE,
    HTTP_CAT_REJ_EMPTY_PATH,
    HTTP_CAT_REJ_PATH_TOO_LONG,
    HTTP_CAT_REJ_ABSOLUTE_URL,
    HTTP_CAT_REJ_ABSOLUTE_PATH,
    HTTP_CAT_REJ_BACKSLASH,
    HTTP_CAT_REJ_EMPTY_SEGMENT,
    HTTP_CAT_REJ_PARENT_SEGMENT,
    HTTP_CAT_REJ_UNKNOWN_EXTENSION,
};

typedef struct
{
    char startup[HTTP_CAT_STARTUP_MAX + 1];
    char title[HTTP_CAT_TITLE_MAX + 1];
    char path[HTTP_CAT_PATH_MAX + 1];
    char extension[5];
    unsigned char media;
    unsigned char supported; // 0 = listable but not launchable (a .zso today)
} http_catalog_row_t;

// Parse one line. `line` is modified in place (fields are terminated and quotes unescaped), and
// must already have had its newline, CR, leading blanks and comment check applied by the caller.
// Returns HTTP_CAT_OK and fills `row`, or the reason it was refused.
int httpCatalogParseLine(char *line, http_catalog_row_t *row);

// Is this line one the caller should skip entirely rather than parse? Blank or '#'-led, after
// leading blanks. Returns a pointer to the first meaningful character, or NULL to skip.
char *httpCatalogSkipBlankOrComment(char *line);

#endif
