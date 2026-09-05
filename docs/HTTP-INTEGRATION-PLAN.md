# HTTP integration plan (RiptOPL)

**Status:** approved in principle, nothing implemented. This document is the brief for the
implementing agent.
**Branch:** `codex/http-protocol`, cut from `origin/rebuild/main`.
**Base audited:** `rebuild/main` @ `24f72490` (the source citations below were re-verified against it).
**Donor audited:** `Docmine17/Open-PS2-Loader-HTTP`, `master` @
`6fced11a6afafe20c52b8d1a090067e3e1889b99`. Feature diff `e62872f..6fced11a`, 24 files, +750/-7.
**Shipping shape:** HTTP appended to the existing Network protocol selector, with its own game
source and its own in-game cdvdman driver. No new build flavour — unlike RetroAchievements, this is
a normal feature of the default ELF.

---

## 0. Read this first

### 0.1 Permission and provenance

The upstream work is **Docmine17**. Permission to adopt was granted by the author, who wants the
approach to become a shared convention and offered it here first. He also supplies the PC side:
`pc/http_server.py` in his tree, a ~90-line `ThreadingHTTPServer` subclass that serves the process
working directory with byte-range support and defaults to **port 1100**
(`pc/http_server.py:66-75` in the donor tree).

Preserve his attribution, the AFL-3.0 notices, the donor SHA above, and a record of what we changed
and why. He is not writing our client and we are not rewriting his server.

**His unmodified server is a release gate, not a fixture.** Anyone already running it, with their
existing `games.csv` and their existing folder layout, must be able to point RiptOPL at it and have
it work. No replacement server, no new API, no mandatory catalog header, no regeneration step, no
capability handshake.

### 0.2 Port, don't merge

The donor's feature diff is small, but it is threaded through `src/ethsupport.c`, `src/gui.c` and
`src/system.c` — three of RiptOPL's most diverged files — and it is built on a design decision we
are deliberately not taking: it rides *inside* SMB's settings, SMB's game source and SMB's mode.
Ours is a peer protocol with its own source.

Take the byte-range reader and the wire shape. Re-author the integration. `git cherry-pick` and
three-way merge will both produce garbage here.

The parts that map closely are `modules/iopcore/cdvdman/http.c`, `http.h` and `device-http.c`. The
menu side does not map at all.

### 0.3 Standing repo rules that bite this feature

1. **`rebuild/main` is the publishing branch.** `master` is dead lineage. Base everything on
   `origin/rebuild/main`.
2. **Deviation rule.** Copy upstream *behaviour*, restructure freely. Do not change behaviour
   without evidence.
3. **Language strings are APPEND-ONLY.** New `_STR_HTTP_*` labels go at the **end** of
   `lng_tmpl/_base.yml`; `.lng` files are consumed by line position. `lang_autogen.h` and
   `lang_internal.c` are gitignored build artifacts — never commit them.
4. **Art lives in the ART folder.** One art location per type; only the key varies. Do not invent a
   second lookup path for HTTP covers.
5. **Folders vs settings.** Settings go to CWD; library folders go to the ROOT of each activated
   device. HTTP is the one source with no local device root — see §2.2, which is an explicit
   documented exception, not a silent fallback.
6. **Use the Write tool for any file containing a literal backslash.** Quoted heredocs collapse
   escapes at the tool layer. `src/bdmsupport.c` also contains 2 NUL bytes, so `grep` treats it as
   binary — use `grep -a`.
7. **CI must always pass.** `check-format` is *not* a required check, so a red format run still
   merges. Build **and** run clang-format 12 locally before pushing.
8. **`SifExitRpc()` must stay** before `ExecPS2()` in elfldr.
9. **Deleting `obj/` is mandatory** after any change to a struct shared between EE and IOP. This
   Makefile does not track header dependencies. This feature changes
   `modules/network/common/httpclient.h`, which is exactly such a struct — see §2.3.
10. **`docs-sync.yml` owns the README's External Tools section.** Hand edits to `README.md` there
    are deleted on the next push. The PC server entry goes in the workflow template.

