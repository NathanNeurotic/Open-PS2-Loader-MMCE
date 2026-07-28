# Languages / translations — how they work and how to maintain them

RiptOPL's on-screen text is built from three sources that `lang_compiler.py` merges at
build time. This doc explains the pipeline and the exact steps to add or update strings.

## The pieces

| Piece | What it is | Do you edit it? |
|---|---|---|
| `lng_tmpl/_base.yml` | The fork's **master list** of every string + its **English** text. Source of truth. | **Yes** — add strings here. |
| `lng_fork/<Language>.yml` | The fork's **own translations** of fork strings (and overrides). | **Yes** — add translations here. |
| `lng_src/` (git-ignored) | The **upstream** community translations, cloned from [`ps2homebrew/Open-PS2-Loader-lang`](https://github.com/ps2homebrew/Open-PS2-Loader-lang) by `download_lng.sh`. | **No** — upstream owns these. |
| `lang_compiler.py` | The tool that assembles everything into the binary `.lng` files + the C sources. | No (it's the machinery). |

### `_base.yml` entry format
```yaml
- label: MMCE_PREFIX
  string: MMCE Prefix Path
```

### `lng_fork/<Language>.yml` entry format
```yaml
translations:
  MMCE_PREFIX: Chemin du préfixe MMCE
```

## The build flow (`make languages`, all via `lang_compiler.py`)

1. **`--make_header` / `--make_source`** → generate `include/lang_autogen.h` (the `_STR_*`
   enum every `.c` file uses) and `src/lang_internal.c` (the built-in **English fallback**)
   straight from `_base.yml`. So English is always complete and automatic — you never
   translate English, and a missing translation always falls back to English, never blank.
2. **`--update_translation_yml`** → sync each upstream `lng_src/<Lang>.yml` against
   `_base.yml`; any label it lacks is marked `untranslated`.
3. **`--overlay_translation_yml --overlay lng_fork/<Lang>.yml`** → pour the fork's own
   translations on top. If upstream later translates a label, the overlay quietly steps
   aside (upstream wins). Idempotent — safe to re-run.
4. **`--make_lng`** → compile the merged `.yml` into the binary `lng/lang_<Lang>.lng`.

`lang_autogen.h` and `lang_internal.c` are generated (git-ignored) — never hand-edit them.

## How to ADD a new string

1. Add it to the **END** of `lng_tmpl/_base.yml`, below the APPEND-ONLY banner (~line 923):

   ```yaml
   - label: MY_NEW_SETTING
     string: My New Setting
   ```

   External `lang_*.lng` files are consumed by **line position** (`lang.c`), so inserting a
   label mid-list shifts every later string ID and makes stale user language packs show the
   **wrong text** — never an English fallback.
2. Use `_STR_MY_NEW_SETTING` in code (the enum id is auto-generated).
3. That's enough to ship — it shows English everywhere until translated.

## How to TRANSLATE a string (per language)

Add the key under `translations:` in the target `lng_fork/<Language>.yml`:
```yaml
translations:
  MY_NEW_SETTING: Mon nouveau réglage
```
Repeat per language you want to cover. Untranslated languages fall back to English.

## Important: there is **no auto-translator** in the build

`lang_compiler.py` only *merges* translations that already exist in `lng_fork/*.yml`. The
initial bulk fill of the 32 fork languages was a **one-off external step** (a machine
translation pass), not part of this repo's build. So to keep translations current you must
supply the translated text for `lng_fork/*.yml` yourself (by hand, an external MT tool, or
an assistant) — the build will not invent it.

## Building just the languages

```sh
make languages
```
Requires `python3` + `PyYAML` (`py3-yaml`) and network access for `download_lng.sh` (first
run clones the upstream lang repo into `lng_src/`). CI does this automatically.

## Where the fork strings live

Fork-added strings are the labels in `lng_tmpl/_base.yml` that upstream doesn't have. To
see which are still English-only in a given language, run `make languages` and look for
`untranslated` entries in `lng_src/<Language>.yml` after the overlay step.
