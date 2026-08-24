import pathlib, re, sys

p = pathlib.Path(sys.argv[1])
body = p.read_text(encoding='utf-8')
banner = '''<p align="center">\n  <img width="400" height="92" alt="AI-Assisted-Software-Lovers-Only" src="https://github.com/user-attachments/assets/71335775-9fe3-4507-ac2c-caa851abb24c" />\n</p>'''
resources = '''## External Tools & Services\n\n- **PS2-Servers** — all-in-one PC server launcher for **SMBv1, UDPFS and UDPBD**: https://github.com/NathanNeurotic/PS2-Servers\n- **OrbitOPL Toolbox** — cross-platform PC library manager for importing discs, artwork/screenshots, ZSO compression, per-game settings and VMC management: https://github.com/Luden02/OrbitOPL-Toolbox\n- **OPL PS1 AIO Converter GUI** — Windows all-in-one PS1/POPStarter preparation tool for converting BIN/CUE backups to VCDs and installing them to USB, MX4SIO, MMCE, exFAT HDD, SMB and APA internal HDD: https://github.com/shaanhomebrew-cloud/OPL-PS1-AIO-Converter-GUI\n- **PS2RD CHT Manager** — Windows application for creating, editing and managing `.cht` cheat files for **Open PS2 Loader (OPL) / PS2RD**: https://github.com/TheRealNextria/PS2RD-CHT-Manager\n'''

for marker in ('## Other downloads', '## External Tools & Services'):
    if marker in body:
        body = body.split(marker, 1)[0].rstrip()
body = re.sub(r'\nSHA256 \(also published as SHA256SUMS\.txt\):\n```.*?```\n', '\n', body, flags=re.DOTALL)
body = body.lstrip()

downloads = '''## Release downloads\n\n- **Unified package:** the normal installable RiptOPL package containing the standard loaders, PS1/POPSTARTER files, bundled Neutrino and the four external-tool shortcuts.\n- **Variants:** alternate build configurations, including the DualSense (DS5) loaders.\n- **Debug:** diagnostic builds for troubleshooting.\n- **Languages:** additional UI language files and fonts.\n- **Source:** the exact source snapshot used to produce the release.\n\nNo bare ELF files or SDK/IRX manifests are published as release assets; they are kept inside the appropriate archives where needed.'''

print(f'{banner}\n\n{body}\n\n{downloads}\n\n{resources}', end='')