---

## 1. How the feature works

Two halves that share only a URL and a file format.

### 1.1 Menu half — the catalog

Runs on the EE in the OPL menu, over the **full lwIP stack** (`netman` + `smap` + `ps2ip` +
`ps2ips`), through the existing HTTP RPC client that `src/ethsupport.c:296-350` already loads.

```
console -> GET /games.csv HTTP/1.1                       (TCP, default port 1100)
PC      -> 200 + CSV body
console -> GET /<relative iso path> HTTP/1.1
           Range: bytes=<start>-<end>
PC      -> 206 + Content-Range + Content-Length + body
```

The catalog is the entire record of what exists. There is no registry, no discovery daemon and
nothing stored between runs.

### 1.2 Game half — byte ranges

Runs on the IOP after the reset, in a dedicated `http_cdvdman.irx`, over the **in-game SMSTCPIP
stack** — a different lwIP fork from the menu's, with a different socket API surface (§2.1).

`device-http.c` maps a requested LSN and sector count to a byte interval; `http.c` issues one
`Range:` GET per read over a persistent TCP connection and copies the body into the sector buffer.

### 1.3 What this means for scope

HTTP is a transport for OPL's own loader core. The launch paths that hand the console to an
external ELF never load `ee_core`, so they cannot carry it:

| Launch path | HTTP-capable |
| --- | --- |
| OPL core | yes — this is the target |
| Neutrino core (`$CoreLoader`) | **no** — needs its own HTTP backend |
| POPSTARTER / Ember (PS1/VCD) | **no** |

This is structural. Gate it explicitly rather than letting a stale saved `$CoreLoader` walk a game
into a core that cannot read it — `src/guigame.c:1224` already does exactly this for SMB and is the
pattern to copy.

---

## 2. Three decisions the planning pass left open — settled

The planning pass ended with two engineering questions marked "must be settled from source evidence
before implementation starts". Both are settled below. A third fell out of settling them.

### 2.1 The bounded timeout and cancel mechanism, per stack

**The two stacks have different answers, and conflating them is the trap.**

**Menu side — already solved, nothing to build.** The menu links the full ps2sdk `ps2ip.irx`, which
exports `lwip_select` and `lwip_shutdown`; `modules/network/httpclient/imports.lst` already imports
both. The shipping menu HTTP client already bounds its reads with a 10-second `select()`
(`modules/network/httpclient/httpclient.c:48-70`), with the comment saying why: it guards against a
deadlock when a connection breaks and the peer's RST is lost. Reuse it.

**In-game side — the export table is the constraint.** SMSTCPIP *compiles* `lwip_select`
(`modules/network/SMSTCPIP/sockets.c:658`), `lwip_shutdown` (`:880`) and `lwip_ioctl` (`:1343`), but
`modules/network/SMSTCPIP/exports.tab` publishes **none** of them — ordinals 14 (`lwip_select`), 15
(`lwip_ioctl`) and 18 (`lwip_getsockopt`) are all `ps2ip_Stub`, at `exports.tab:17`, `:18` and `:21`.
`lwip_setsockopt` *is* exported (`:22`), but this lwIP fork has no `SO_RCVTIMEO`/`SO_SNDTIMEO`, so
it buys nothing here.

What is reachable today, without touching the table:

* `MSG_DONTWAIT` — **`0x40` in SMSTCPIP** (`modules/network/SMSTCPIP/include/lwip/sockets.h:122`),
  honoured in `lwip_recvfrom` at `sockets.c:367`, and `lwip_recv` is exported. A poll loop against a
  `GetSystemTime()` deadline gives a bounded **read**.
* Nothing gives a bounded **connect**. `lwip_connect` blocks and FIONBIO is unreachable.

> **`MSG_DONTWAIT` is `0x08` on the menu path** (ps2sdk `tcpip.h`) and `0x40` here. Different
> stacks. Do not unify the constants.

