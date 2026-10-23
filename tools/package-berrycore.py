#!/usr/bin/env python3
"""Package local builds with hash-pinned, previously validated BB10 runtime assets.

Runs on the macOS host. Does not require Nix or change the source asset trees.
The selected base must be one of the existing v1.1.0 production bundles below.
Only bbnix, fonts and Fen are reused; Term50, termctl and docs come from this tree.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASES = {
    '8a7116fbbac0028bb8dc6bacee61b8a6099d9157da9f5252740f6a747842ce37':
        'Term50 v1.1.0 upstream release',
    '4bf58bd3c3744570dd220b04e00679a707ff7eb031eb0745fab4e281302514ff':
        'Term50 v1.1.0 with validated SSH compatibility fix',
    '6d2d04e05b2bf9fa2931e31c82d8e08f44cfdd3105cad5b130b7afb379d1634a':
        'Term50 v1.1.0 with SSH compatibility fix and existing bbtailscale addon',
}
NS = 'http://www.qnx.com/schemas/application/1.0'
Q = '{' + NS + '}'
ET.register_namespace('', NS)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def check_manifest(archive):
    """Validate every asset's digest, including reused third-party payloads."""
    manifest = archive.read('META-INF/MANIFEST.MF').decode().replace('\r\n', '\n')
    manifest = manifest.replace('\n ', '')
    digests = {}
    for section in manifest.split('\n\n'):
        fields = dict(line.split(': ', 1) for line in section.splitlines() if ': ' in line)
        if 'Archive-Asset-Name' in fields:
            name = fields['Archive-Asset-Name']
            expected = fields['Archive-Asset-SHA-512-Digest'].rstrip('=')
            actual = base64.urlsafe_b64encode(hashlib.sha512(archive.read(name)).digest()).decode().rstrip('=')
            if actual != expected:
                raise ValueError('Asset digest mismatch: ' + name)
            digests[name] = expected
    payload = {n for n in archive.namelist() if not n.endswith('/') and not n.startswith('META-INF/')}
    if payload != set(digests):
        raise ValueError('Manifest and payload asset lists differ')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-bar', type=Path, required=True)
    parser.add_argument('--container', help='Existing Linux build container (omit inside container)')
    parser.add_argument('--container-root', default='/workspace/q-term/Term50')
    parser.add_argument('--sdk-env', default='/root/qnx800/env.sh')
    parser.add_argument('--output', default='Term50-modal-input.bar')
    args = parser.parse_args()
    if Path(args.output).name != args.output or not args.output.endswith('.bar'):
        parser.error('--output must be a .bar filename')
    base_hash = sha256(args.base_bar.read_bytes())
    if base_hash not in BASES:
        parser.error('Unrecognized base BAR hash; review and pin its provenance first')
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    output = dist / args.output
    container_root = PurePosixPath(args.container_root)

    if args.container:
        # Copy the opaque ZIP into the mount, never extract case-sensitive
        # terminfo names onto the usually case-insensitive macOS filesystem.
        with tempfile.TemporaryDirectory(prefix='.base-', dir=dist) as tmp:
            base = Path(tmp) / args.base_bar.name
            shutil.copyfile(args.base_bar, base)
            remote_base = container_root / base.relative_to(ROOT).as_posix()
            command = ['python3', str(container_root / 'tools/package-berrycore.py'),
                       '--base-bar', str(remote_base), '--output', args.output,
                       '--sdk-env', args.sdk_env]
            subprocess.run(['docker', 'exec', args.container] + command, check=True)
        return

    # Must be Linux-local /tmp, NOT the macOS bind mount: terminfo includes
    # pairs such as 2621a/2621A that collide on default APFS volumes.
    with tempfile.TemporaryDirectory(prefix='term50-package-') as tmp:
        stage = Path(tmp)
        reused = {}
        with zipfile.ZipFile(args.base_bar) as base:
            check_manifest(base)
            for info in base.infolist():
                name = info.filename
                if not (name.startswith(('native/bbnix/', 'native/fonts/')) or name == 'native/root/bin/fen'):
                    continue
                rel = PurePosixPath(name)
                if rel.is_absolute() or '..' in rel.parts:
                    raise ValueError('Unsafe archive path')
                if info.is_dir():
                    continue
                data = base.read(name)
                target = stage.joinpath(*rel.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                target.chmod((info.external_attr >> 16) & 0o777 or 0o644)
                reused[name] = sha256(data)
        for name in ['native/bbnix/bin/zsh', 'native/bbnix/bin/ssh', 'native/bbnix/etc/bbnix-env',
                     'native/bbnix/etc/busybox.applets', 'native/fonts/JetBrainsMono-Regular.ttf',
                     'native/root/bin/fen']:
            if name not in reused:
                raise ValueError('Missing runtime asset: ' + name)
        descriptor = ET.parse(ROOT / 'bar-descriptor.xml')
        root = descriptor.getroot()
        for config in list(root.findall(Q + 'configuration')):
            if config.get('name') != 'Device-Debug':
                root.remove(config)
        source_assets = {}
        for asset in root.iter(Q + 'asset'):
            source = ROOT / asset.get('path')
            dest = asset.text
            if dest in ('bbnix', 'fonts', 'root/bin/fen'):
                source = stage / 'native' / dest
            elif source.is_file():
                source_assets['native/' + dest] = sha256(source.read_bytes())
            if not source.exists():
                raise ValueError('Missing source asset: ' + str(source))
            asset.set('path', str(source))
        descriptor_path = stage / 'bar-descriptor.xml'
        descriptor.write(descriptor_path, encoding='utf-8', xml_declaration=True)
        package_path = stage / args.output
        command = ['blackberry-nativepackager', '-package', str(package_path),
                   str(descriptor_path), '-configuration', 'Device-Debug']
        shell = 'source ' + shlex.quote(args.sdk_env) + ' && exec ' + shlex.join(command)
        subprocess.run(['bash', '-lc', shell], cwd=ROOT, check=True)
        with zipfile.ZipFile(package_path) as package:
            if package.testzip() is not None:
                raise ValueError('BAR ZIP integrity failure')
            manifest = check_manifest(package)
            for key in ['Package-Name: io.github.term50', 'Package-Id: testRel_thub_term50b96c21aa',
                        'Application-Development-Mode: false']:
                if key + '\n' not in manifest:
                    raise ValueError('Application identity/development mode changed: ' + key)
            for name, digest in (reused | source_assets).items():
                if sha256(package.read(name)) != digest:
                    raise ValueError('Packaged asset differs: ' + name)
            asset_count = len(package.namelist())
        # Copy across filesystems, then atomically replace the published BAR.
        pending = output.with_suffix('.bar.pending')
        try:
            shutil.copyfile(package_path, pending)
            os.replace(pending, output)
        finally:
            pending.unlink(missing_ok=True)
    report = {'base_name': args.base_bar.name, 'base_description': BASES[base_hash],
              'base_sha256': base_hash, 'reused_runtime_assets': len(reused),
              'source_assets': source_assets, 'output': output.name,
              'bytes': output.stat().st_size, 'sha256': sha256(output.read_bytes()),
              'archive_entries': asset_count, 'all_manifest_digests_verified': True,
              'new_binaries_and_docs_verified': True, 'reused_runtime_bytes_verified': True,
              'unsigned': True, 'device_install_tested_by_this_script': False}
    output.with_suffix('.validation.json').write_text(json.dumps(report, indent=2) + '\n')
    output.with_suffix('.sha256').write_text(report['sha256'] + '  ' + output.name + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
