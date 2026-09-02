# Neutrino Core (External Loader)

This fork can hand a game off to an **external [Neutrino](https://github.com/rickgaiser/neutrino) ELF**
instead of OPL's built-in EE core, on a **per-game** basis. This is useful for titles that
boot better under Neutrino, or for users who prefer Neutrino's loader and its launch flags.

> The core selector and the launch-args fields described here are specific to this fork.

## Credit and licence

**Neutrino is created by [rickgaiser](https://github.com/rickgaiser)** — a *"Small, Fast and Modular
PS2 Device Emulator"*. Home: <https://github.com/rickgaiser/neutrino>.

It is an independent project, **not** part of RiptOPL and not our work. The RiptOPL release package
bundles the official latest build, re-fetched from upstream at publish time, under Neutrino's
**AFL-3.0** licence. We add exactly one file to that folder — `config/bsd-udpfsbd.toml`, because
Neutrino ships `udpfs_bd.irx` without a matching `-bsd` token and RiptOPL launches UDPFS as
`-bsd=udpfsbd`. Nothing of rickgaiser's is altered or removed.

Neutrino is deliberately **UI-agnostic**: it has no interface of its own, and that is exactly what
makes a front-end like this one possible. RiptOPL is one of several — **NHDDL**, **XEB+ Plugin**,
**RETROLauncher**, **OSD-XMB** and **PSBBN/BBNL** all drive the same core.

Report *launching* problems here — building the arguments and handing off are ours. Genuine Neutrino
bugs belong on rickgaiser's tracker.

## 1. Install Neutrino

**The RiptOPL release package ships a ready-to-use `neutrino/` folder** — drag it onto a memory card
as `mc?:/neutrino/` and this is already done. (Upstream OPL does not bundle Neutrino; if you built
RiptOPL yourself or are coming from another loader, supply it from
<https://github.com/rickgaiser/neutrino/releases>.)

Either way, copy Neutrino's **whole `neutrino/` folder**, not just the ELF: OPL only accepts an install when `config/system.toml` (or a flat `system.toml`,
the SAS layout) sits beside `neutrino.elf`. An ELF-only folder is **skipped** and probing
continues, so you get the `<OPL>` fallback even though the file exists.

A memory card is the zero-config default — `mc0:NEUTRINO/neutrino.elf`, then
`mc1:NEUTRINO/neutrino.elf` — but it is the **last** place OPL looks, not the first. The full
order is:

| Priority | Where OPL looks |
|---|---|
| 1 | The device chosen in **Game Launching → Neutrino Defaults → Default Device** (see below) |
| 2 | The custom **Neutrino ELF Path** (`neutrino_path`) |
| 3 | The active game's own device — `<games prefix>/neutrino/`, then `<device root>/neutrino/` |
| 4 | `mc0:` / `mc1:` — `NEUTRINO/neutrino.elf` and its lowercase / `NEUTRINO.ELF` variants |

If no complete install is found when you launch a game set to the Neutrino core, OPL shows a
warning and falls back to the `<OPL>` core for that launch.

**Game Launching → Neutrino Defaults → Default Device** picks the device *type* holding
`<root>:/neutrino/neutrino.elf` — **Auto** / Memory Card / USB / MX4SIO / MMCE / HDD (exFAT) /
HDD (APA) / **Game's Device** / iLink. A miss on the device you picked is not a dead end: OPL falls
through to the AUTO tiers above. The exception is **Game's Device**, which only ever looks on
the game's own device and reports "not found" instead of falling back.

You can also point OPL at a **custom location** via the **Neutrino ELF Path** (`neutrino_path` key
in `settings_riptopl.cfg`). When that field is set and the file exists it takes **priority** over the
auto-detected tiers above (the game's own device and `mc0:`/`mc1:`) — only the **Neutrino
Device** picker outranks it; leave it blank to use the auto-detection. The custom path is
honoured only when its install passes the same `config/system.toml` completeness check (a path
with no directory component is taken as-is). For a path longer than the on-screen 31-character
editor, set `neutrino_path` in `settings_riptopl.cfg` directly.

> **Network boot is the exception:** the UDPFS feature (§4) ships its **own bundled
> Neutrino** (a ready-to-use `neutrino/` folder inside the release's installable package, pre-populated with the
> UDPFS config). The per-game Neutrino use described in this section still needs you to supply
> `neutrino.elf` at the paths above.

## 2. Pick the core per game

1. Highlight a game and open **Game Settings → Compatibility Settings**.
2. Set **Loader Core** to one of:
   - **`<OPL>`** — OPL's built-in core (default).
   - **`Neutrino`** — chain to the external `neutrino.elf`.
3. Save. The choice is stored per title (`$CoreLoader` in the game's `.cfg`).

### Where Neutrino works

| Source | Neutrino? | VMC under Neutrino? |
|---|---|---|
| USB / MX4SIO / internal ATA (BDM) | ✅ | ✅ `-mcN=massN:…VMC/<name>.bin` |
| iLink / IEEE 1394 (**FAT PS2 models only**) | ⚠️ supported and wired; the post-2692 build adds quick boot for RiptOPL's inherited-stack handoff, pending hardware retest | ⚠️ `-mcN=massN:…VMC/<name>.bin` is emitted; the overall iLink launch is pending retest |
| Internal HDD (APA → HDL) | ✅ | ❌ **games boot, VMC is dropped with a warning** — Neutrino has no APA/pfs backing store to open the `.bin` from (NHDDL's HDL backend has the same no-VMC rule). The OPL core honors the same VMC normally. |
| MMCE | ✅ | ✅ `-mcN=mmceN:/…VMC/<name>.bin` |
| UDPFS (network boot — Files or Image) | ✅ **required** — no OPL core, Neutrino only (see §4) | ✅ `-mcN=udpfs:/VMC/<name>.bin` — the PC server must **not** run read-only, or saves fail |
| SMB / ETH | ❌ (always launches with `<OPL>`; the core selector is locked and a stale Neutrino selection warns at launch) | n/a (OPL-core VMC works normally) |
| USB Extreme split images (`.ul`) | ❌ (falls back to `<OPL>`) | n/a |
| Compressed ISO (`.zso`) | ❌ (falls back to `<OPL>`) | n/a |

> **Upstream rates iLink lowest of every backend.** Neutrino's own backend table lists iLink /
> IEEE1394 at a device-compatibility score of **10**, against 80 for USB, 60 for MX4SIO and 100 for
> MMCE, ATA, UDPBD and UDPFS -- and restricts it to **FAT PS2 models**. So a passing `-qb` retest is
> necessary but may not be sufficient: some iLink enclosures are expected to fail upstream too. Use
> the isolation checks below rather than assuming any single fix makes iLink universally work.

Unsupported cases fall back to the `<OPL>` core automatically with an on-screen warning.

> **PS1 games are a separate path.** PlayStation 1 titles (`*.VCD`, shown according to the shared
> **PS2/PS1 Game Display** setting) always boot through **POPSTARTER.ELF** — never OPL's core and never Neutrino
> — so the per-game Loader Core selector is locked/inert for them. See **[VCD.md](VCD.md)**.

## 3. Launch arguments

OPL always builds the **mandatory** Neutrino arguments for you from the game and its
settings:

| Auto argument | When |
|---|---|
| `-bsd=<usb\|ilink\|mx4sio\|ata\|mmce\|udpfs\|udpfsbd\|udpbd>` | always (the storage backend). APA HDD maps to `ata` (+ `-bsdfs=hdl`), so no separate `apa` token is ever emitted. OPL emits this unconditionally for every backend, including `udpfs` — nightly Neutrino builds that auto-detect the backend simply ignore the explicit token, so there is no version split on OPL's side |
| `-bsdfs=hdl` | internal HDD (APA) only |
| `-bsdfs=<exfat\|hdl\|bd>` | only if the per-game **Neutrino Filesystem** picker (Compatibility screen) is set off Auto; block-backed devices only (never mmce/udpfs — no filesystem layer there). `hdl`/`bd` also reshape `-dvd` to `hdl:`/`bdfs:`; a hand-typed `-bsdfs=`/`-dvd=` in the args always wins |
| `-dvd=<path>` / `-dvd=hdl:<partition>` | always (the game image/partition) |
| `-qb` | USB and iLink. RiptOPL's no-reset ELF bridge deliberately preserves the already-mounted BDM environment; quick boot makes Neutrino enter its load environment without resetting that inherited stack away. A user-supplied active `-qb` is never duplicated |
| `-gc=<modes>` | only if the game has OPL compatibility modes set |
| `-dbc` | only if **Debug Colors** is enabled |
| `-logo` | only if **PS2 Logo** is enabled |
| `-gsm=<mode>[:<comp>]` | only if a **Neutrino Video** mode resolves for the game (per-game picker, or the global **Settings → Neutrino Video** default when the per-game picker is "Default"). The `:<comp>` half is appended only when a **Neutrino GSM Compatibility** type is set — it is never emitted on its own |

> **Revision 2692 iLink result:** Neutrino failed on an SCPH-39001 from iLink in all four SDK
> flavours. That build emitted the documented `-bsd=ilink` token but, unlike its USB handoff, did not
> emit `-qb`. Because RiptOPL's bridge intentionally preserves the mounted iLink environment and
> Neutrino documents `-qb` as direct entry into its load environment, that omission is a
> source-confirmed handoff mismatch consistent with the observed reset-boundary symptom. The
> post-2692 change corrects the mismatch; only a new hardware pass can establish whether it was the
> complete cause, so this remains **pending retest** rather than being documented as proven.

On top of those, you can pass **extra Neutrino flags** (e.g. media-type or video tweaks)
in two places. Both are **appended after** the auto-built arguments; **global first, then
per-game**, so a game can extend the global set.

### Global default args — applies to every Neutrino launch

**Game Launching → Neutrino Defaults → Advanced Arguments** (config key `neutrino_args` in
`settings_riptopl.cfg`).

### Per-game args — applies to one title

**Game Settings → Compatibility Settings → Neutrino Launch Args** (config key
`$NeutrinoArgs` in the game's `.cfg`).

Both rows are **buttons** that open the same structured sub-screen rather than one raw text
field: **Quick Boot** (`-qb`), **Debug Colors** (`-dbc`), **PS2 Logo** (`-logo`), **Working Dir**
(`-cwd`), **Config** (`-cfg`), **Boot ELF** (`-elf`), `-ata0`, `-ata0id`, `-ata1`, plus a
free-text **Extra** field for everything else. OPL reassembles the fields in the order Neutrino
accepts, with the Extra / `--b` tail last.

Quick Boot means **enter Neutrino's load environment directly**; it is not merely a boot-screen
toggle. RiptOPL supplies it automatically for USB and iLink handoffs, so enabling the field yourself
on those devices changes nothing and does not add a duplicate.

The stored format is unchanged — a space-separated string, e.g.:

```
-mt=dvd -gsm=1
```

> **Editor length:** each on-screen field still edits at most 31 characters (the same limit as
> Alt-Startup / Game-ID), but fields you don't touch keep their full stored value, so editing one
> field no longer truncates the others. Edit `neutrino_args` / `$NeutrinoArgs` directly in the
> config file only when a single field needs more than 31 characters — OPL reads and forwards the
> full string at launch.

### Automatic `-elf=` (enabled by default; config key only)

RiptOPL defaults `neutrino_elf_arg` to `1` (there is deliberately no settings-screen row for
this), making every Neutrino launch also pass
`-elf=cdrom0:\<STARTUP>;1` -- the game's boot-file path -- so Neutrino's per-GameID
`config/<GameID>.toml` compatibility lookup can resolve before its IOP reset. It is only
emitted for retail-shaped startups (`AAAA_NNN.NN`) and never when your own args already carry
an `-elf=`. Set `neutrino_elf_arg = 0` by hand to disable it if a launch misbehaves.

For the full list of flags Neutrino accepts, see the
[Neutrino documentation](https://github.com/rickgaiser/neutrino).

## 4. Network boot — the Network Protocol selector

RiptOPL streams games from a PC over the LAN, chosen with the **Game Sources → Network Start
Mode** row (**Off** / Manual / Auto) plus the **Network → Protocol** selector — **SMB / UDPFS /
UDPBD**. **UDPFS** is the modern
network-boot protocol (Rick Gaiser's **UDPRDMA** transport); **UDPBD** is the older SUDPBDv2 protocol,
kept for users still running the `udpbd-server`. Both appear in OPL as their own games list — with
covers and per-game settings — and boot via the external Neutrino core. **SMB** is the exception: it's
a mounted SMB file share served by OPL's **own** core (no Neutrino), and it keeps its own address /
port / share / credentials fields plus an **SMB Version** row (**SMBv1** default, or **SMB2**).
Changing the version only takes effect on the next boot — OPL shows the usual restart notice when a
network stack is already up — and it warns first that SMB2 changes **both** browsing and in-game
reading, so an SMBv1-only server (including the bundled PS2-Servers tool) will list no games.

| Choice | Wire protocol | On the PS2 | Core | PC server |
|---|---|---|---|---|
| **Off** | — | no network device (default) | — | — |
| **SMB** | SMBv1 (default) or SMB2 — **Network → SMB Version** | a mounted file share | OPL's *own* core (not Neutrino) | [PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers) (recommended) / Samba |
| **UDPFS** | UDPRDMA | a games source served over UDP (see **UDPFS Access** below) | Neutrino only | [PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers) (recommended, PC) / [udpfs-server](https://github.com/YouKnow-sys/udpfs-server) (Android) / [`udpfsd`](https://github.com/pcm720/udpfsd) |
| **UDPBD** | SUDPBDv2 | a served disk image mounted as `massN:` | Neutrino only | [PS2 Servers](https://github.com/NathanNeurotic/PS2-Servers) (recommended) / [`udpbd-server`](https://github.com/israpps/udpbd-server) |

> **SMB3 is reserved, not offered.** The dialect enum has a slot for it, but packet signing isn't
> implemented, so the picker deliberately stops at SMB2. The bundled PS2-Servers SMB server speaks
> SMBv1 — leave the picker on **SMBv1** unless your server does SMB2.

Because the PS2 has a **single** network adapter, only ONE of these is active per session — the
selector is exclusive *by construction* (the old separate ETH start-mode + "Network Boot" toggle +
"Net Boot Protocol" picker, and their live interlock, are all gone). Local devices (USB / internal
HDD / MMCE) are independent and browse alongside whichever network protocol you pick.

> **UDPBD vs UDPFS — pick by your PC server.** They are **wire-incompatible**: UDPBD speaks SUDPBDv2
> (port `0xBDBD`, `udpbd-server`), UDPFS speaks UDPRDMA (port `0xF5F6`, `udpfsd`). If you already run
> the old `udpbd-server`, select **UDPBD** and nothing else changes. If you're setting up fresh, prefer
> **UDPFS** (it also serves loose ISOs and transparent `.zso`/`.chd` — see below). A config saved as
> UDPBD stays UDPBD: RiptOPL never silently migrates you off it.

### UDPFS Access — Files vs Image

When **UDPFS** is selected, a second sub-setting **"UDPFS Access"** appears with two values — **Files**
(default) and **IMG** (image mode). They are the **same UDPFS protocol** in two shapes: Files is the `udpfs_ioman`
FILESYSTEM (`udpfs:`), Image is the `udpfs_bd` BLOCK device (`massN:`).

| | **Files** (default) | **IMG** |
|---|---|---|
| IOP driver | `udpfs_ioman.irx` → `udpfs:` (filesystem) | `udpfs_bd.irx` → `massN:` (block device) |
| PC serves | a folder containing **`CD/` and `DVD/` subfolders** of ISOs (the standard OPL layout — OPL lists from `<served>/CD` + `<served>/DVD`, NOT from the folder root) | a **FAT/exFAT disk image** |
| Server command | `udpfsd -fsroot <dir>` | `udpfsd -bdpath <image>` |
| Launch | by name (`-dvd=udpfs:<name>`, stock `-bsd=udpfs`) — no `massN:`, no fragment list | mounted like a USB drive (`massN:`, fragment-list launch) |
| Add a game | drop it into the served `CD/` or `DVD/` folder | mount the image, copy, unmount |
| Compression | transparent `.zso`/`.cso`/`.chd` | — (raw sectors only) |

Because both backends bind the same UDPRDMA port and the IOP ministack can load only one per boot, you
**can't run both at once** — Files vs Image is a **config-time choice** (defaulting to Files). It's a
single-adapter, single-transport decision, so **switching Files ↔ Image needs an OPL restart to take
effect** (OPL shows the usual restart-to-apply notice).

- **Files** launches with the **stock** `-bsd=udpfs`, which loads Neutrino's shipped `bsd-udpfs.toml`
  (the FHI filesystem driver, `udpfs_ioman` / `udpfs_fhi`). Neutrino opens the game by name
  (`-dvd=udpfs:<name>`) — no `massN:` block device, no fragment list. On the nightly Neutrino the
  `-bsd` is auto-detected.
- **Image** launches with **`-bsd=udpfsbd`**, a **RiptOPL-private** token: Neutrino ships `udpfs_bd.irx`
  but no stock `-bsd` for it, so RiptOPL auto-places `config/bsd-udpfsbd.toml` into the bundled Neutrino
  folder. The private name is deliberate — it avoids colliding with stock's `bsd-udpfs.toml` (a
  *different* driver), so the block config and the stock filesystem config coexist on one install.

### Requirements

- A **PC-side UDPFS server** on the same LAN, matching the UDPFS Access mode you chose. RiptOPL does
  **not** embed one. Serving from a **phone or tablet** instead of a PC?
  **[udpfs-server](https://github.com/YouKnow-sys/udpfs-server)** by
  **[YouKnow-sys](https://github.com/YouKnow-sys)** is an Android app that shares folders and disk
  images over UDPFS and is found by broadcast, so there is no address to type on the console; it is
  built on **[udpfsd](https://github.com/pcm720/udpfsd)** by **[pcm720](https://github.com/pcm720)**
  and is MIT licensed. On a PC, use **[NathanNeurotic/PS2-Servers](https://github.com/NathanNeurotic/PS2-Servers)**,
  the maintained all-in-one launcher for UDPFS, SMBv1 and UDPBD; release packages include a
  `PS2-Servers.url` shortcut to it. Advanced users can instead run
  **[pcm720/udpfsd](https://github.com/pcm720/udpfsd)** directly. The old standalone `udpbd-server`
  (SUDPBDv2) does **not** work with UDPFS. Layout: **Image** mode's served FAT/exFAT image uses the
  usual OPL folders (`CD`, `DVD`,
  `ART`, `CFG`, …); **Files** mode's served *directory* needs the same `CD/` + `DVD/` subfolders —
  OPL never lists ISOs sitting loose at the served root.
- A **static** PS2 IP. UDPFS has no DHCP client — it reuses the address from **Settings → Network
  Config**, so set a static IP there. OPL warns if DHCP is on when you select UDPFS, and repeats the
  warning as a boot notice while a UDPFS protocol is active with DHCP still on. SMB's server / port /
  share / credentials fields hide automatically when UDPFS is selected.
- You can start the server **after** the console: the UDPFS drivers keep re-discovering in the
  background (both Files and Image), so the games page appears when the server comes up — no reboot.
- UDPFS is Neutrino-only (no `<OPL>` core fallback); if `neutrino.elf` is missing, OPL warns and
  returns to the menu.

> **At launch, Neutrino reads its own IP from a toml, not from OPL.** When a game boots, control hands
> to the external Neutrino, which reads the toml for the active mode — **Files** uses the stock
> `config/bsd-udpfs.toml`; **Image** uses the bundled `config/bsd-udpfsbd.toml`. Both ship hardcoding
> `ip=192.168.1.10`, which historically meant games *listed* fine in OPL but failed to *boot* on any
> other subnet. **RiptOPL now rewrites the active toml's `ip=` to your configured PS2 IP at every UDPFS
> launch**, so no hand-edit is needed; if the toml has been restructured so the `"ip=` token isn't
> found, OPL logs it and leaves the file alone (the old hand-edit contract applies).

> **Hardware note:** the UDPRDMA path (both Files and Image) is validated on emulator only so far —
> real-PS2 confirmation is pending.

## 5. Core-aware per-game settings

The per-game settings adapt to the **Loader Core** chosen for that title, so you only see
options the selected core actually honors (Neutrino ignores most of OPL's embedded-core
features — see the mapping below).

When a game's core is **Neutrino**:
- **Compatibility screen:** the **Neutrino Launch Args** field is editable, and a **Neutrino
  Video** picker (Off / 240p / 480p / 1080i x1–x3 / Default) appears beside it — it maps to
  Neutrino's `-gsm` (`fp1` / `fp2` / `1080ix1..3`; Off emits nothing) and is the Neutrino-side
  stand-in for the hidden OPL GSM panel. **Default** follows the *global* Neutrino Video setting
  in **Settings** (so you can force e.g. `-gsm=1080ix3` for every Neutrino game at once); an
  explicit per-game value — including **Off** — overrides the global. The picker is the *default*
  source of `-gsm`: a manual `-gsm` typed into **Launch Args** takes precedence (OPL emits only
  one `-gsm`, since Neutrino aborts on a duplicate/malformed value).

  > **The "1080p impression" trick:** on 1080-class displays, `1080i x3` (`-gsm=1080ix3`) is the
  > community workaround for progressive-looking output — the same effect people previously got by
  > launching Neutrino from PS2BBLE/OSDmenu with `-gsm=1080ix3`. Set it per game, or globally via
  > **Settings → Neutrino Video** and leave games on "Default". Neutrino itself exposes only the
  > listed `1080i` modes; it does not expose RiptOPL's separate, GSM-synthetic forced-progressive
  > 1080p mode used by the native OPL core.
  OPL compat **mode 4 (Skip Videos)** and **mode 6 (Disable IGR)** are greyed —
  they're OPL ee-core features with **no Neutrino equivalent** (Neutrino has no in-game reset, and
  no PSS/BIK video-skip), so OPL never forwards them. **DL Defaults** is greyed too (it pulls
  OPL-bitmask data that doesn't map to `-gc`). Modes 1/2/3/5 *do* map to `-gc`, and a Neutrino-only
  **mode 7** (greyed under the OPL core — the inverse of 4/6) maps to `-gc=7` (fix games that overrun
  an IOP buffer).
- **Neutrino GSM Compatibility** (Compatibility screen, beside Neutrino Video) — **Off / Type 1
  (GSM/OPL) / Type 2 / Type 3 / Default** — supplies the `:<comp>` half of `-gsm=<mode>:<comp>`,
  a field-flipping fix for shake/tear. The comp half is **never** emitted without a video mode (a
  bare `:<comp>` aborts Neutrino's boot), so the row greys out while the effective video mode is
  Off; **Default** follows the global **Settings** value.
- **GSM, Cheats, PADEMU, OSD Language** panels are OPL-core-only; opening one shows
  *"not used with the Neutrino core"* instead of editing dead options (use the Neutrino Video
  picker above for video forcing).
- **VMC** and **Compatibility** stay available — both are honored under Neutrino. VMC becomes
  discrete `-mc0`/`-mc1` args on **BDM devices (USB/iLink/MX4SIO/exFAT HDD/UDPBD), MMCE and
  UDPFS**; the one exception is **APA HDD**, where no `-mc` args can be emitted (Neutrino has no
  APA/pfs backing store — the game boots and OPL warns that the VMC was dropped). See the VMC
  notes below.
- **UDPFS games** (both Files and Image) have no OPL core backend, so the **Loader Core selector is
  locked to Neutrino** for them (they always launch via Neutrino regardless).

When a game's core is **`<OPL>`** the screen is unchanged from classic OPL, except the Neutrino
Args field and Neutrino Video picker are greyed (never read on the OPL path).

> What Neutrino honors per game: the storage backend + image (`-bsd`/`-dvd`, automatic),
> compat subset (`-gc`), VMC (`-mc0`/`-mc1`), `-logo`, and the free-text **Neutrino Launch
> Args** (the catch-all for everything else, with `$`-disable). Cheats, GSM hacks, IGR/IGS,
> PADEMU and OSD-language are OPL-embedded-core features with no Neutrino equivalent.

### VMC under Neutrino — how it actually works

OPL turns the per-game **VMC** settings (`$VMC_0`/`$VMC_1`) into discrete
`-mcN=<device>:…VMC/<name>.bin` arguments — the same `.bin`, in the same `VMC/` folder, that the
OPL core's mcemu uses, so one card serves both cores.

Each slot also has a **Disable VMC N (keep card)** toggle on the Compatibility screen (stored as
`$VMCDisable_0` / `$VMCDisable_1`). It suppresses that slot's `-mc` argument for **Neutrino
launches only** — the card name stays configured and the game sees the real physical card; OPL's
own mcemu ignores the toggle entirely. Dropping the argument also clears that slot from the VMC
slot mask, so the MMCE GameID card-switch re-arms for it, and on APA HDD it silences the
"VMC unsupported under Neutrino" warning.

Rules that follow from Neutrino's design (verified against `rickgaiser/neutrino` source):

- **The `.bin` must already exist.** Neutrino opens it `O_RDWR` with no create, and **aborts the
  whole boot** (black screen) if it can't. OPL therefore creates/format cards at *config* time
  (the per-game VMC menu, via genvmc — including over UDPFS), and at *launch* verifies each card
  and skips a missing one with a warning rather than handing Neutrino an unopenable path.
- **The VMC must live on the same device as the game.** Neutrino loads exactly one backing-store
  driver per launch and every virtual file (ISO + VMCs) shares it — a VMC on a different device
  than the ISO is structurally unsupported. This is also why APA HDD can't have one: the game
  comes from raw APA (`-bsd=ata -bsdfs=hdl`), and there is no pfs backend for the `.bin`.
- **UDPFS VMCs are writable network files**: the card lives at `VMC/<name>.bin` under the PC
  server's shared folder, and the server must not run in read-only mode or saves fail.
- **Block devices share Neutrino's 64-fragment budget** across ISO + VMCs; OPL pre-counts and
  falls back/aborts with a message when it can't fit (defragment the drive if you see it).
- **Upstream caveats** (Neutrino's `mc_emu`, as of mid-2026): only **port 1 (`-mc0`) is actually
  emulated** — `-mc1` is accepted and opened but the second port's emulation is left inactive —
  and the advertised card geometry is hardcoded (8192 pages ≈ first 4 MB of the image addressed)
  rather than read from the card file. Both are Neutrino-side behaviors, not OPL's; prefer slot 1
  and 8 MB cards for predictable results.