**Recommendation: promote `lwip_select` into ordinal 14, its existing stub slot.** The function is
already compiled into `sockets.o` and therefore already occupying IOP memory; the cost is one export
entry. Ordinals do not move, so `smb.c`, `raudp` and the udpfs modules are unaffected — this is the
same ABI-safe move RA made when it added `etharp_lookup_mac` at ordinal 42
(`exports.tab:46`). Promote ordinal 15 (`lwip_ioctl`) too if a bounded connect turns out to be
needed. **Measure the built IRX with `nm` and compare module size before and after** — the claim
that it is already linked in is an inference from it being in the same translation unit, not a
measurement.

**Correction to an earlier finding.** The planning pass listed the donor's "retries connect forever"
(`modules/iopcore/cdvdman/http.c:57-62`) as a donor defect. It is not — it is a verbatim copy of
OPL's own shipping SMB driver, `modules/iopcore/cdvdman/smb.c:137-142`, which does the identical
`while (1) { connect; DelayThread(500); }`. So bounded connect is a **new behaviour for the whole
in-game tree**, not a port defect to fix in passing. Decide deliberately whether HTTP should be the
first driver to have it, or whether it should match its siblings for now and be changed for all of
them separately.

### 2.2 Where HTTP per-game metadata is written

**HTTP has no filesystem, so it cannot do what the other sources do.** Every existing source hands
`sbPopulateConfig()` a real ioman prefix and lets ordinary `open()` resolve it —
`src/ethsupport.c:1099` passes `ethPrefix` with a `\` separator, `src/udpfssupport.c:485` passes
`udpfsPrefix` with `/`. `sbPopulateConfig()` at `src/supportbase.c:957` then builds
`<prefix>CFG<sep><key>.cfg` and opens it. A catalog plus a cdvdman driver is not a filesystem, and
the first release deliberately does not build an `http:` ioman device.

**Answer: HTTP's per-game CFG/ART/CHT root is the settings home, `configGetDir()`
(`src/config.c:283`).** That is the only writable local location HTTP can be sure of.

The sub-question — *what if the settings home is itself on SMB?* — does not arise, and the code
already guarantees it. `src/opl.c:2660` sets `gBootHomeDeferred` for any `smb`/`udpfs` boot dir and
**keeps the memory card as the config home**, with a comment recording exactly why: the earlier
behaviour migrated the home onto the share and produced the "my settings save but never load back"
reports. So the settings home is always local and writable by the time an HTTP source exists.

Record this as a documented exception to the folders-vs-settings doctrine in §0.3.5, in the code, at
the call site. Never let an HTTP URL become a config filename.

### 2.3 The menu RPC ABI cannot carry this feature as it stands

This was not on the open list and should have been. Three hard limits in
`modules/network/common/httpclient.h` and `modules/network/httpclient/main.c`:

| Limit | Value | Consequence |
| --- | --- | --- |
| `DmaBuffer` | **512 bytes** (`main.c:22`) | Any response over 512 bytes is **silently truncated** — `main.c:38-41` clamps `out_len` and only `printf`s. A 600-byte catalog currently returns a "successful" partial. The donor raised this to 8192; that is a bigger silent truncation, not a fix. |
| `HTTP_CLIENT_URI_MAX` | **128** | A percent-encoded `DVD/Some Long Title (USA).iso` can exceed 127 chars. |
| Range support | **none** | `HttpSendGetRequest()` has no range parameter at all. |

`struct HttpClientSendGetArgs` is shared between EE and IOP, so changing it is a `make clean`
change (§0.3.9), and the existing compatibility-update callers must keep working. Prefer **adding a
new RPC opcode** for bounded streaming reads over widening the existing one, so the old small-download
ABI stays untouched.

Bounded incremental delivery is required, not optional: a partial catalog must never be published as
a successful one.

---

## 3. What exists, and what must change

Every row below was re-verified against `24f72490` and the donor tree at `6fced11a`.

| Area | Confirmed source | Consequence |
| --- | --- | --- |
| Protocol enum | `include/opl.h:259-265`: Off=0, SMB=1, UDPFS=2, UDPFSBD=3, UDPBD=4 | Append `NET_PROTO_HTTP = 5`. Existing persisted values must not move. |
| Source enum | `include/iosupport.h:9-25`, `UDPFS_MODE` last before `MODE_COUNT` | Append `HTTP_MODE` after it, same reasoning ("appended last so existing mode values don't shift"). |
| Mode string | `ee_core/src/main.c:55` compares `"ETH_MODE"` with `_strncmp(..., 8)` | An 8-char **prefix** compare. Name the new mode `HTTP_MODE` so it cannot collide; do not name it `ETH_MODE_HTTP`. Add the reverse mapping at `src/system.c:759`. |
| Module bits | `src/system.c:666-675`, `CORE_IRX_MMCE = 0x200` | The donor's `CORE_IRX_HTTP = 0x200` (`src/system.c:395` in the donor tree) **collides**. It also has no independent consumer. Drop it: HTTP selects `CORE_IRX_ETH` without `CORE_IRX_SMB`. |
| RA network preload | `ee_core/src/iopmgr.c:167-176` loads SMSTCPIP+SMAP for every `GameMode != ETH_MODE` with a watch list | HTTP's own branch will load them, so extend that predicate or HTTP+RA double-loads the stack. |
| Core gating | `src/guigame.c:1224` pins the Loader Core row to `<OPL>` for `ETH_MODE` | Do the same for `HTTP_MODE`, and enforce it on the Favourites path and against stale saved `$CoreLoader` keys. |
| Shared menu networking | `src/ethsupport.c:296-350` loads NetMan, SMAP, ps2ip, ps2ips and the HTTP client; SMB auth is downstream | Extract the smallest common init/config/teardown. HTTP init must not imply an SMB share or session. |
| NIC interlock | `src/gui.c:726-750`, `src/ethsupport.c:300-311`, `src/udpfssupport.c:61-69`, `src/bdmsupport.c:1034` | Extend in all directions. Distinguish "the TCP/IP stack is resident" from "an SMB session is connected" — HTTP must not satisfy an SMB-only check. |
| Donor catalog parse | Donor `src/ethsupport.c:558-580` splits the **first two commas only**, hardcodes `extension = ".iso"`, and derives the filename from the title | Its own README documents `STARTUP,TITLE,MEDIA,FILENAME.iso` — the fourth field is read by nothing. Store title and relative path separately; see §4. |
| Donor media guess | Donor `src/ethsupport.c:587-589`: media is CD if the third field **contains the substring** `CD`/`cd` | Because only two commas are split, that field is the whole rest of the line — so `...,DVD,DVD/CD Game.iso` is misclassified as CD. Match on an exact token. |
| Donor extension steal | Donor `:580-585` moves a trailing `.iso`/`.zso` off the **title** into `extension` | Keep this for legacy rows (it is why `Already Has Ext.iso` works), but note it also silently accepts `.zso` and would stream compressed bytes as raw sectors. |
| No path field | `base_game_info_t` (`include/supportbase.h:22-30`) has `name[161]`, `startup[13]`, `extension[5]` and **no path** | The explicit fourth field has nowhere to live. `httpsupport.c` needs its own parallel record array holding the relative path, keyed by index alongside the `base_game_info_t` array — do not overload `name`. |
| Donor endpoint | Donor `:499` falls back to port **8080** for the catalog; `:721` falls back to **1100** for launch; `:519` uses the share path, `:722` forces `/` | One normalized endpoint for catalog, probe and launch. |
| Donor settings | Donor `src/gui.c:804-807` clears the SMB share field when the HTTP settings dialog opens | HTTP gets its own persisted fields. Opening a dialog must never mutate another protocol's settings. |
| Donor range validation | Donor `modules/iopcore/cdvdman/http.c:250-269` checks `status == 206` and nothing else | It then reads `nbytes` blindly off the socket. A short or mis-ranged reply makes it read the *next* response's headers as disc sectors. Validate `Content-Range` start/end/total and `Content-Length` before returning success. |
| Donor DVD9 | Donor `src/ethsupport.c:726` always sets `layer1_start = 0` | Wide-offset arithmetic is not dual-layer support. Probe the volume descriptors over range GETs and populate it; ours already does the equivalent for SMB at `src/ethsupport.c:1047-1085`. |
| Menu RPC | §2.3 | 512-byte silent truncation, 128-char URIs, no ranges. |

---

## 4. The catalog and wire profile

Compatibility with the donor's existing server and existing catalogs is a firm requirement.
Everything additive below is a **proposal to coordinate with him**, never a prerequisite.

**Catalog.** `GET <base>/games.csv`, complete, unversioned.

```csv
SLUS_123.45,"Example, The",DVD,DVD/Example Game.iso
```

* Fields: `STARTUP,TITLE,MEDIA,RELATIVE_ISO_PATH`. Display title and filename may be unrelated.
* **Legacy rows must keep working.** `STARTUP,TITLE[,MEDIA]` with no path: preserve an existing
  `.iso` suffix in the title-derived filename or append `.iso`; default omitted media to `DVD`.
  That is what the donor's client does today and what existing catalogs rely on.
* **A four-field row's path is authoritative.** Never fall back to a title-derived name after a 404
  — that can silently launch a different disc.
* Define UTF-8, LF/CRLF, no-final-newline, blank and `#` comment lines, quoting for embedded commas
  and quotes, duplicate IDs, and field/record limits. One bounded parser, shared with the fixtures.
  Do not infer media from a substring of the rest of the line.
