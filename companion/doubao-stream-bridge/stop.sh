#!/bin/zsh
set -eu
cd "${0:A:h}"
python3 - <<'PY'
import pathlib, json, uuid
root = pathlib.Path('runtime')
if root.is_dir():
    temporary = root / 'control.tmp'
    temporary.write_text(json.dumps({'id': str(uuid.uuid4()), 'action': 'quit'}))
    temporary.replace(root / 'control.json')
PY
