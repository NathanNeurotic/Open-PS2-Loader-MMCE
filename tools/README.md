# Repository tools

Run these commands from the **repository root**. Moving a script does not change
where it reads source files or writes generated output. Python tools require the
packages in the root `requirements.txt`; shell tools use the existing build
environment (Bash, Git, make, and the usual Unix utilities).

| Directory | Tools | Purpose |
| --- | --- | --- |
| `dependencies/` | `download_lng.sh`, `download_lwNBD.sh`, `download_cfla.sh` | Fetch the existing language, lwNBD, and formatting dependencies |
| `languages/` | `lang_compiler.py`, `lang_decompiler.py`, `lng_pack.sh` | Compile/decompile translations and assemble the language ZIP |
| `release/` | `make_changelog.sh` | Generate the root `DETAILED_CHANGELOG` |

Prefer the unchanged Makefile targets for normal work:

```sh
make download_lng languages
make download_lwNBD
make download_cfla
make DETAILED_CHANGELOG
```

Direct invocations now use these paths:

```sh
python3 tools/languages/lang_compiler.py --help
python3 tools/languages/lang_decompiler.py
./tools/languages/lng_pack.sh
sh tools/release/make_changelog.sh
```

The compiler's arguments and outputs are unchanged. The decompiler still reads
`lng/` and writes translation YAML under `lng_src/`; it is not a read-only command.
The language pack still appears as `RIPTOPL-LANGS-*.zip` at the repository root.
Dependency scripts retain their existing update behavior, including resetting the
generated upstream language checkout to its remote branch.

External scripts that invoked a former root helper path must use its new path
above, or the corresponding Makefile target. Helpers remain executable where they
were executable before. The legacy changelog input is now
`docs/history/OLD_DETAILED_CHANGELOG`.

Host applications remain under `pc/`, standalone experiments under `labs/`, and
GitHub-specific workflow helpers under `.github/scripts/`.
