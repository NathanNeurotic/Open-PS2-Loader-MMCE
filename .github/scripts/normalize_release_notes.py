import pathlib, re, sys

p = pathlib.Path(sys.argv[1])
body = p.read_text(encoding='utf-8')
banner = '''<p align="center">\n  <img width="400" height="92" alt="AI-Assisted-Software-Lovers-Only" src="https://github.com/user-attachments/assets/71335775-9fe3-4507-ac2c-caa851abb24c" />\n</p>'''
# The project banner, served from rebuild/main so one file in the repo drives the README, the
# docs site and every release page at once. It has to be an ABSOLUTE url: a release description
# is rendered outside any repository context, so a relative path resolves to nothing.
hero = '''<p align="center"><img alt="RiptOPL" src="https://raw.githubusercontent.com/NathanNeurotic/Open-PS2-Loader/rebuild/main/docs/assets/riptopl.png" /></p>'''
hero_re = re.compile(r'<p align="center">(?:(?!</p>).)*riptopl\.png(?:(?!</p>).)*</p>\s*', re.DOTALL)
resources = '''## External Tools & Services

- **PS2-Servers** — all-in-one PC server launcher for **SMBv1, UDPFS and UDPBD**: https://github.com/NathanNeurotic/PS2-Servers
- **udpfs-server** — Android UDPFS server for sharing folders and disk images from a phone: https://github.com/YouKnow-sys/udpfs-server
- **OrbitPS2 Manager** — cross-platform PC library manager for importing discs, artwork/screenshots, ZSO compression, per-game settings and VMC management: https://github.com/Luden02/OrbitPS2-Manager
- **OPL PS1 AIO Converter GUI** — Windows all-in-one PS1/POPStarter preparation tool for converting BIN/CUE backups to VCDs and installing them to USB, MX4SIO, MMCE, iLink, exFAT HDD, SMB and APA internal HDD: https://github.com/shaanhomebrew-cloud/OPL-PS1-AIO-Converter-GUI
- **PS2RD CHT Manager** — Windows application for creating, editing and managing `.cht` cheat files for **Open PS2 Loader (OPL) / PS2RD**: https://github.com/TheRealNextria/PS2RD-CHT-Manager
'''

# '## Release downloads' is in this list for the same reason the hero is stripped above: a second
# normalization pass reads back the body the first one wrote. Without it that section was re-appended
# every pass, so a release normalized twice listed its downloads twice.
for marker in ('## Other downloads', '## Release downloads', '## External Tools & Services'):
    if marker in body:
        body = body.split(marker, 1)[0].rstrip()
body = re.sub(r'\nSHA256 \(also published as SHA256SUMS\.txt\):\n```.*?```\n', '\n', body, flags=re.DOTALL)
# Drop any hero this script added on an earlier pass before adding it back. Normalization is not
# once-per-release -- it fires on `release: published` AND on every Rolling Release completion, and
# a re-run reads back the body it wrote last time. Without this, each pass would stack another copy
# of the banner on top of the notes.
body = hero_re.sub('', body)
body = body.lstrip()

downloads = '''## Release downloads

- **Unified package:** the normal installable RiptOPL package containing the standard loaders, PS1/POPSTARTER files, bundled Neutrino and the five companion-tool shortcuts.
- **Variants:** alternate build configurations, including the DualSense (DS5) loaders.
- **Debug:** diagnostic builds for troubleshooting, when produced.
- **Languages:** additional UI language files and fonts, when produced.
- **Source:** the exact source snapshot used to produce the release.

No bare ELF files or SDK/IRX manifests are published as release assets; they are kept inside the appropriate archives where needed.'''

# Hero FIRST, AI-disclosure banner LAST -- the two images do different jobs. The hero is the
# project's identity and belongs where a reader lands. The disclosure badge is a footnote about how
# the software is built, so it stays at the foot where it does not push the notes down the page.
print(f'{hero}\n\n{body}\n\n{downloads}\n\n{resources}\n\n{banner}', end='')
