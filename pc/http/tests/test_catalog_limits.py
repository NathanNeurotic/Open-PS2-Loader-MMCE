"""Compile the real menu catalog function, without the PS2 UI/network dependencies.

The existing line-parser harness models de-duplication itself. This test extracts the
unchanged function and its record types from production source so it covers the menu's
actual ordering of duplicate detection and capacity checks. Temporary files stay outside
the checkout; no production source is rewritten.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def extract(pattern, text):
    match = re.search(pattern, text, re.S)
    if match is None:
        raise RuntimeError("Production declaration changed: " + pattern)
    return match.group(0)


def main():
    support = (ROOT / "src/httpsupport.c").read_text()
    base = (ROOT / "include/supportbase.h").read_text()
    limits = (ROOT / "include/httpsupport.h").read_text()
    source = """
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/httpcatalog.h"
typedef unsigned char u8;
typedef unsigned int u32;
#define LOG(...) ((void)0)
#define HTTP_STREAM_ERR_TRUNC -7
static int httpLastError;
"""
    source += "\n".join(re.findall(r"^#define (?:ISO_GAME_NAME_MAX|ISO_GAME_EXTENSION_MAX|GAME_STARTUP_MAX|HTTP_CATALOG_\w+)\b[^\n]*", base + "\n" + limits, re.M))
    source += "\n" + extract(r"enum GAME_FORMAT \{.*?\n\};", base)
    source += "\n" + extract(r"typedef struct\s*\{[^}]*\} base_game_info_t;", base)
    source += "\n" + extract(r"typedef struct\s*\{[^}]*\} http_catalog_extra_t;", support)
    source += "\n" + extract(r"static int httpParseCatalog\([^\n]*\n\{.*?\n\}", support)
    source += r'''
int main(void)
{
    char *buf = malloc(HTTP_CATALOG_BYTES_MAX + 1);
    base_game_info_t *games;
    http_catalog_extra_t *extras;
    int scenario, i, len, result;
    assert(buf != NULL);
    for (scenario = 0; scenario < 3; scenario++) {
        len = 0;
        for (i = 0; i < HTTP_CATALOG_ROWS_MAX; i++)
            len += sprintf(buf + len, "G%04d,Original,DVD,original.iso\n", i);
        if (scenario == 1)
            len += sprintf(buf + len, "G0000,Replacement,CD,replacement.iso\n");
        if (scenario == 2)
            len += sprintf(buf + len, "UNIQUE,Extra,DVD,extra.iso\n");
        assert(len < HTTP_CATALOG_BYTES_MAX);
        result = httpParseCatalog(buf, len, &games, &extras);
        if (scenario < 2) {
            assert(result == HTTP_CATALOG_ROWS_MAX);
            assert(strcmp(games[0].name, "Original") == 0);
            assert(strcmp(extras[0].path, "original.iso") == 0);
            assert(games[0].media == HTTP_CAT_MEDIA_DVD);
            free(games);
            free(extras);
        } else {
            assert(result == -1);
            assert(games == NULL && extras == NULL);
            assert(httpLastError == HTTP_STREAM_ERR_TRUNC);
        }
    }
    free(buf);
    puts("PASSED: menu catalog at limit, duplicate at limit, unique beyond limit");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="http-catalog-limits-") as directory:
        path = Path(directory)
        (path / "test.c").write_text(source)
        binary = path / "test"
        subprocess.run([os.environ.get("CC", "gcc"), "-O1", "-Wall", "-Wextra",
                        "-I" + str(ROOT), str(path / "test.c"),
                        str(ROOT / "src/httpcatalog.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