* Keep the original unescaped path in the record; encode path segments once at request time.
  Preserve `/` separators; handle spaces, `%`, `#`, `?`, commas and non-ASCII consistently. Reject
  control characters, absolute URLs and paths escaping the base.
* Deliver incrementally with bounded buffers; bound total memory and game count and *report* an
  over-limit catalog. A successful empty response clears the list. A failed or truncated one must
  not replace the last good list with an apparently-successful partial.
* Existing `.zso` rows stay subject to the deferred ZSO capability — never stream compressed bytes
  as raw sectors.

**Reads.** One inclusive interval per request: `Range: bytes=<start>-<end>` plus
`Accept-Encoding: identity`. Require `206`, matching `Content-Range` start/end/total, matching
`Content-Length`, and exactly that many payload bytes.

* Do not require suffix ranges, multi-ranges or open-ended reads.
* His server returns **416 without a `Content-Range` header** when the end offset exceeds EOF, so
  accept that terminal error without demanding the header, and get the total size from a valid small
  range first.
* Do not rely on `Accept-Ranges`: his server sends it on error responses too.
* Parse header names case-insensitively; handle split headers and short reads; bound total header
  bytes. Reject chunked, content-encoded and multipart replies in this profile. Close the socket
  whenever framing is uncertain.
* Keep-alive close → fresh socket, retry the same idempotent range within a measured budget. 404,
  416, malformed ranges and a changed image size are **terminal**, not reasons to reconnect.
