# OPL Theme Engine — Authoring Reference

This document describes the Open PS2 Loader theme format **as implemented by the parser**
(`src/themes.c`), so you can build your own themes. It covers every block type, property,
value token, and built-in asset name the engine understands, including this fork's
**`Coverflow`** render mode.

- A theme is a folder in the `THM` directory containing a **`conf_theme.cfg`** plus its
  image/font assets.
- Anything not supplied by the theme falls back to OPL's embedded defaults.
- OPL also ships two built-in themes you can study as references: the default `<OPL>` theme
  and this fork's `<Coverflow>` theme.

> Quick mental model: a theme is a flat list of **elements**, each positioned on a virtual
> **640×480** canvas. OPL scales that canvas to the real screen, and (with `scaled=1`)
> corrects for widescreen and the PS2's non-square pixels automatically.

---

## 1. File structure

`conf_theme.cfg` is a plain key/value file. Two kinds of lines exist:

- **Global (top-level) properties** — `key=value` at the start of the file.
- **Element blocks** — a `blockname:` line followed by indented `key=value` lines.

```ini
# lines starting with '#' are comments
bg_color=#182580          ; a global property

main0:                    ; an element block
	type=Background
	pattern=BG
```

Internally, a block property `foo` under block `main0` is read as the key `main0_foo`. You'll
mostly just write the indented form above.

### Block families

Element names carry a **family prefix** + an index (`<family>Main<N>` / `<family>Info<N>`). Each
family targets one screen/view and is a **per-slot override** of the base `mainN`/`infoN`: for every
slot the engine looks for the family's block and, if it's absent, falls back down a chain — so you
override only what differs and inherit the rest.

| Family (main / info) | Drawn on | Per-slot fallback chain |
|---|---|---|
| `main0…` / `info0…` | The **games** list / info — the base layout | *(none — this is the base)* |
| `appsMain0…` / `appsInfo0…` | The **apps** device list / info | → `mainN` / `infoN` |
| `favsMain0…` / `favsInfo0…` | The **Favourites** tab list / info | → `mainN` / `infoN` |
| `vcdMain0…` / `vcdInfo0…` | A device's **PS1/VCD view** (toggle with **L3**) list / info | main: → `appsMainN` → `mainN`  ·  info: → `infoN` |

**The VCD family** (new in this fork) lets PS1/VCD games have their own look. When a device is
switched to its PS1 `*.VCD` list (press **L3**), its covers render from `vcdMain*` and the info page
from `vcdInfo*`. Because each `vcdMain` slot falls back to `appsMain` — a **square** box, matching
PS1 jewel-case art — before `main`, a theme that defines *no* `vcdMain` blocks still shows PS1 games
in the square apps box. So you only add `vcdMain` blocks to make PS1 covers *differ* from apps;
`vcdInfo` falls back to the game `info` layout, preserving the rich PS1 metadata page.

> The games / apps / favs families share one cover-art cache (deduplicated by `pattern`). The **PS1/VCD
> view keeps its OWN cover cache**: an **L3** view reuses the device's game *list* (same item indices as
> the ISO list), so a shared cache would thrash covers on every toggle. The VCD family automatically
> claims its own (4th) `ItemsList` slot and a separate cover cache — one small extra cache; the rest is
> shared. (A theme needs no extra `ItemsList` block for this; it is auto-claimed via the fallback.)

### ⚠ Contiguous numbering rule

The parser reads blocks **in order starting from 0 and stops at the first gap.** If you define
`main0`, `main1`, `main3` (skipping `main2`), then `main3` is **never read**. When you remove
a block, **renumber** the ones after it so there are no holes.

---

## 2. Global properties

Put these at the top of the file (not inside a block).

### Colors (`#RRGGBB`)

