# wOPL coverflow reference assets

Reference-only material vendored from the **wOPL** fork (KrahJohlito), branch
`wopl/wOPL-base`, to aid inference of OPL's (undocumented) theme system and future
porting work. **None of this is compiled** — these files live outside `gfx/` and `misc/`
and are not listed in the `Makefile`, so they have zero effect on the build. They are
kept purely for reference.

## Contents

- **`theme_coverflow.cfg`** — wOPL's *original* coverflow theme cfg. Our shipping
  `misc/theme_coverflow.cfg` is derived from it but trimmed to assets we ship. This copy
  preserves the full original (settings_bg/plank/info backgrounds, the `$Enable*` status
  icons, Genre/Release rows, font defs, etc.) as a worked example of the theme format.

- **`gfx/`** — the texture PNGs wOPL's theme references that our build does **not** ship:
  | file | wOPL theme use | wОПL texture enum |
  |---|---|---|
  | `settings_bg.png` | `main0` settings/menu background (the `use_settings_bg` feature) | `SETTINGS_BG` |
  | `plank.png` | `main2` decorative plank behind the covers | `PLANK` |
  | `info.png` | `info1` info-screen background | `INFO_BG` (dead in wOPL) |
  | `pademu_on/off.png` | `$EnablePadEmu` status icon | `PADEMU_ON/OFF` |
  | `cht_on/off.png` | `$EnableCheat` status icon | `CHT_ON/OFF` |
  | `gsm_on/off.png` | `$EnableGSM` status icon | `GSM_ON/OFF` |

## Notes for future use

- To actually *use* any of these, the texture must be added to `include/textures.h`
  (enum), `src/textures.c` (`internalDefault[]` table, with a bin2c'd `_png` symbol), and
  `gfx/` + `PNG_ASSETS` in the `Makefile` (for an embedded default). Inserting enum values
  shifts texture IDs — keep the enum and the positional `internalDefault[]` table in sync,
  and mind the enum-range load loops in `src/themes.c` (`for (i = ELF_FORMAT; i <= VMODE_PAL; …)` etc.).
- `settings_bg`: the **piping** (a `SETTINGS_BG` slot + `guiDrawBGSettings()` hook + the
  `use_settings_bg` theme flag) is wired in our tree; it is dormant until an embedded
  `gfx/settings_bg.png` is added (flip the `internalDefault[]` `NULL` → `&settings_bg_png`,
  add the asset + `PNG_ASSETS` entry) or a disk theme ships its own `settings_bg.png` with
  `use_settings_bg=1`.
- The `$Enable*` status icons also need `src/supportbase.c` to write the config keys back
  as image-name strings (`"pademu_on"`/`"pademu_off"` …) — wOPL does this in `writeConfig`;
  our master only reads them as ints. Without that, the AttributeImage elements won't resolve.
- Genre/Release attribute text already works on our master (no asset needed).
