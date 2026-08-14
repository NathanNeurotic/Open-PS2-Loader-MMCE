# Translation guide

## For translators

All translation files moved to the separate repository: <https://github.com/ps2homebrew/Open-PS2-Loader-lang>\\
Provide your submissions there.\\
Languages were moved there for reducing commit stress for the main repository.\\
Check if your `.yml` file has untranslated strings. Example:

```yml
  MENU:
  - untranslated
  - original: Menu
```

You should change this into:

```yml
  MENU: Menu_on_your_language
```

If you want to test your changes:

- run `make download_lng`
- make your changes int `lng_src/*.yml`
- run `make languages`
- test generated file from `lng/lang_*.lng`.

## For developers

If you add the language string into the code,\\
propose your changes into `lng_tmpl/_base.yml` with the necessary comments.\\
For example, if you add the string `_STR_NETWORK_STARTUP_ERROR`,\\
then you should update the base file like:

```yml
- comment: Generic network error message.
  label: NETWORK_STARTUP_ERROR
  string: '%d: Network startup error.'
```

After running `make languages` propose your changes into [the language repo](https://github.com/ps2homebrew/Open-PS2-Loader-lang).\\
Folder `lng_src` will contain updated files.

It is not recommended to rename or remove already existing strings.\\
In such a case, you will need to edit all language yml files manually.

## Fork-specific strings (the `lng_fork/` overlay)

This fork (RiptOPL) adds many UI strings that don't exist in upstream OPL — the
Neutrino core, MMCE, BDMA, UDPBD, VCD, Coverflow, Favourites, and more. The shared
upstream language repository above doesn't know about those labels, so without help
they would show in **English** in every translated language.

To fill that gap without forking the upstream language files, the fork keeps a small
per-language **translation overlay** under [`lng_fork/`](../lng_fork). Each file is just a
`translations:` map of the fork's labels:

```yml
# lng_fork/French.yml
translations:
  VCD_ON: Affichage des jeux VCD
  COVERFLOW_SETTINGS: Paramètres Coverflow
  NEUTRINO_PATH: Chemin ELF Neutrino
```

During `make languages`, after each upstream `lng_src/<Language>.yml` is merged against
`lng_tmpl/_base.yml`, `lang_compiler.py --overlay_translation_yml` applies the matching
`lng_fork/<Language>.yml`:

- It only **fills gaps.** A label that already carries a real (human) translation in the
  upstream language file is left untouched — the overlay never clobbers upstream work, and
  once upstream translates a label the overlay quietly steps aside.
- It is **idempotent** and **optional** per language: no overlay file simply means English
  fallback for the fork strings, exactly as before.

The bundled overlays are **machine-generated, best-effort** translations — corrections are
very welcome. To fix or add one:

1. Edit `lng_fork/<Language>.yml` (use the English values in `lng_tmpl/_base.yml` as the source).
   Keep technical terms / acronyms / file paths / flags and `%s` `%d` `%i` format specifiers verbatim.
2. Run `make languages`.
3. Check the result in `lng/lang_<Language>.lng`.
