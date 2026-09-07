# RetroAchievements

RiptOPL can report a running game's memory to a PC client that runs
[rcheevos](https://github.com/RetroAchievements/rcheevos) on the console's behalf, so achievements
unlock on real PlayStation 2 hardware.

The console does two things: it works out **which** game an image is (a hash), and once the game is
running it streams the handful of memory addresses that game's achievements depend on. Everything
else — talking to the RetroAchievements servers, deciding what unlocked, your login — happens on the
PC.

Upstream design and implementation are by **hacan359 (yoba)**, adopted here with his permission
(2026-08-27). He also supplies and maintains the PC client; RiptOPL does not ship one and will not
grow one.

---

## Status

**Implemented, not yet validated on PS2 hardware.** Every part below exists in the build; not one of them
has run on a PlayStation 2. Treat the RA variant as a development build until that changes.

| Part | State |
| --- | --- |
| `RETROACHIEVEMENTS=1` build flavour | done |
| In-game telemetry (`raudp`, ee_core snapshots) | written, **not hardware-tested** |
| Watch-list loading from `<device>/RA/<serial>.wl` | done |
| Image hashing (ISO9660 walk + MD5) | done |
| Menu actions: check game support, test PC link | written, **not hardware-tested** |
| List badges and the cover mark | written, **not hardware-tested** |
| Unlock overlay | written, **not hardware-tested** |
| Settings: RA telemetry, RA badges | done |
| Disc-in-the-tray as a game source | deliberately deferred |

---

## Using it

Two settings, on the **Network** settings page:

| Setting | Default | Effect |
| --- | --- | --- |
| RetroAchievements telemetry | **Off** | The master switch. While it is off an RA build launches a game for exactly what the standard build costs — no in-game network stack, no `raudp`, no snapshots. |
| RetroAchievements badges | On | The `RA ` prefix in the game list and the mark over the cover. Cosmetic, and it follows the master switch: badges stay dark while telemetry is off. |

Two actions on a game's own menu:

* **check game support** — hashes the image, asks the PC client about it, and writes
  `<device>/RA/<serial>.wl` if the game is tracked.
* **test PC link** — confirms the console can find the client at all, without touching a game.

A game the console has a watch list for shows the badge and the cover mark after the next list
refresh. Switch telemetry on and launch it.

---

## Which launch paths can work

Telemetry lives inside OPL's own loader core, which only exists for launches OPL performs itself.

| Launch path | Supported |
| --- | --- |
| BDM — USB, iLink, MX4SIO, ATA/exFAT | yes |
| ETH / SMB | wired, but **known bad upstream** — see below |
| HDD (APA) | yes — but see the note below |
| MMCE | yes |
| HTTP | local watch-list loading is wired into OPL-core launch; combined HTTP/RA operation is not hardware-validated, and it shares the ETH/SMB risk below |
| Neutrino core (`$CoreLoader`) | **no** |
| UDPFS | **no** |
| PS1 / VCD (POPSTARTER, Ember) | **no** |

Neutrino, UDPFS and PS1 hand the console over to an external ELF and never load OPL's core, so there is
nothing to take a snapshot from. This is structural, not an oversight.

### Do not expect a game with achievements to run from a network share

A game that streams its own disc over the NIC is competing with telemetry for that NIC. This build no
longer *reads* from the network in play on such a launch — `raudp` stops walking the SMAP receive ring
and stops polling the stack, because the game's disc stream arrives through both — but **hacan359
reports on hardware (2026-09-05) that this is not enough: a game with an achievement set still stops
loading about a minute in from a share, so the send path disturbs the stream on its own.** xeRAbora's
own documentation says the same, and names the USB stick and the original disc as the only tested ways
to play.

That defect is upstream's and ours alike; we have not reproduced or ruled it out on our own hardware,
because none of this has been hardware-tested here yet. Until it is, **keep the images you play with
achievements on a local device.** The same reasoning applies to HTTP, which streams down the same path.

HDD (APA) games are stored in HDLoader format and have no image file to hash, so while a watch list
placed by hand still loads and streams, the console cannot work out the hash for them itself.

## Networking

Telemetry is UDP, sent by an IOP module that builds Ethernet frames by hand rather than going through
the network stack — the stack's mailbox is shared with the game's own traffic, and 60 sends a second
pushed the game out of it.

Because of that, **the console needs a valid IP even when the game was not launched over the
network.** OPL's own network settings supply it: whatever address, netmask and gateway are configured
there are handed to the in-game Ethernet driver on every RA launch. If those are wrong or unset, the
game runs normally but nothing reaches the PC.

The PC client is found automatically: the console broadcasts a query on UDP port 18194 and the client
answers. Nothing is stored between runs.

---

## Files on the card

```
<device root>/RA/<serial>.wl        e.g. mass0:/RA/SLUS_210.65.wl
```

One watch list per tracked game, generated by the PC client from that game's achievement set. `RA/` follows the source’s support-file prefix, like `CHT/`. HTTP loads an existing watch list from the local settings home because the server is read-only; do not assume automatic image hashing/support detection works for a remote HTTP ISO. RiptOPL creates it along with the other library folders.

A missing list is not an error. It means the game is simply not tracked, and it launches normally with
no telemetry and no extra modules loaded.

---

## Building

```bash
make clean && make RETROACHIEVEMENTS=1 release
```

`make clean` is not optional when switching the flag: it changes the layout of a structure and an
enumeration shared between the menu and the loader core, and make cannot see that a stale object file
is now wrong.

The rolling release publishes the flavour as its own archive, **`RIPTOPL-RA-*.zip`**, built to the
same shape as the main package: `POPS/`, `EMBER/`, `neutrino/` and the PC-tool shortcuts, with
`APP_RIPTOPL-RA-<flavour>/RIPTOPL.ELF` in place of the standard loader (`-nopademu` folders carry the
`PADEMU=0` builds). It also carries **`xeRAbora.url`**, because the loader does nothing without the
PC client.

It is deliberately *not* an entry in `RIPTOPL-VARIANTS-*.zip`: that archive is a ~120 MB bag of every
build permutation, and the release workflow excludes it from the permanent MEGA archive as a
diagnostic bundle rather than installable payload. The default build is unaffected by all of this — every RA source file and
every call site is behind `#ifdef RETROACHIEVEMENTS`, and that is checked by comparing the two builds'
symbol tables, loader core and embedded IOP modules.

## Before this is called finished

Nothing below has been done. The feature is written end to end and builds clean; it has not run on a
PlayStation 2. Emulator testing gets you as far as: the menu boots, the RA entries render and
navigate, the flag-off build is unchanged, and a game with no `.wl` does not crash. That is the
ceiling — GS raster timing and real SMAP behaviour are not testable there.

Hand testers a **run-pinned nightly.link build**, never a bare artifact link.

| # | Test | Expected |
| --- | --- | --- |
| 1 | Default build (`RETROACHIEVEMENTS=0`) | no regression on any existing path |
| 2 | RA build, telemetry Off | behaves as the default build |
| 3 | USB launch, valid `.wl`, telemetry On | ~60 snapshots/s for ≥5 min, skip/fail counters flat |
| 4 | Same, watching frame rate | no perceptible impact |
| 5 | SMB launch with telemetry | the game's own SMB stream unaffected |
| 6 | Menu check, protocol = SMB | title + counts, `.wl` written |
| 7 | Menu check, protocol = Off | RA raises the stack, same result |
| 8 | Menu check, protocol = UDPBD / UDPFS | clean refusal + hash written to `RA/hashes.txt`, no wedge |
| 9 | Neutrino-core game | clean refusal, no hash attempt |
| 10 | PS1/VCD entry | clean refusal |
| 11 | HDD/APA entry | the check refuses; a watch list still loads |
| 12 | Badge + cover mark | appear after refresh, correct game, correct device |
| 13 | Unlock overlay | gold pulse on `RAU1`, game keeps running |
| 14 | IGR combo during an RA launch | still resets |
| 15 | MX4SIO launch with telemetry | no navigation or art regression (that device has history) |

---

## Notes for anyone changing this code

* **Telemetry requires an OPL-core launch.** BDM, ETH/SMB, HDD/APA and MMCE have this path; HTTP also loads an existing watch list from its local settings prefix. Neutrino and the PS1 cores run external ELFs, so OPL's telemetry core does not run there. Treat HTTP/RA interoperability as unvalidated until tested.

* **The badge cache belongs to the I/O thread.** `raBadgeRefresh` frees and reallocates it. Nothing
  on the render path may read it — that is why a row carries `raBadged` as a plain int, resolved
  when the row is built. Re-introducing a `raBadgeHas`-style call from a draw routine re-introduces
  a use-after-free that only shows up as a rare hang.
* **`MSG_DONTWAIT` is `0x08` on the menu path**, from ps2sdk's `tcpip.h`. The `0x40` in SMSTCPIP
  belongs to the in-game stack that serves `raudp`. They are different stacks; do not unify them.
* **The texture table is positional.** `RA_MARK` in `enum INTERNAL_TEXTURE` and its row in
  `internalDefault[]` are guarded by the same `#ifdef` for that reason. Guard one without the other
  and the array silently goes one short.
* **Deleting `obj/` is mandatory** after any change to `EECoreConfig_t`, `OPL_MODULE_ID` or the
  submenu structs. This Makefile does not track header dependencies, so an incremental build can
  pass locally while CI's `make clean` fails.
* **`ee_core` has about 3 KB of headroom, and RA spends it.** ram84 is 77312 bytes
  (`ee_core/linkfile`). Without RA, an `EXTRA_FEATURES=1` ee_core comes to 74215; with RA and
  without extras, 74067. Either fits; both together overflow by roughly 6.5 KB and `ld` refuses
  with `.bss is not within region ram84`. That is why the release builds RA at the default
  `EXTRA_FEATURES=0`, which is also what the main loader ships as — so the RA archive is the
  shipping loader plus achievements, not a variant. `ra_snap_buf` and `ra_watch` are most of RA's
  share; shrink them before trying to add anything else to that build.
* **Do not build a PC client.** hacan359 provides and maintains xeRAbora. Do not commit `pc/RA/`.
* **Disc mode is deferred**, deliberately, as a separate game source. It is not a missing piece of
  this work.

---

## Credits

* **hacan359 (yoba)** — the RetroAchievements design, the console implementation this port follows,
  and the PC client.
* `src/md5.c`, `include/md5.h` — L. Peter Deutsch, zlib licence, vendored unchanged.