* Validate the expected total size across reads. Swapping the ISO under a running game is outside
  the contract.
* Use wide arithmetic from LSN through range formatting. Check the length multiply, the inclusive
  end, zero-length requests and EOF. Validate the donor's hand-rolled decimal converter against an
  independent oracle at the 2 GiB, 4 GiB and dual-layer boundaries.

---

## 5. User-facing behaviour

Keep the existing Network page and the Network start row (Off / Manual / Auto). Append **HTTP** to
Protocol. HTTP is Files access; lock or hide the Files/IMG row for it.

An HTTP group, shown only when HTTP is selected:

| Row | Default |
| --- | --- |
| Server IPv4 address | — |
| Port | **1100**, to match his server; any valid port configurable, including 80 and 8080 |
| Base path | `/` |
| Catalog filename | `games.csv`, fixed for now |
| Test server | checks the catalog, parses it, and verifies one small ISO range and the total length when the catalog has a game |

An empty catalog is valid but proves nothing about range support — say so in the result.

Reuse the global PS2 network settings, static and DHCP alike, through the existing full menu stack.
Do not apply the UDP-only static-address restriction to HTTP just because it is another network
option; validate both on hardware.

Selecting a protocol may differ from the resident one until restart — keep today's restart prompt;
no hot stack replacement in the first release. A partial module load must leave truthful residency
and must not let another driver claim SMAP.

