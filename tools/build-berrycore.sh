#!/bin/sh
# Alternative to the upstream qcc build, inside a Linux x86_64 container.
# Requires the BerryCore GCC9 toolchain mounted/symlinked at /root/qnx800,
# its env.sh sourced, and the pinned Ghostty library built first.
set -eu
cd "$(dirname "$0")/.."
: "${QNX_TARGET:?Source /root/qnx800/env.sh first}"
cc=${CC:-arm-blackberry-qnx8eabi-gcc}
libdir="$QNX_TARGET/arm-blackberry-qnx8eabi"
ghostty=vendor/libghostty-vt/build/ghostty
voice_openssl=vendor/voice-openssl
if [ ! -f "$ghostty/lib/libghostty-vt.a" ] || [ ! -f "$ghostty/include/ghostty/vt.h" ]; then
    echo 'Build the pinned Ghostty dependency first; see docs/modal-input-validation.md.' >&2
    exit 1
fi
: "${TERM50_VOICE_OPENSSL_RUNTIME_DIR:?Set TERM50_VOICE_OPENSSL_RUNTIME_DIR to the pinned ARM/QNX OpenSSL runtime directory}"
python3 tools/prepare-voice-openssl.py --runtime-dir "$TERM50_VOICE_OPENSSL_RUNTIME_DIR"
# This toolchain's stdint.h exposes its limit macros only in C11 mode.
# App sources still use C99; the upstream qcc flags are unchanged.
exec make -j"${BUILD_JOBS:-4}" all \
    CC="$cc" LUA_AR=arm-blackberry-qnx8eabi-ar \
    CFLAGS="-std=gnu11 -O2 -I$voice_openssl/include -I$QNX_TARGET/include -I$QNX_TARGET/include/freetype2 -Ivendor/lua -Ivendor/qrcodegen -I$ghostty/include" \
    LIBPATHS="-L$libdir/lib -L$libdir/usr/lib -Wl,-rpath-link,$voice_openssl/lib -Wl,-rpath-link,$libdir/usr/lib" "$@"
