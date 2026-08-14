# RiptOPL SMBv1 server

> **Legacy source only:** this standalone server is no longer included in RiptOPL release packages.
> For the maintained UDPFS / SMBv1 / UDPBD all-in-one launcher and packaged downloads, use
> **[NathanNeurotic/PS2-Servers](https://github.com/NathanNeurotic/PS2-Servers)**.

A tiny, dependency-free **SMBv1/CIFS server** that Open-PS2-Loader (and forks) can browse and
load games from — so SMB keeps working even on hosts where the OS has killed SMBv1.

## Why this exists

OPL's network game loading speaks **SMBv1** (`NT LM 0.12`) with **LM/NTLMv1** auth. Windows 11
(24H2 / 25H2) ships the SMB1 server **off by default** and **removed NTLMv1**, so the old
"share a folder from Windows" path is effectively dead — and re-enabling it fights Microsoft's
hardening with no guarantee it works.

This program sidesteps all of that: it **implements SMBv1 itself** and accepts **guest** logons,
so it does not touch Windows' SMB stack at all. OPL connects to *this* server on a custom TCP
port — Windows' own SMB2/3 service on port 445 is left completely alone. No Windows SMB1 feature,
no NTLMv1, no registry edits.

It's pure Python 3 standard library (no `pip install`), so it runs as a bare `.py` with zero
antivirus false-positives, and works the same on Windows, Linux and macOS.

## Quickstart

### Windows — easiest, no terminal

Double-click **`Start-SMB-Server.bat`**. To tell it where your games are, either:

- **drag your PS2 games folder onto the `.bat`**, or
- open the `.bat` in Notepad and set `GAMES=` to your games folder, or
- just run it and type the folder path when it asks.

It checks for Python (and points you to the download if it's missing), then starts the
server and shows your PC's IP and port. Leave the window open while you play.

### Any OS — terminal

```sh
python smbserver_opl.py --share games=D:/PS2Games
```

It prints something like:

```
 RiptOPL SMBv1 server -- listening on 0.0.0.0:1111
 In OPL  ->  SMB Server IP: 192.168.1.50   Port: 1111   user/pass: blank (guest)
            Share: games   ->   D:\PS2Games   (writable)
 (writable -- OPL can save settings + VMC-on-SMB here; pass --read-only to lock it)
```

Then in OPL → **Settings → Network**:

| Field            | Value                                  |
|------------------|----------------------------------------|
| Address type     | **IP address** (turn NetBIOS *off*)    |
| PC IP Address    | the LAN IP the server printed          |
| **Port**         | **1111** (the printed port)            |
| Share            | `games`                                |
| User / Password  | **blank** (guest)                      |

Save, and your network games should populate from the share.

> **Upgrading from an older RiptOPL build?** The default port has moved over time
> (upstream `445` → `1000` → `1024` → **`1111`** today), and a *saved* OPL config keeps
> whatever port it was saved with — defaults never retro-apply. If SMB stopped connecting
> right after an update, the fix is one field: set **OPL → Network Config → Port** to match
> what this server prints at startup (or start the server with `--port <your saved value>`).
> Each side looks "correctly configured at its own default" — they just have to be the *same*.

## Options

```
--share NAME=PATH   a share to export (repeatable), e.g. --share apps=E:/PS2Apps
--port N            TCP port (default 1111). If it's taken, the server walks forward
                    and prints the real one — just match OPL's Port field to it.
--bind ADDR         interface to bind (default 0.0.0.0 = all)
--read-only         serve the share read-only (no saves / no VMC writes); default is writable
--take-445          bind the *standard* port 445 instead, by pausing Windows' LanmanServer
                    (admin; reversible — see below)
-v                  verbose protocol logging
```

### Writable by default

The share is **writable** so OPL can do what it normally does over SMB — save per-game settings
and write **VMC-on-SMB** memory cards back to the folder. It's a guest share on your LAN, so only
point it at a folder you're comfortable letting the PS2 write to. Pass **`--read-only`** if you
want a strictly read-only share (browsing + booting games only, no saves).

### `--take-445` (no OPL change needed, but invasive)

If you'd rather not edit OPL's Port field (e.g. an existing setup pinned to 445), `--take-445`
makes the server sit on the standard port 445. Windows still holds 445 via its own
**LanmanServer** service, so this **stops LanmanServer** (which turns off Windows' *own* file
sharing) while the server runs, and **restarts it on exit**. It needs an **Administrator** shell.

It only *stops* the service (never disables it), so even a hard kill self-heals on the next
reboot. Still — the plain `--port 1111` default is simpler and touches nothing; prefer it unless
you specifically need 445.

## Status / testing

`selftest.py` is a raw SMB1 client that mimics OPL's exact wire sequence (share-level + plaintext
guest, the `READ_ANDX` framing, `FIND_FIRST2` listing) and asserts the protocol byte-for-byte:

```sh
python selftest.py     # spins up the server on a scratch port + temp share, runs 28 checks
```

The protocol is validated locally against that harness. **Final validation is on real hardware**
(an actual PS2 running OPL, or PCSX2 with a network adapter) — booting an ISO over the share and
browsing the menu. If you hit an issue there, run the server with `-v` and capture the output.

## Optional: build a single `.exe`

Power users should just run the `.py`. If you want a double-clickable Windows binary, see
`build_exe.ps1` (uses Nuitka, not PyInstaller — PyInstaller's bootloader trips antivirus). Code
signing is the single biggest false-positive reducer.