| Key | Applies to |
|---|---|
| `bg_color` | Plasma/background tint (the gradient's HIGH end) |
| `plasma_blend_color` | Plasma gradient's LOW end — historically hardcoded black; omit the key (or use `#000000`) for the classic look |
| `text_color` | Default element text |
| `ui_text_color` | UI text (menus, buttons, hints) |
| `sel_text_color` | Selected-item text |

Colors are 8-bit-per-channel hex; alpha is fixed internally.

### Fonts

| Key | Value | Notes |
|---|---|---|
| `default_font` | font file path | Theme's main font; falls back to the built-in font if missing. |
| `font1` … `font15` | font file path | Extra fonts; each defaults to `default_font` if not set. |
| `default_font_size` | int (px) | Size for the default font. |
| `font1_size` … `font15_size` | int (px) | Per-font sizes. |

Reference a font from an element with `font=N` (0 = default). Use `font1=builtin` to force the
embedded font for a slot.

### Other global keys

| Key | Value | Meaning |
|---|---|---|
| `coverflow_cover_offset` | int (−1024…1024, default 0) | Horizontal nudge for the Coverflow carousel (see §8). |
| `use_default` | 0/1 | Use OPL's embedded default assets for anything the theme omits. |
| `use_real_height` | 0/1 | Lay out against the real screen height instead of 480. |
| `use_settings_bg` | 0/1 | Load the theme's `settings_bg.png` behind the settings menus. |

---

## 3. Common element properties

Every element block starts with `type=<ElementType>` (see §5). Most also accept these:

| Property | Type | Default | Notes |
|---|---|---|---|
| `x` | int or `POS_MID` | element-specific | Negative = measured from the **right** edge. `POS_MID` = horizontal center. |
| `y` | int or `POS_MID` | element-specific | Negative = measured from the **bottom** edge. `POS_MID` = vertical center. |
| `width` | int or `DIM_INF` | element-specific | `DIM_INF` = full screen width. |
| `height` | int or `DIM_INF` | element-specific | `DIM_INF` = full screen height. |
| `aligned` | `0` / `1` / `2` | element-specific | `0` = anchor top-left, `1` = center on (x, y), `2` = right-justified + vertically centered (wOPL/uOPL key; regular OPL renders it centered). |
| `scaled` | `0` / `1` | element-specific | `0` = raw pixels, `1` = ratio-correct (handles widescreen + pixel-aspect). Use `1` for images you want undistorted. |
| `color` | `#RRGGBB` | `text_color` | Text/tint color. |
| `font` | `0`…`15` | `0` | Font index (see §2). |
| `reflection` | `0` / `1` | `0` | Draw a mirrored reflection beneath the element (used by `Coverflow`). |
| `enabled` | `0` / `1` | `1` | Set `0` to skip the element without deleting the block. |
| `devices` | name list | *(none)* | **This fork.** Show the element only on the listed device pages — honored by `MenuIcon`, `ItemsList` and `HintText` (ignored elsewhere). See §5 “Per-device placement”. |

**Value tokens:** `POS_MID`, `DIM_INF` (above), and `#RRGGBB` for colors. `x`/`y` accept
negative numbers for right/bottom-relative placement.

---

## 4. Image & overlay properties

Image-type elements (`StaticImage`, `GameImage`, `Background`, `AttributeImage`, `ItemCover`,
`Coverflow`) add:

| Property | Type | Notes |
|---|---|---|
| `default` | asset name | The image to draw (built-in name from §7, or a file in the theme folder). |
| `pattern` | cache suffix | For dynamic art: `COV` (cover), `SCR`/`SCR2` (screenshots), `ICO`, `DISC`, … |
| `count` | int | Number of cache slots for `pattern` (min 2 for `COV`). |
| `overlay` | asset name | An optional frame drawn over the image (e.g. a case/bezel). |
| `overlay_ulx`,`overlay_uly` | int | Upper-left corner of the inlaid image inside the overlay. |
| `overlay_urx`,`overlay_ury` | int | Upper-right corner. |
| `overlay_llx`,`overlay_lly` | int | Lower-left corner. |
| `overlay_lrx`,`overlay_lry` | int | Lower-right corner. |
| `overlay2` | asset name | An optional **second** overlay painted over `overlay`. Shares the element position/size; takes no corners of its own. |

The overlay corners describe where the *inner* art sits within the overlay frame, in the
element's own coordinate space (i.e. relative to `width`/`height`). They scale with the drawn
element, so the window tracks the art at any size.

`overlay2` stacks a second layer in the draw order **cover → `overlay` → `overlay2`**. Use it to
split a frame into parts that sit over a *centred* cover (e.g. a transparent plastic case in
`overlay` and foliage/decoration in `overlay2`), avoiding the off-centre window shift a single
asymmetric overlay would otherwise need. Both layers are shared across elements that name the same
asset, so a second layer costs no extra VRAM when reused.

---

## 5. Element types

Set with `type=`. Elements marked **item** redraw when you move the selection.

| `type=` | Renders |
|---|---|
| `Background` | Full-screen background (`pattern=BG`, or a `default=` image). Auto-added if you omit it. |
| `StaticImage` | A fixed image from `default=`. |
| `StaticText` | Fixed text from `value=`. |
| `AttributeText` *(item)* | Game metadata text — see `attribute=` in §6. |
| `GameCountText` *(item)* | The number of items in the current list. |
| `GameImage` *(item)* | Dynamic art from `pattern=` (cover/screenshot/…) with `default=` fallback. |
| `AttributeImage` *(item)* | Badge image chosen by a game attribute — see §6. |
| `MenuIcon` | The current device/menu icon. |
| `MenuText` | The current menu name with left/right arrows. |
| `ItemsList` *(item)* | The scrolling list of games/apps. (Auto-added if omitted.) |
| `ItemIcon` *(item)* | Per-row decorator icons (uses the list's `decorator=` pattern). |
| `ItemCover` *(item)* | The selected item's cover, with optional `overlay`. |
| `ItemText` *(item)* | The selected item's startup filename. |
| `HintText` | Button hints for the list screen. |
| `InfoHintText` | Button hints for the info screen. |
| `LoadingIcon` | The animated loading spinner. |
| `BdmIndex` | The block-device mode indicator. |
| `Coverflow` *(item)* | **This fork:** a cover-art carousel (see §8). |

### Text element extras

| Property | Used by | Notes |
|---|---|---|
| `value` | `StaticText` | The literal string to show. |
| `attribute` | `AttributeText`, `AttributeImage` | The metadata key (§6). |
| `display` | text | `0` = always (label + value), `1` = only when the value exists, `2` = value only (no label). |
| `wrap` | text | `1` = word-wrap within `width`/`height`. |
| `title` | `AttributeText` | Override the auto label for the attribute. |

### List extras

| Property | Used by | Notes |
|---|---|---|
| `decorator` | `ItemsList` | A `GameImage` pattern name to draw as per-row icons. |

### Per-device placement (`devices=`) — *this fork*

Normally one `MenuIcon`, one `ItemsList` and one `HintText` serve **every** device page, so they
share a single position. `devices=` lets a theme place them differently per device: an element
with the key draws **only** on the listed pages, and an unfiltered element of the same type
automatically stands down there (no key needed on it), staying the default everywhere else.

```ini
# The shared icon spot, used by every page not claimed below.
main3:
type=MenuIcon
x=POS_MID
y=400

# On the MMCE and APA-HDD pages, pin the icon top-right instead.
main4:
type=MenuIcon
devices=mmce,hdd
x=-40
y=24
```

**Device names** (comma-separated, case-insensitive):

| Name | Page |
|---|---|
| `usb` | USB mass storage (any slot) |
| `ilink` | iLink / FireWire |
| `mx4sio` | MX4SIO |
| `hdd_bd` | Internal HDD, exFAT (BDM) |
| `hdd` | Internal HDD, APA/PFS |
| `eth` | Network (SMB) |
| `smb` | Alias of `eth` |
| `mmce` | MMCE / memory-card emulator |
| `udpbd` | UDPBD network boot |
| `udpfs` | UDPFS network boot |
| `app` | The Apps tab |
| `fav` | The Favourites tab |
| `bdm` | The generic BDM page before its driver is identified (auto/manual start) |

Rules worth knowing:

- **Only `MenuIcon`, `ItemsList` and `HintText`** honor the key; on other element types it parses
  but has no effect.
- Matching follows the page's **visible device icon**, so a BDM page counts as `usb`/`mx4sio`/…
  once its driver is identified, and as `bdm` before that. In the PS1/VCD (L3) view the page keeps
  the device's identity — filters keep working there via the `vcdMain*` family.
- A filtered `ItemsList` is an **override**: it does not consume one of the theme's ItemsList
  slots (games/apps/favs/VCD, claimed in file order — see §1), and navigation/paging automatically
  follows whichever list is drawn. Give it its own `x`/`y`/`height` (its row count comes from
  `height`); `decorator=` works as usual.
- **Keep one unfiltered `ItemsList`.** If *every* list in the theme carries `devices=`, OPL adds a
  plain default list (373×316 at 42,42) on the pages nothing covers — navigation always needs a
  list — and it won't match your layout.
- Unknown device names are logged and skipped; if nothing valid remains, the element behaves as if
  the key were absent (shared placement) rather than disappearing.
- Two filtered elements of the same type may list different devices; listing the **same** device
  twice draws both — that's an authoring error, not a picker.
- Themes without the key are untouched — the filter only activates when `devices=` appears.

### Using RiptOPL themes on regular OPL

RiptOPL themes are designed to load on regular/upstream OPL and degrade gracefully — one theme can
serve both. Regular OPL's parser never aborts on fork content: keys it doesn't know (`devices=`,
`reflection_offset`, `overlay2`, `plasma_blend_color`, the entire `favsMain*` and `vcdMain*`
families…) are simply never read, and unknown element types (`Coverflow`) are skipped without
stopping the parse. What to expect there:

- **Invisible:** all pure-key extras above — regular OPL renders the theme as if they weren't
  written.
- **Cosmetic:** `aligned=2` renders centered; `#Size`/`#DiscType`/`#System` attributes and the
  Coverflow carousel are silently absent.
- **Ugly but harmless:** `devices=` layouts — regular OPL draws *every* `MenuIcon`/`HintText` block
  on *every* page (overlapping), and treats a second `ItemsList` as the apps list while still
  drawing it on the games screen. Per-device layouts are effectively RiptOPL-only.
- **⚠ The one rule:** **always include one plain (unfiltered) `ItemsList`**. A theme with none —
  e.g. a Coverflow-only theme, or one where every list carries `devices=` — *crashes regular OPL on
  the first scroll* (a latent upstream bug in its default-list repair; RiptOPL fixed it on our
  side). Ship the plain list and the theme is safe everywhere.

---

## 6. `attribute=` values

### AttributeText (metadata text)

`Title`, `Developer`, `Description`, `Genre`, `Release`, `#Size` (the `#Size` value renders
with a `MiB` suffix). Labels are auto-localized; override with `title=`.

### AttributeImage (badge chosen by value)

A per-game **attribute** picks which glyph to draw. Where that glyph comes from depends on the theme:

* **Built-in `<OPL>` theme** (no theme folder) — draws the *embedded* glyph whose internal name equals
  the value, e.g. `#System=PS2` → the built-in `PS2`, `#Media=CD` → `CD`, `#Format=ISO` → `ISO`.
  Attributes with no embedded glyph (e.g. `#DiscType`) draw nothing on the built-in theme.
* **Disk theme** — loads **`<value>_<attribute>.png`** from the theme folder — *value first, then the
  attribute* (the same `<name>_<suffix>` shape cover art uses, e.g. `<id>_COV.png`). So `#System=PS2`
  → `PS2_#System.png`, `#Media=CD` → `CD_#Media.png`, `#DiscType=PS2DVD` → `PS2DVD_#DiscType.png`.

> ⚠️ The disk-theme file is **`<value>_<attribute>`**, *not* `<attribute>_<value>`. `PS2DVD_#DiscType.png`
> is correct; `#DiscType_PS2DVD.png` is never read. (The `#` is a literal character in the filename, and
> the extension must be lower-case **`.png`** — the PS2 HDD's filesystem is case-sensitive.)

Metadata attributes OPL sets automatically per game — on **every device** (USB/BDM, SMB, MMCE and the
internal HDD/HDL, including their PS1/VCD lists):

| `attribute=` | Value(s) OPL sets | Built-in glyph | Disk-theme file |
|---|---|---|---|
| `#System` | `PS1`, `PS2` | `PS1`, `PS2` | `PS1_#System.png`, `PS2_#System.png` |
| `#Media` | `CD`, `DVD` | `CD`, `DVD`, `APP` | `CD_#Media.png`, `DVD_#Media.png` |
| `#Format` | `ISO`, `ZSO`, `VCD`, `UL`, `ELF`, `HDL` | same names | `ISO_#Format.png`, `VCD_#Format.png`, … |
| `#DiscType` | `PS1CD`, `PS2CD`, `PS2DVD` | *(none — supply your own)* | `PS1CD_#DiscType.png`, `PS2CD_#DiscType.png`, `PS2DVD_#DiscType.png` |
| `#Size` | a byte count *(use with `AttributeText`, renders as `… MiB`)* | — | — |

Compatibility attributes whose value is *already* a full glyph name — `Vmode`, `Aspect`, `Scan`,
`Players`, `Rating` (e.g. `Vmode_ntsc`, `Aspect_w`, `Scan_480p`, `Rating_3`) — follow the same rules:
the built-in theme draws the matching embedded glyph, a disk theme uses `<value>_<attribute>.png`.

#### `#DiscType` worked example *(issue #49)*

`#System` says PS1 vs PS2 and `#Media` says CD vs DVD, but PS1-CD and PS2-CD both report `#Media=CD`, so
one `#Media` glyph can't tell them apart. `#DiscType` collapses console + media into a single token so a
theme can show **one** disc glyph per kind. There are no built-in `#DiscType` glyphs — a disk theme
supplies its own. In your theme's `conf_theme.cfg`, add an element to the page you want it on. The
`mainN` / `infoN` indices must be **contiguous** (the parser stops at the first gap), so use the next
free number — if your theme already has `main0`…`main8`, add `main9`:

```
main9:
	type=AttributeImage
	attribute=#DiscType
	x=55
	y=-252
	width=32
	height=16
```

Then drop three PNGs **in the theme folder, next to `conf_theme.cfg`**:

* `PS1CD_#DiscType.png`
* `PS2CD_#DiscType.png`
* `PS2DVD_#DiscType.png`

(Position with `x`/`y`/`width`/`height` — negative `x`/`y` anchor from the right/bottom edge. Copy the
coordinates from a neighbouring `AttributeImage` such as the `#Media` badge to line it up.)

---

## 7. Built-in `default=` asset names

You can point `default=`/`overlay=` at any of OPL's embedded textures (no file needed):

- **Covers / art:** `cover`, `coverapp`, `disc`, `screen`, `screens` (overlay), `missing`
- **Case overlays:** `case` (the shared frame, layer 1), `case_overlay` (layer 2, drawn over `case`), `apps_case` (legacy apps-only frame)
- **Device icons:** `usb`, `mmce`, `hdd`, `eth`, `app`, `fav`, `usb_bd`, `ilk_bd`, `m4s_bd`, `hdd_bd`, `udp_bd` (UDPBD), `udp_fs` (UDPFS)
- **BDM indicators:** `Index_0` … `Index_4`
- **Buttons:** `cross`, `circle`, `triangle`, `square`, `left`, `right`, `select`, `start`, `L3`, `R3`, `fav_mark`
- **Loading frames:** `load0` … `load7`
- **Boot logo:** `logo`, `logo0` … `logo6`
- **Backgrounds:** `incebtion` (default theme bg), `ip`
- **Format icons:** `ELF`, `HDL`, `ISO`, `ZSO`, `UL`
- **Media icons:** `APP`, `CD`, `DVD`
- **Aspect / scan / vmode / rating** sets as listed in §6.

(`settings_bg` is theme-supplied; `Device_*` indicators are deprecated/removed in this fork.)

---

## 8. The Coverflow element (this fork)

`type=Coverflow` renders the game/app list as a centered cover carousel with a reflection.
It behaves like a `GameImage` (uses the `COV` cover cache) but draws 3 or 5 covers with a
slide animation.

While the user is idle, the carousel also *prefetches* the covers just outside the visible
window (up to 4 positions each side of the selection, wrapping around the list ends), so a
scroll step reveals a cover that is usually already loaded instead of the placeholder
(issue #296). The prefetch only starts once the selection's own cover has loaded, and it is
skipped entirely on MMCE-backed items (including MMCE-sourced favourites) to keep the shared
SIO2 bus quiet — those covers still load on selection, as before.

### Per-theme properties (in `conf_theme.cfg`)

| Property | Notes |
|---|---|
| `default` | Fallback cover (e.g. `cover`, or `coverapp` for the apps list). |
| `overlay` + `overlay_*` corners | The case art drawn around each cover (e.g. `case` / `apps_case`). The corners place the cover **inside** the case frame; the engine auto-centers the visible frame and keeps it aspect-correct in both 4:3 and widescreen. |
| `reflection` | `1` to draw the mirrored reflection below each cover (alpha-faded). |
| `x`, `y`, `width`, `height` | Position and per-cover size. `width`/`height` should match your case art's pixel size so the overlay corners line up. |

A minimal example (this fork's `<Coverflow>` theme uses values like these):

```ini
main2:
	type=Coverflow
	default=cover
	reflection=1
	y=197
	width=184
	height=256
	overlay=case
	overlay2=case_overlay
	overlay_ulx=0    overlay_uly=0    overlay_urx=184  overlay_ury=0
	overlay_llx=0    overlay_lly=256  overlay_lrx=184  overlay_lry=256
appsMain2:
	type=Coverflow
	default=coverapp
	reflection=1
	y=239
	width=184
	height=184
	overlay=case
	overlay2=case_overlay
	overlay_ulx=0    overlay_uly=0    overlay_urx=184  overlay_ury=0
	overlay_llx=0    overlay_lly=184  overlay_lrx=184  overlay_lry=184
vcdMain2:
	type=Coverflow
	default=cover
	reflection=1
	y=239
	width=184
	height=184
	overlay=case
	overlay2=case_overlay
	overlay_ulx=0    overlay_uly=0    overlay_urx=184  overlay_ury=0
	overlay_llx=0    overlay_lly=184  overlay_lrx=184  overlay_lry=184
```

Apps and PS1/VCD covers share the games frame but use a **square** element so the box matches
their square art, while games stay PS2-case **portrait**. The `vcdMain2` block above gives PS1/VCD
covers their own square element; omit it and PS1 games reuse `appsMain2` (see *Block families*). The element `width`/`height` set the box
aspect; the overlay corners (full-canvas of those dims) let the cover fill it.

### Global Coverflow tuning (NOT in the theme)

These live in **`settings_riptopl.cfg`** and are exposed in the **Coverflow Settings** menu (shown
while a Coverflow theme is active). They apply to *any* Coverflow theme:

| Setting / key | Values | Effect |
|---|---|---|
| `coverflow_count` | 3 or 5 | Number of covers shown. |
| `coverflow_scale` | px | How much the center cover grows. |
| `coverflow_anim` | ms | Slide animation duration (0 = instant). |
| `coverflow_dim` | 0/1 | Dim the non-center covers. |

(The per-theme horizontal nudge `coverflow_cover_offset` is a global theme key — see §2.)

---

## 9. A note on coordinates & scaling

- All positions/sizes are authored on a **640×480** canvas; OPL scales to the active video
  mode.
- With `scaled=1`, image **widths** are corrected for widescreen and the PS2's pixel aspect,
  so a square stays square in both 4:3 and 16:9. Positions are not aspect-scaled, so layouts
  fill the wider screen naturally.
- Negative `x`/`y` and `POS_MID`/`DIM_INF` let you anchor to edges/center without hardcoding
  resolution-specific numbers.

For a complete, working reference, open the bundled `<OPL>` and `<Coverflow>` themes and the
`misc/conf_theme_OPL.cfg` / `misc/theme_coverflow.cfg` sources in the repository.