Distinct errors, not one generic failure: cannot connect / catalog missing / catalog invalid /
catalog too large / server ignores ranges / unsupported encoding / ISO missing / read timed out.

Discovery and refresh run on the existing IO worker. **The render and input loop must never wait on
an HTTP request.**

---

## 6. Initial capability policy

| Capability | First release |
| --- | --- |
| Raw PS2 ISO, CD/DVD5, OPL core | required |
| DVD9 / reads past 4 GiB | must pass high-offset **and** layer-transition tests before it is advertised |
| Local per-game settings, last-played | required, via §2.2 |
| Favourites, game info | required — real source resolution, stable identity, local config destination |
| Local artwork, themes, cheats | explicit local asset root; missing media surfaces normally |
| Remote ART/CFG/CHT over HTTP | follow-up; a remote CFG must never replace the local write destination |
| Physical memory cards | the save path |
| HTTP VMC, writes, rename, delete | disabled |
| Local USB VMC with an HTTP ISO | separate mixed-backend work; the donor passes no VMC module, so promise nothing |
| Neutrino | unavailable until it has its own HTTP backend |
| POPSTARTER / Ember / PS1 | out of this source |
| ZSO, UL/split images | deferred — do not infer ZSO support from a `.zso` extension being accepted |
| HTTPS, auth, redirects, WebDAV | outside the initial LAN profile |
| RA build | must build and must not disturb other sources; HTTP+RA telemetry is advertised only after §3's double-load fix is validated |

Where an existing capability flag expresses the restriction, use it. If one must be added, append
the minimum field with a safe default for existing positional `item_list_t` initializers. Do not
redesign the source interface for this feature.

---

## 7. Phased work plan

Each phase ends with both pinned and rolling toolchain builds and a clang-format 12 run. Keep the
phases as separate commits.

**Phase 1 — Freeze the contract and the fixtures.** Record donor SHA and attribution. Pin his
unmodified server. Write catalog fixtures (documented four-field, legacy two/three-field, quoted,
CRLF, oversized, empty, malformed) and range fixtures. Write the host-side conformance harness.
*Gate:* his existing setup works unchanged against the specified client contract; fixtures and
limits are written down before any porting.

**Phase 2 — Separate shared networking, and settle the export table.** Extract the smallest menu
TCP/IP init/config/teardown from `ethsupport`; keep SMB session state where it is. Represent
readiness and partial residency truthfully. Promote `lwip_select` at ordinal 14 and **measure the
IRX size delta**. *Gate:* SMB browse and launch, the compatibility updater and every UDP source
still initialize and shut down correctly, including on partial failure. Review this independently —
it changes existing network behaviour before HTTP exists.

**Phase 3 — Menu RPC, settings and the HTTP source.** Bounded streaming reads over a new RPC
opcode; wider URIs; `NET_PROTO_HTTP`, `HTTP_MODE`, persistence, validation; the endpoint controls;
`src/httpsupport.c` + `include/httpsupport.h` as an `item_list_t`; streaming catalog parse; the
test-server action; the local metadata destination from §2.2. *Gate:* clean settings round trips;
correct behaviour on large, empty and malformed catalogs; repeated refresh; title and path stay
separate. **A visible game list is explicitly not launch proof.**

**Phase 4 — The in-game driver.** A `USE_HTTP` cdvdman target, embedded IRX, packed settings, the
appended game mode, module selection, IOP-reset loading, shutdown and IGR. Exact range validation,
bounded reconnection, wide-offset correctness, volume-descriptor probes for `layer1_start`. Fix the
RA preload predicate. Copy startup, URI and endpoint into launch-owned storage before cleanup frees
the catalog records; close menu sockets before teardown; never carry a menu socket across the reset.
*Gate:* byte-correct reads and a CD/DVD5 game through sustained play. DVD9 is a separate gate.

