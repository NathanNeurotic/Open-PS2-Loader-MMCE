# HTTP

RiptOPL can read a game library straight off an ordinary HTTP server: a small `games.csv` catalog
says what exists, and the console streams the ISO itself with plain HTTP byte ranges as the game
asks for sectors. Nothing on the PC side is special — any static server that honours `Range`
requests will do.

The design, the byte-range reader RiptOPL follows, and the PC server are
**[Docmine17](https://github.com/Docmine17)**'s
([Open-PS2-Loader-HTTP](https://github.com/Docmine17/Open-PS2-Loader-HTTP)).

**His unmodified server is the compatibility baseline.** If you already run it, RiptOPL points at
it as-is: same port, same `games.csv`, same folder layout. No new API, no catalog conversion, no
regeneration step, nothing to upgrade.

---

## Status

**Written end to end, verified on nothing.** Every part below exists in the build and none of it
has run on a PlayStation 2. Treat HTTP as a development feature until that changes.

| Part | State |
| --- | --- |
| Catalog fetch and parse | done, and cross-checked against a reference parser |
| Byte-range validation | done, and covered by host tests |
| Network settings, Test action | written, **not hardware-tested** |
| Game list, artwork, per-game settings | written, **not hardware-tested** |
| Launching a game | written, **not hardware-tested** — this is the least proven part |
| DVD9 (dual layer) | probe implemented, **never seen a real dual-layer read** |
| Virtual memory cards | deliberately unavailable |
| Neutrino, PS1/VCD, Ember | deliberately unavailable |
| ZSO / compressed images | listed, refused by name |

---

## Setting it up

On the PC, run any HTTP server that supports byte ranges, with your ISOs under its document root,
and put a `games.csv` beside them. Docmine17's `http_server.py` does exactly this and defaults to
port **1100**.

On the console, **Settings → Network**:

| Row | What it is |
| --- | --- |
| Protocol | choose **HTTP** |
| HTTP server | the PC's IPv4 address |
| HTTP port | **1100** by default; anything valid works, 80 and 8080 included |
| HTTP base path | `/` unless your catalog and images live in a subfolder |
| Test HTTP server | fetches the catalog, parses it, and reads one real byte range |

Use **Test** before saving. It tells you which part failed — unreachable, catalog missing, catalog
unreadable, or the server not returning the exact bytes asked for — rather than one generic error.

An empty catalog is a valid answer, and Test says so plainly: it proves the server is there, and
proves nothing at all about whether byte ranges work.

Switching protocol takes effect after a restart, the same as SMB and the UDP transports. Only one
of them can drive the console's single network adapter.

---

## The catalog

`games.csv` at the root of your base path, one game per line:

```csv
SLUS_123.45,Example Game,DVD,DVD/Example Game.iso
SLUS_200.01,"Example, The",DVD,DVD/example-the.iso
SLES_512.34,Short One,CD,CD/Short One.iso
```

`STARTUP,TITLE,MEDIA,PATH` — the boot identifier, the name shown on screen, `CD` or `DVD`, and the
path to the image relative to your base path. The title and the filename need not resemble each
other.

**Older catalogs keep working untouched.** These are all still accepted:

```csv
SLUS_123.45,Example Game            <- filename derived from the title, media defaults to DVD
SLUS_200.02,Another Game,CD         <- explicit media
SLUS_300.03,Already Has Ext.iso,CD  <- extension already on the title, not doubled
SLUS_400.04                         <- startup only; it doubles as title and filename
```

Blank lines and lines starting with `#` are ignored. Both LF and CRLF work, and a missing final
newline is fine. A field containing a comma or a quote can be double-quoted, `""` being a literal
quote.

A row RiptOPL cannot use is skipped and the rest of the library still loads — one typo does not
cost you every game. What gets a row skipped: no startup ID, a startup longer than 12 characters, a
media field that is not exactly `CD` or `DVD`, a path that escapes your base directory, an absolute
URL, or a filename that is not an image.

If a row names a path explicitly, that path is what gets requested. It is never quietly retried
under a name derived from the title — that is how you end up launching a different disc than the
one you picked.

---

## What works, and what does not

| Feature | Over HTTP |
| --- | --- |
| PS2 ISO, CD and DVD5 | the supported target |
| DVD9 | implemented, unproven — see Status |
| Per-game settings, artwork, cheats | yes, stored locally (see below) |
| Favourites, last played | yes |
| Saving to a physical memory card | yes |
| Virtual memory cards | **no** — nothing writes to the server, so there is nowhere to keep one |
| Neutrino core | **no** — it would need its own HTTP backend |
| PS1 / VCD / Ember | **no** |
| ZSO and other compressed images | **no** — they are listed but refuse to launch |
| HTTPS, passwords, redirects | **no** — this is plain LAN HTTP |

The PS2 logo animation is skipped on HTTP launches. Checking it means reading the logo out of the
image through a file handle, and HTTP has no filesystem to open one on.

### Where per-game data lives

Every other device keeps `CFG/`, `ART/` and `CHT/` at the root of the device the game came from.
HTTP cannot: the server is read-only and there is no drive to write to. So HTTP keeps them **with
your settings** instead — wherever RiptOPL saves its configuration, normally the memory card.

That is the one deliberate exception to the usual rule, and it is why covers and per-game options
for HTTP games go next to `conf_opl.cfg` rather than on the server.

---

## When something goes wrong

RiptOPL refuses rather than guesses. A read is only accepted when the server returns exactly the
bytes that were asked for: the right status, the right byte interval, and the right length. A
server that ignores the range, answers a different interval, or stops early gets a disc read error
instead of quietly feeding the game whatever arrived.

| What you see | What it means |
| --- | --- |
| Cannot reach the HTTP server | wrong address or port, or the server is not running |
| games.csv is missing, unreadable or too large | the catalog did not arrive intact |
| The server did not return the exact bytes | the server does not support byte ranges properly |
| Compressed images are not supported | the row points at a `.zso` |

If the server disappears mid-game, the console retries the same read once on a fresh connection and
then reports a disc error. It will not sit there reconnecting forever.

---

## Notes for anyone changing this code

* **`src/httpcatalog.c` and `pc/http/catalog_reference.py` are one contract.** The fixtures in
  `pc/http/fixtures/` hold them to it and `pc/http/tests/compare_catalog.py` proves it. Change one,
  change all three, in the same commit.
* **Run the host tests.** `sh pc/http/tests/run.sh` compiles the real sources and exercises the
  parsers directly — header framing, range validation, truncation, and every catalog fixture in
  three line-ending forms.
* **`modules/network/common/httpstream.inc` is shared** by the menu RPC module and the in-game
  cdvdman driver. Both IRX rules depend on it in the top-level Makefile, because make cannot see
  inside a sub-make and a stale IRX otherwise builds clean locally and fails CI.
* **The two lwIP stacks are not interchangeable.** `MSG_DONTWAIT` is `0x08` on the menu path and
  `0x40` in SMSTCPIP, and SMSTCPIP exports no `select`, so the in-game reader polls against a
  deadline instead.
* `docs/HTTP-INTEGRATION-PLAN.md` carries the phase ledger, the evidence table and the console test
  matrix. Read it before picking this up.

---

## Credits

* **[Docmine17](https://github.com/Docmine17)** — the HTTP design, the byte-range reader this port
  follows, and the PC server.
