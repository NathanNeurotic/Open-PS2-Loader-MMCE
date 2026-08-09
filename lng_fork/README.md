# Fork translation overlay

Each `<Language>.yml` here is a small **overlay** that translates the UI strings this
fork (RiptOPL) adds on top of upstream OPL — Neutrino, MMCE, BDMA, UDPBD, VCD, Coverflow,
Favourites, and friends. Upstream's shared language repository doesn't carry those labels,
so this overlay is what keeps them from falling back to English.

Format — a single `translations:` map of label → translated string:

```yml
translations:
  VCD_ON: Affichage des jeux VCD
  COVERFLOW_SETTINGS: Paramètres Coverflow
```

At build time (`make languages`), `lang_compiler.py --overlay_translation_yml` merges each
file into its language **only to fill gaps** — it never overwrites a real upstream/human
translation, and a missing file just means English fallback. `<Language>` must match a name
in the Makefile's `TRANSLATIONS` list (e.g. `French`, `Portuguese_BR`, `SChinese`).

The shipped files are machine-generated best-effort translations; corrections are welcome.
Keep acronyms, file paths, flags and `%s`/`%d`/`%i` specifiers verbatim. Full guide:
[`../lng/README.md`](../lng/README.md).
