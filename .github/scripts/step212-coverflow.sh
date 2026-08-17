#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "684826ff730ee537d3b9429f92ce5ec13d948696"

python3 - <<'PY'
from pathlib import Path
p = Path('src/favsupport.c')
text = p.read_text()
old = '''        for (int i = 0; i < favCount; i++) {\n            item_list_t ownerView;\n        item_list_t *o = favOwnerView(i, &ownerView);\n            if (o == NULL || o->itemGetImage == NULL || !favOwnerHasId(o, favArray[i].id))\n'''
new = '''        for (int i = 0; i < favCount; i++) {\n            item_list_t ownerView;\n            item_list_t *o = favOwnerView(i, &ownerView);\n            if (o == NULL || o->itemGetImage == NULL || !favOwnerHasId(o, favArray[i].id))\n'''
if text.count(old) != 1:
    raise SystemExit('favsupport clang-format target did not match exactly once')
p.write_text(text.replace(old, new, 1))
PY

git diff --check -- src/favsupport.c
git add src/favsupport.c
git commit -m "rebuild-212: fix favourites formatting"
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