**Phase 5 — Library integration.** Favourites launch and settings, last-played, the global core
default, local cheats/GSM/PADEMU, refusal of unsupported actions, localized labels, appended theme
texture IDs, device help. *Gate:* HTTP behaves as a normal source, and no unsupported capability is
reachable through an alternate UI route.

**Phase 6 — Release validation and documentation.** Four toolchain flavours plus the affected
RA/debug/PADEMU combinations. Check embedded IRX symbols, module manifests and the real ELF
footprint. `docs/HTTP.md`, setup instructions, the docs-sync template for the PC server link,
release notes. *Gate:* documented interoperability, the console matrix, and no regressions.

These are review boundaries, not intermediate user releases. Keep the feature in draft until the
claimed path passes its gates.

---

## 8. Traps, in one place

* **`MSG_DONTWAIT` is `0x08` on the menu path and `0x40` in SMSTCPIP.** Two different lwIP forks.
  Never share the constant, and never assume a menu-side socket idiom works in-game.
* **`lwip_select`, `lwip_ioctl` and `lwip_getsockopt` are compiled but not exported** in SMSTCPIP.
  Ordinals 14/15/18 are `ps2ip_Stub`. Promoting one is ABI-safe because ordinals do not move; adding
  one anywhere else in the table is not.
* **The menu HTTP RPC silently truncates at 512 bytes.** It logs a `printf` and returns success.
  Anything built on it without fixing that will ship a partial catalog that looks fine.
* **`ee_core/src/main.c:55` compares mode strings with an 8-char prefix.** Name the mode
  `HTTP_MODE`.
* **`CORE_IRX_MMCE` already owns `0x200`.** The donor's `CORE_IRX_HTTP` collides with it and is
  unnecessary anyway.
* **The RA network preload fires for every mode that is not `ETH_MODE`** — `ee_core/src/iopmgr.c:173`.
  HTTP will double-load SMSTCPIP and SMAP unless that predicate is extended.
* **The donor validates only the status code on a range read.** Blindly reading `nbytes` after a
  short or mis-ranged 206 feeds the next response's headers into the sector buffer as disc data.
* **Infinite connect retry is OPL's own behaviour**, not the donor's — `smb.c:137-142`. Changing it
  for HTTP alone makes the in-game drivers inconsistent; decide that deliberately.
* **HTTP has no ioman filesystem.** Never pass an HTTP URL to `sbPopulateConfig()`, `open()` or
  `stat()`. §2.2 is the destination.
* **`struct HttpClientSendGetArgs` is shared EE/IOP.** Change it and `obj/` must be deleted; an
  incremental build will pass locally and CI's `make clean` will not.
* **His server sends `Accept-Ranges` on error responses** and omits `Content-Range` on 416. Do not
  key correctness off either.

---

## 9. Verification

### Host-side

* Catalog: documented four-field and legacy title-derived rows; root-level ISOs with and without
  `.iso` in the title; the explicit fourth path never falling back after an error; quoted commas and
  quotes; LF/CRLF; no final newline; blank and comment lines; invalid media and startup; duplicate
  IDs. Sizes under, at and far over the transfer buffer; the entry-count limit; empty; truncated;
  allocation failure.
* Paths: root and subdirectory bases, encoded reserved characters, length boundary, non-ASCII
  policy, server case sensitivity.
* Framing: mixed-case headers, arbitrary TCP splits, headers plus body in one packet, short body
  reads, oversized and malformed headers.
* Responses: 200 despite a Range, wrong 206 start/end/total, bad `Content-Length`, 404, 416,
  redirects, chunked, compressed, stale or replaced ISO.
* Failures: short send, EOF, timeout, reset during headers and mid-body, idle keep-alive close,
  retry exhaustion. **Confirm a failed request is never reported as a successful disc read.**
* Byte-for-byte comparison against synthetic reference data around 2 GiB, 4 GiB and the final
  sector.
