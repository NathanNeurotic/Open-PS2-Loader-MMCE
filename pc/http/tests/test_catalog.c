/* Runs the REAL src/httpcatalog.c over the fixtures and prints one line per outcome.
 *
 * On its own it proves nothing -- compare_catalog.py diffs this output against
 * catalog_reference.py over the same fixtures, in all three line-ending forms. That comparison is
 * the only thing that makes "the two parsers are one contract" a fact rather than a comment.
 *
 * This models the CATALOG, not just the line: first-record-wins de-duplication happens here
 * because httpCatalogParseLine cannot see across lines, and src/httpsupport.c does the same thing
 * for the same reason. Leaving it out made the two sides disagree on duplicates.csv, which is how
 * it got noticed.
 *
 * Output format, one record per line, so the differ can be dumb:
 *   ACC <startup>|<title>|<CD or DVD>|<path>|<supported>
 *   REJ <line-number>|<reason-number>
 *   DUP <line-number>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/httpcatalog.h"

int main(int argc, char **argv)
{
    FILE *f;
    char *buf;
    long size;
    long i, lineStart;
    int lineNo = 1;
    static char seen[512][HTTP_CAT_STARTUP_MAX + 1];
    int seenCount = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <catalog.csv>\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size + 2);
    if (!buf) {
        fclose(f);
        return 2;
    }
    if (size > 0 && fread(buf, 1, size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return 2;
    }
    fclose(f);
    buf[size] = '\0';

    lineStart = 0;
    for (i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            char *line = &buf[lineStart];
            char *trimmed;
            long end = i;

            if (i < size)
                buf[i] = '\0';
            else
                buf[size] = '\0';
            /* strip one trailing CR so CRLF and LF parse identically */
            if (end > lineStart && buf[end - 1] == '\r')
                buf[end - 1] = '\0';

            trimmed = httpCatalogSkipBlankOrComment(line);
            if (trimmed) {
                http_catalog_row_t row;
                int r = httpCatalogParseLine(trimmed, &row);
                if (r == HTTP_CAT_OK) {
                    int d, dup = 0;
                    for (d = 0; d < seenCount; d++) {
                        if (!strcmp(seen[d], row.startup)) {
                            dup = 1;
                            break;
                        }
                    }
                    if (dup) {
                        printf("DUP %d\n", lineNo);
                    } else {
                        if (seenCount < (int)(sizeof(seen) / sizeof(seen[0]))) {
                            snprintf(seen[seenCount], sizeof(seen[0]), "%s", row.startup);
                            seenCount++;
                        }
                        printf("ACC %s|%s|%s|%s|%d\n", row.startup, row.title,
                               row.media == HTTP_CAT_MEDIA_CD ? "CD" : "DVD",
                               row.path, row.supported);
                    }
                } else {
                    printf("REJ %d|%d\n", lineNo, r);
                }
            }

            lineStart = i + 1;
            lineNo++;
        }
    }

    free(buf);
    return 0;
}
