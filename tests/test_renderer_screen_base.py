#!/usr/bin/env python3
"""Compile the full native renderer against a minimal host Screen mock."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    cc = shlex.split(os.environ.get("TEST_CC", "cc"))
    flags = shlex.split(os.environ.get("TEST_SANITIZERS", "-fsanitize=address,undefined -fno-omit-frame-pointer"))
    icu = shlex.split(os.environ.get("TEST_ICU_CFLAGS", ""))
    if not icu and Path("/opt/homebrew/opt/icu4c/include").is_dir():
        icu = ["-I/opt/homebrew/opt/icu4c/include"]
    common = cc + ["-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror=implicit-function-declaration"] + flags + icu + ["-Itests/compat", "-Isrc"]
    with tempfile.TemporaryDirectory(prefix="term50-screen-base-") as folder:
        object_ = Path(folder) / "screen.o"
        binary = Path(folder) / "screen-base"
        subprocess.run(common + ["-Dbitmap_alloc=test_bitmap_alloc", "-Dbitmap_free=test_bitmap_free", "-c", "src/renderer_screen.c", "-o", str(object_)], cwd=ROOT, check=True)
        subprocess.run(common + ["tests/test_renderer_screen_base.c", "src/renderer.c", "src/bitmap.c", str(object_), "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