* **Mandatory: the exact unmodified donor server**, at default 1100, with an unversioned catalog,
  root-level ISOs, nested paths, encoded names, keep-alive, final-sector reads, large offsets, 404
  and 416. Then two other independent range-capable servers. Save traces and exact versions.

### Build and ABI

* Clean `PS2DEVPINNED`, `OFFICIALPINNED`, `PS2DEVROLLING`, `OFFICIALROLLING`.
* RA on/off, PADEMU on/off, debug module loading. Confirm no mode double-loads SMAP or SMSTCPIP.
* HTTP target imports, export ordinals, packed settings offsets, module IDs, embedded blob
  generation, marker discovery, clean targets.
* Measure IOP free memory and module sizes, including the SMSTCPIP export change.
* Confirm SMB/UDPBD/UDPFS/MMCE mode numbers and saved config values are unchanged.

### Console

Nothing below has been done; the feature does not exist yet. Hand testers a **run-pinned
nightly.link build**, never a bare artifact link.

| # | Test | Expected |
| --- | --- | --- |
| 1 | Every existing source with HTTP compiled in | no regression |
| 2 | Protocol = HTTP, server absent | clean error, no wedge |
| 3 | CD and DVD5 boot from his unmodified server | sustained play, FMVs, random seeks |
| 4 | Long play with saves to a physical card | no read faults |
| 5 | DVD9 with a demonstrated layer transition | reaching a logo is **not** sufficient |
| 6 | Reads above 4 GiB, verified byte-exact | correct |
| 7 | Cable drop, server restart, idle keep-alive close | bounded recovery, then a real disc-read error |
| 8 | Terminal malformed response | fails as a read error, never as silent corruption |
| 9 | Static addressing and DHCP, fat and slim | both |
| 10 | HTTP ⇄ SMB ⇄ UDP protocol changes through the restart prompt | settings survive, no cross-contamination |
| 11 | Favourites-origin HTTP launch | same restrictions as a direct launch |
| 12 | Stale saved `$CoreLoader` = Neutrino on an HTTP game | refused, not attempted |
| 13 | RA build, HTTP game with a watch list | stack loaded once, telemetry flows |
| 14 | IGR during an HTTP launch | still resets |
| 15 | Alternate startup ELF | launches |

For every hardware result record the ELF hash, source SHA, BUILD-MANIFEST/toolchain, PS2 model,
server version and configuration, image identity and size, the last successful stage and the
observed outcome. Keep a non-HTTP control on the same game where practical.

---

## 10. Still open

* **Should HTTP be the first in-game driver with a bounded connect?** See §2.1. Either answer is
  defensible; it needs a decision, not a default.
* **Additive catalog extensions** — a version marker, richer quoting — are worth proposing to
  Docmine17, but only as extensions. Existing catalogs must never need conversion.
* **Remote ART/CFG/CHT over HTTP** is the obvious follow-up and is deliberately not in release one.
* **PS2-Servers HTTP support**, if it ever lands, should reuse this catalog and range profile. The
  client must stay usable without it.

---

## Credits

**Docmine17** — https://github.com/Docmine17/Open-PS2-Loader-HTTP

The design and the console implementation RiptOPL's HTTP support follows: serving a library from a
plain static HTTP server, and streaming an ISO into the loader core with ordinary HTTP byte ranges.
Adopted with the author's permission, given directly to Ripto (NathanNeurotic); he wants the
approach to become a shared convention and offered it here first. He also supplies and maintains the
PC server, which RiptOPL does not ship and will not grow a replacement for.

Re-authored against this fork rather than merged, so any defect in the port is ours — his server and
his existing catalogs stay the compatibility baseline they always were.

Behind him, as behind all of it: the **ps2homebrew** team's Open PS2 Loader and the PS2SDK, which
both trees descend from. `Docmine17/Open-PS2-Loader-HTTP` is listed with the rest of the OPL family
in [`CREDITS`](../CREDITS); the PC server gets its External Tools entry at Phase 6, in the
`.github/workflows/docs-sync.yml` template rather than in `README.md` directly.
