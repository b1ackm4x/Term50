#!/usr/bin/env python3
"""Preserve the verified CJK/OMZ BAR and produce a separate voice candidate."""
import argparse
import base64
import copy
import hashlib
from pathlib import Path
import re
import zipfile
import json

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base', type=Path, default=ROOT/'dist/Term50-cjk-oh-my-zsh-bbtailscale.bar')
parser.add_argument('--output', type=Path, default=ROOT/'dist/Term50-voice-preview.bar')
args = parser.parse_args()
if args.base.resolve() == args.output.resolve():
    parser.error('Output must not replace rollback BAR')

def digest(data):
    return base64.urlsafe_b64encode(hashlib.sha512(data).digest()).decode().rstrip('=')

changes = {'native/Term50': (ROOT/'Device-Debug/Term50').read_bytes()}
assert changes['native/Term50'].startswith(b'\x7fELF\x01\x01'), 'Expected ARM ELF32 binary'
assert b'libssl.so.3\0' in changes['native/Term50'] and b'libssl.so.2\0' not in changes['native/Term50'], 'Rebuild with voice OpenSSL 3 headers and runtime'
pins=json.loads((ROOT/'vendor/voice-openssl/pins.json').read_text())
with zipfile.ZipFile(args.base) as old:
    for lib in ('libssl.so.3', 'libcrypto.so.3'):
        changes['native/lib/'+lib] = old.read('native/bbnix/lib/'+lib)
        assert hashlib.sha256(changes['native/lib/'+lib]).hexdigest()==pins['runtime_sha256'][lib], 'Runtime does not match build headers: '+lib
    changes['native/voice-openssl-LICENSE.txt']=(ROOT/'vendor/voice-openssl/LICENSE.txt').read_bytes()
    manifest = old.read('META-INF/MANIFEST.MF').decode()
    assert 'LD_LIBRARY_PATH=app/native/lib' in manifest
    assert 'record_audio' in manifest
    for name, data in changes.items():
        pattern = r'(Archive-Asset-Name: '+re.escape(name)+r'\nArchive-Asset-SHA-512-Digest: )[^\n]+'
        if re.search(pattern, manifest):
            manifest = re.sub(pattern, lambda m: m[1]+digest(data), manifest)
        else:
            manifest += '\nArchive-Asset-Name: '+name+'\nArchive-Asset-SHA-512-Digest: '+digest(data)+'\n\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED) as new:
        for info in old.infolist():
            data = manifest.encode() if info.filename == 'META-INF/MANIFEST.MF' else changes.get(info.filename,old.read(info.filename))
            new.writestr(copy.copy(info), data)
        for name, data in changes.items():
            if name not in old.namelist():
                info=zipfile.ZipInfo(name);info.external_attr=0o100755<<16;info.compress_type=zipfile.ZIP_DEFLATED
                new.writestr(info,data)
    with zipfile.ZipFile(args.output) as new:
        assert new.testzip() is None
        for name in old.namelist():
            if name not in changes and name != 'META-INF/MANIFEST.MF':
                assert new.read(name) == old.read(name), name
        for block in manifest.split('\n\n'):
            name=re.search(r'^Archive-Asset-Name: (.+)$',block,re.M)
            value=re.search(r'^Archive-Asset-SHA-512-Digest: (.+)$',block,re.M)
            if name and value:
                assert digest(new.read(name[1])) == value[1], name[1]
        assert not any('modal-test' in n or 'pairing.key' in n for n in new.namelist())
print(json.dumps({'bar':str(args.output), 'sha256':hashlib.sha256(args.output.read_bytes()).hexdigest(),
                  'native_sha256':hashlib.sha256(changes['native/Term50']).hexdigest(),
                  'changed_assets':sorted(changes),'all_asset_digests_verified':True,
                  'existing_configuration_preserved':True},indent=2))
