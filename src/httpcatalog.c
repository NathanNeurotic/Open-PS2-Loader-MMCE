// The games.csv parser. See include/httpcatalog.h for why it is its own file.
//
// Depends on nothing but string.h and stdio.h so that pc/http/tests/test_catalog.c can compile it
// on the host and diff it, fixture for fixture, against pc/http/catalog_reference.py.

#include <stdio.h>
#include <string.h>

#include "include/httpcatalog.h"

#define HTTP_CSV_MAX_FIELDS 4

char *httpCatalogSkipBlankOrComment(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '#')
        return NULL;

    return line;
}

// Splits one line on commas with RFC4180 quoting. A double quote only opens a quoted field at the
// START of that field; mid-field it is an ordinary character. That leniency is deliberate and
// donor-compatible: his parser does no quote handling at all, so a title like
// Ready 2 Rumble "Round 2" has always worked and must keep working.
static int httpSplitFields(char *line, char *fields[HTTP_CSV_MAX_FIELDS], int *reject)
{
    int count = 0;
    char *p = line;

    *reject = HTTP_CAT_OK;

    for (;;) {
        if (count >= HTTP_CSV_MAX_FIELDS) {
            *reject = HTTP_CAT_REJ_TOO_MANY_FIELDS;
            return -1;
        }

        if (*p == '"') {
            char *out;

            p++;
            fields[count] = p;
            out = p;
            for (;;) {
                if (*p == '\0') {
                    *reject = HTTP_CAT_REJ_UNTERMINATED_QUOTE;
                    return -1;
                }
                if (*p == '"') {
                    if (*(p + 1) == '"') {
                        *out++ = '"';
                        p += 2;
                        continue;
                    }
                    p++;
                    break;
                }
                *out++ = *p++;
            }
            if (*p != ',' && *p != '\0') {
                *reject = HTTP_CAT_REJ_TEXT_AFTER_QUOTE;
                return -1;
            }
            *out = '\0';
            count++;
            if (*p == '\0')
                return count;
            p++;
        } else {
            fields[count] = p;
            while (*p != '\0' && *p != ',')
                p++;
            count++;
            if (*p == '\0')
                return count;
            *p++ = '\0';
        }

        // A trailing comma yields one final empty field.
        if (*p == '\0') {
            if (count >= HTTP_CSV_MAX_FIELDS) {
                *reject = HTTP_CAT_REJ_TOO_MANY_FIELDS;
                return -1;
            }
            fields[count++] = p;
            return count;
        }
    }
}

static char *httpTrim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
        return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t'))
        *end-- = '\0';

    return s;
}

static int httpHasControlChars(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F)
            return 1;
    }

    return 0;
}

static int httpEndsWithNoCase(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    size_t i;

    if (ls <= lx)
        return 0;
    for (i = 0; i < lx; i++) {
        char a = s[ls - lx + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return 0;
    }

    return 1;
}

// A relative path we are willing to request: nothing that escapes the configured base, carries
// control bytes, or names something that is not an image.
static int httpCheckPath(const char *path)
{
    const char *seg;

    if (path[0] == '\0')
        return HTTP_CAT_REJ_EMPTY_PATH;
    if (httpHasControlChars(path))
        return HTTP_CAT_REJ_CONTROL_CHAR;
    if (strlen(path) > HTTP_CAT_PATH_MAX)
        return HTTP_CAT_REJ_PATH_TOO_LONG;
    if (strstr(path, "://") != NULL)
        return HTTP_CAT_REJ_ABSOLUTE_URL;
    if (path[0] == '/')
        return HTTP_CAT_REJ_ABSOLUTE_PATH;
    if (strchr(path, '\\') != NULL)
        return HTTP_CAT_REJ_BACKSLASH;

    seg = path;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);

        if (len == 0)
            return HTTP_CAT_REJ_EMPTY_SEGMENT;
        if (len == 1 && seg[0] == '.')
            return HTTP_CAT_REJ_EMPTY_SEGMENT;
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return HTTP_CAT_REJ_PARENT_SEGMENT;
        if (!slash)
            break;
        seg = slash + 1;
    }

    if (!httpEndsWithNoCase(path, ".iso") && !httpEndsWithNoCase(path, ".zso"))
        return HTTP_CAT_REJ_UNKNOWN_EXTENSION;

    return HTTP_CAT_OK;
}

