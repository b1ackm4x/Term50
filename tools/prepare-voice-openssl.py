#!/usr/bin/env python3
"""Generate matching ARM/QNX OpenSSL headers and stage pinned runtime libraries.

Run inside the BerryCore Linux container with its cross-toolchain environment.
Only public headers are generated; no OpenSSL library is rebuilt, and host
OpenSSL headers/libraries are never used. Downloads fail closed on hash drift.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor/voice-openssl"
PINS = VENDOR / "pins.json"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def headers_valid(pin_digest):
    try:
        if not (VENDOR / "LICENSE.txt").is_file():
            return False
        manifest = json.loads((VENDOR / "headers-manifest.json").read_text())
        headers = manifest["headers"]
        return (manifest["pins_sha256"] == pin_digest and headers and
                all(sha256(VENDOR / "include" / name) == digest
                    for name, digest in headers.items()))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def prepare_headers(pins, pin_digest):
    if headers_valid(pin_digest):
        return
    try:
        subprocess.run(["perl", "-MFindBin", "-MFile::Spec", "-MIPC::Cmd", "-e", "1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("Header generation needs full Perl (install the perl package, not only perl-base)") from exc
    cache = VENDOR / ".cache"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ("openssl-" + pins["version"] + ".tar.gz")
    if not archive.is_file():
        download = archive.with_suffix(".download")
        try:
            with urllib.request.urlopen(pins["source_url"], timeout=90) as response:
                with download.open("wb") as target:
                    shutil.copyfileobj(response, target)
            if sha256(download) != pins["source_sha256"]:
                raise RuntimeError("OpenSSL source archive failed pinned SHA-256 verification")
            download.replace(archive)
        finally:
            download.unlink(missing_ok=True)
    if sha256(archive) != pins["source_sha256"]:
        raise RuntimeError("Cached OpenSSL source archive failed pinned SHA-256 verification")
    with tempfile.TemporaryDirectory(prefix="headers-", dir=cache) as work:
        work = Path(work)
        with tarfile.open(archive, "r:gz") as source:
            for member in source.getmembers():
                destination = (work / member.name).resolve()
                if (work.resolve() not in destination.parents or
                        not (member.isfile() or member.isdir())):
                    raise RuntimeError("Unexpected path or link in OpenSSL archive")
            source.extractall(work)
        source = work / ("openssl-" + pins["version"])
        env = os.environ.copy()
        env.update(CC="arm-blackberry-qnx8eabi-gcc",
                   AR="arm-blackberry-qnx8eabi-ar",
                   RANLIB="arm-blackberry-qnx8eabi-ranlib")
        # Configure's target supplies 32-bit integer/BN settings. It never
        # guesses macOS/Linux host ABI settings for these headers.
        subprocess.run(["perl", "./Configure", *pins["configure"]],
                       cwd=source, env=env, check=True)
        targets = sorted(str(path.relative_to(source))[:-3]
                         for path in (source / "include/openssl").glob("*.h.in"))
        subprocess.run(["make", "-j4", *targets], cwd=source, env=env, check=True)
        stage = work / "staged"
        (stage / "openssl").mkdir(parents=True)
        for header in (source / "include/openssl").glob("*.h"):
            shutil.copyfile(header, stage / "openssl" / header.name)
        required = ["ssl.h", "crypto.h", "opensslv.h", "configuration.h", "opensslconf.h"]
        if any(not (stage / "openssl" / name).is_file() for name in required):
            raise RuntimeError("OpenSSL public header generation was incomplete")
        manifest = dict(pins_sha256=pin_digest, headers={
            str(path.relative_to(stage)): sha256(path)
            for path in sorted(stage.rglob("*.h"))})
        includes = VENDOR / "include"
        if includes.exists():
            shutil.rmtree(includes)
        stage.replace(includes)
        (VENDOR / "headers-manifest.json").write_text(
            json.dumps(manifest, sort_keys=True, indent=2) + "\n")
        shutil.copyfile(source / "LICENSE.txt", VENDOR / "LICENSE.txt")


def stage_runtime(pins, runtime):
    libraries = VENDOR / "lib"
    libraries.mkdir(exist_ok=True)
    for name, expected in pins["runtime_sha256"].items():
        source = runtime / name
        if not source.is_file() or sha256(source) != expected:
            raise RuntimeError("Missing or modified pinned ARM/QNX runtime: " + str(source))
        with source.open("rb") as binary:
            elf = binary.read(20)
        if (elf[:6] != b"\x7fELF\x01\x01" or len(elf) != 20 or
                struct.unpack("<H", elf[18:20])[0] != 40):
            raise RuntimeError("Runtime is not little-endian ELF32 ARM: " + name)
        destination = libraries / name
        if not destination.is_file() or sha256(destination) != expected:
            shutil.copyfile(source, destination)
            destination.chmod(0o755)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", type=Path, required=True,
                        help="directory containing the pinned ARM/QNX OpenSSL runtime")
    args = parser.parse_args()
    pins = json.loads(PINS.read_text())
    stage_runtime(pins, args.runtime_dir.resolve())
    prepare_headers(pins, sha256(PINS))
    print("OpenSSL " + pins["version"] + ": verified ARM/QNX runtime and matching headers ready")


if __name__ == "__main__":
    main()