int httpCatalogParseLine(char *line, http_catalog_row_t *row)
{
    char *fields[HTTP_CSV_MAX_FIELDS];
    char *startup, *title;
    char derived[HTTP_CAT_PATH_MAX + 1];
    const char *path;
    int count, reject, err;

    memset(row, 0, sizeof(*row));
    row->media = HTTP_CAT_MEDIA_DVD;

    count = httpSplitFields(line, fields, &reject);
    if (count < 1)
        return reject;

    startup = httpTrim(fields[0]);
    if (startup[0] == '\0')
        return HTTP_CAT_REJ_EMPTY_STARTUP;
    if (httpHasControlChars(startup))
        return HTTP_CAT_REJ_CONTROL_CHAR;
    if (strlen(startup) > HTTP_CAT_STARTUP_MAX)
        return HTTP_CAT_REJ_STARTUP_TOO_LONG;

    // Field 2 absent is the donor's startup-only row: the startup is both the displayed name and
    // the filename stem.
    if (count >= 2 && httpTrim(fields[1])[0] != '\0')
        title = httpTrim(fields[1]);
    else
        title = startup;
    if (httpHasControlChars(title))
        return HTTP_CAT_REJ_CONTROL_CHAR;
    if (strlen(title) > HTTP_CAT_TITLE_MAX)
        return HTTP_CAT_REJ_TITLE_TOO_LONG;

    // Field 3 is an EXACT token. The donor substring-matches "CD" against the whole rest of the
    // line -- he only ever splits two commas -- so a four-field row pointing at DVD/CD Game.iso
    // comes out as a CD.
    if (count >= 3) {
        char *m = httpTrim(fields[2]);
        if (m[0] != '\0') {
            if (strlen(m) == 2 && (m[0] == 'C' || m[0] == 'c') && (m[1] == 'D' || m[1] == 'd'))
                row->media = HTTP_CAT_MEDIA_CD;
            else if (strlen(m) == 3 && (m[0] == 'D' || m[0] == 'd') && (m[1] == 'V' || m[1] == 'v') && (m[2] == 'D' || m[2] == 'd'))
                row->media = HTTP_CAT_MEDIA_DVD;
            else
                return HTTP_CAT_REJ_BAD_MEDIA;
        }
    }

    // Field 4, when present, is authoritative. Never fall back to a title-derived name after it
    // misses: that is how a 404 becomes launching a different disc.
    if (count >= 4 && httpTrim(fields[3])[0] != '\0') {
        path = httpTrim(fields[3]);
        snprintf(row->title, sizeof(row->title), "%s", title);
        snprintf(row->extension, sizeof(row->extension), "%s",
                 httpEndsWithNoCase(path, ".zso") ? ".zso" : ".iso");
    } else {
        // Legacy derivation, donor-compatible: an extension already on the title moves to the
        // extension field rather than being doubled.
        size_t tl = strlen(title);
        if (httpEndsWithNoCase(title, ".iso") || httpEndsWithNoCase(title, ".zso")) {
            int i;
            snprintf(row->extension, sizeof(row->extension), "%s", &title[tl - 4]);
            for (i = 1; i < 4; i++) {
                if (row->extension[i] >= 'A' && row->extension[i] <= 'Z')
                    row->extension[i] = (char)(row->extension[i] + 32);
            }
            snprintf(row->title, sizeof(row->title), "%.*s", (int)(tl - 4), title);
        } else {
            snprintf(row->extension, sizeof(row->extension), ".iso");
            snprintf(row->title, sizeof(row->title), "%s", title);
        }
        snprintf(derived, sizeof(derived), "%s%s", row->title, row->extension);
        path = derived;
    }

    err = httpCheckPath(path);
    if (err != HTTP_CAT_OK)
        return err;

    snprintf(row->startup, sizeof(row->startup), "%s", startup);
    snprintf(row->path, sizeof(row->path), "%s", path);
    // .zso parses so an existing catalog still lists, but it must never be streamed as raw sectors.
    row->supported = httpEndsWithNoCase(path, ".iso") ? 1 : 0;

    return HTTP_CAT_OK;
}
